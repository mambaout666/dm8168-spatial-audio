#include "spatial_core.h"

#include <limits.h>
#include <string.h>

static int mode_valid(spatial_mode mode)
{
    return mode.direction < SPATIAL_DIRECTIONS &&
           (mode.bypass == 0 || mode.bypass == 1);
}

static int modes_equal(spatial_mode first, spatial_mode second)
{
    return first.direction == second.direction && first.bypass == second.bypass;
}

static int16_t rounded_q15(int64_t value)
{
    int64_t rounded;
    if (value >= 0) {
        rounded = (value + INT64_C(16384)) / INT64_C(32768);
    } else {
        rounded = -((-value + INT64_C(16384)) / INT64_C(32768));
    }
    if (rounded > INT16_MAX) {
        return INT16_MAX;
    }
    if (rounded < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)rounded;
}

void spatial_bank_demo(spatial_bank *bank)
{
    if (bank == NULL) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    bank->sample_rate = SPATIAL_DEFAULT_RATE;
    bank->taps = 512;
    bank->synthetic = 1;
    /* Educational, symmetric interaural delay/gain cues. Not measured HRIR. */
    bank->coeffs[0][0][0] = 19661;
    bank->coeffs[0][0][24] = 3277;
    bank->coeffs[0][1][12] = 13107;
    bank->coeffs[0][1][36] = 1638;
    memcpy(bank->coeffs[1][0], bank->coeffs[0][1], sizeof(bank->coeffs[1][0]));
    memcpy(bank->coeffs[1][1], bank->coeffs[0][0], sizeof(bank->coeffs[1][1]));
}

int spatial_bank_validate(const spatial_bank *bank)
{
    if (bank == NULL || bank->sample_rate == 0 || bank->taps == 0 ||
        bank->taps > SPATIAL_MAX_TAPS ||
        (bank->synthetic != 0 && bank->synthetic != 1)) {
        return SPATIAL_EINVAL;
    }
    return SPATIAL_OK;
}

int spatial_init(spatial_processor *processor, const spatial_bank *bank,
                 spatial_mode mode, size_t fade_frames)
{
    if (processor == NULL || spatial_bank_validate(bank) != SPATIAL_OK ||
        !mode_valid(mode) || fade_frames > UINT32_MAX) {
        return SPATIAL_EINVAL;
    }
    memset(processor, 0, sizeof(*processor));
    processor->bank = bank;
    processor->current = mode;
    processor->target = mode;
    processor->pending = mode;
    processor->fade_frames = fade_frames;
    return SPATIAL_OK;
}

static void start_transition(spatial_processor *processor, spatial_mode mode)
{
    processor->target = mode;
    processor->fade_position = 0;
    if (processor->fade_frames < 2 || modes_equal(processor->current, mode)) {
        processor->current = mode;
        processor->transitioning = 0;
    } else {
        processor->transitioning = 1;
    }
}

static int processor_valid(const spatial_processor *processor)
{
    return processor != NULL &&
           spatial_bank_validate(processor->bank) == SPATIAL_OK &&
           processor->write_index < processor->bank->taps &&
           processor->fade_frames <= UINT32_MAX &&
           mode_valid(processor->current) && mode_valid(processor->target) &&
           mode_valid(processor->pending) &&
           (processor->transitioning == 0 || processor->transitioning == 1) &&
           (processor->pending_valid == 0 || processor->pending_valid == 1) &&
           (!processor->transitioning ||
            (processor->fade_frames >= 2 &&
             processor->fade_position < processor->fade_frames));
}

int spatial_set_mode(spatial_processor *processor, spatial_mode mode)
{
    if (!processor_valid(processor) || !mode_valid(mode)) {
        return SPATIAL_EINVAL;
    }
    if (processor->transitioning) {
        processor->pending = mode;
        processor->pending_valid = !modes_equal(mode, processor->target);
    } else {
        processor->pending_valid = 0;
        start_transition(processor, mode);
    }
    return SPATIAL_OK;
}

static void render_mode(const spatial_processor *processor, spatial_mode mode,
                        int16_t mono, int16_t result[SPATIAL_EARS])
{
    size_t tap;
    size_t history_index;
    int64_t left = 0;
    int64_t right = 0;
    const int16_t *coeff_left;
    const int16_t *coeff_right;
    if (mode.bypass) {
        result[0] = mono;
        result[1] = mono;
        return;
    }
    coeff_left = processor->bank->coeffs[mode.direction][0];
    coeff_right = processor->bank->coeffs[mode.direction][1];
    history_index = processor->write_index;
    for (tap = 0; tap < processor->bank->taps; ++tap) {
        const int32_t sample = processor->history[history_index];
        left += (int64_t)sample * coeff_left[tap];
        right += (int64_t)sample * coeff_right[tap];
        history_index = history_index == 0 ? processor->bank->taps - 1
                                          : history_index - 1;
    }
    result[0] = rounded_q15(left);
    result[1] = rounded_q15(right);
}

int spatial_process(spatial_processor *processor,
                    const int16_t *input_interleaved,
                    int16_t *output_interleaved, size_t frames)
{
    size_t frame;
    if (!processor_valid(processor) ||
        (frames != 0 && (input_interleaved == NULL || output_interleaved == NULL)) ||
        frames > SIZE_MAX / (2 * sizeof(int16_t))) {
        return SPATIAL_EINVAL;
    }
    for (frame = 0; frame < frames; ++frame) {
        int16_t old_result[SPATIAL_EARS];
        const int32_t source_sum = (int32_t)input_interleaved[2 * frame] +
                                   (int32_t)input_interleaved[2 * frame + 1];
        const int16_t mono = (int16_t)(source_sum / 2);
        processor->history[processor->write_index] = mono;
        render_mode(processor, processor->current, mono, old_result);
        if (processor->transitioning) {
            unsigned ear;
            int16_t new_result[SPATIAL_EARS];
            const int64_t alpha = (int64_t)(
                (uint64_t)processor->fade_position * UINT64_C(32768) /
                (uint64_t)(processor->fade_frames - 1));
            render_mode(processor, processor->target, mono, new_result);
            for (ear = 0; ear < SPATIAL_EARS; ++ear) {
                output_interleaved[2 * frame + ear] = rounded_q15(
                    (INT64_C(32768) - alpha) * old_result[ear] +
                    alpha * new_result[ear]);
            }
            ++processor->fade_position;
            if (processor->fade_position == processor->fade_frames) {
                processor->current = processor->target;
                processor->transitioning = 0;
                if (processor->pending_valid) {
                    const spatial_mode pending = processor->pending;
                    processor->pending_valid = 0;
                    start_transition(processor, pending);
                }
            }
        } else {
            output_interleaved[2 * frame] = old_result[0];
            output_interleaved[2 * frame + 1] = old_result[1];
        }
        ++processor->write_index;
        if (processor->write_index == processor->bank->taps) {
            processor->write_index = 0;
        }
    }
    return SPATIAL_OK;
}
