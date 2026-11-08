#include "spatial_dsp.h"

int spatial_dsp_init(spatial_dsp_context *context, const spatial_bank *bank, size_t fade)
{
    const spatial_mode initial = {0, 0};
    if (!context) return SPATIAL_EINVAL;
    if (spatial_init(&context->processor, bank, initial, fade) != 0) return SPATIAL_EINVAL;
    context->bank = bank;
    context->fade_frames = fade;
    return 0;
}
int spatial_dsp_configure(spatial_dsp_context *context, const spatial_dsp_control *control)
{
    spatial_mode mode;
    if (!context || !control || control->version != SPATIAL_DSP_CONTROL_VERSION ||
        control->direction >= SPATIAL_DIRECTIONS || control->bypass > 1 ||
        control->reset_history > 1) return SPATIAL_EINVAL;
    mode.direction = control->direction;
    mode.bypass = (int)control->bypass;
    if (control->reset_history)
        return spatial_init(&context->processor, context->bank, mode, context->fade_frames);
    return spatial_set_mode(&context->processor, mode);
}
int spatial_dsp_process(spatial_dsp_context *context, const int16_t *input,
                        int16_t *output, size_t frames)
{
    if (!context || frames > SPATIAL_BLOCK_FRAMES) return SPATIAL_EINVAL;
    return spatial_process(&context->processor, input, output, frames);
}
