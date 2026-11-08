#ifndef SPATIAL_CORE_H
#define SPATIAL_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPATIAL_DIRECTIONS 2
#define SPATIAL_EARS 2
#define SPATIAL_MAX_TAPS 1024
#define SPATIAL_BLOCK_FRAMES 1024
#define SPATIAL_DEFAULT_RATE 48000

/* Values are Q15. coeffs[d][ear][0] multiplies the current mono sample.
 * A bank must remain alive and unchanged while a processor refers to it.
 * synthetic=1 marks an educational delay/gain bank, NOT measured HRIR data.
 */
typedef struct spatial_bank {
    uint32_t sample_rate;
    size_t taps;
    int synthetic;
    int16_t coeffs[SPATIAL_DIRECTIONS][SPATIAL_EARS][SPATIAL_MAX_TAPS];
} spatial_bank;

typedef struct spatial_mode {
    unsigned direction;
    int bypass;
} spatial_mode;

/* Public for static/stack allocation on ARM and DSP; use the API to mutate it.
 * One processor owns one continuous stream and is not thread-safe.
 */
typedef struct spatial_processor {
    const spatial_bank *bank;
    int16_t history[SPATIAL_MAX_TAPS];
    size_t write_index;
    spatial_mode current;
    spatial_mode target;
    spatial_mode pending;
    size_t fade_frames;
    size_t fade_position;
    int transitioning;
    int pending_valid;
} spatial_processor;

enum {
    SPATIAL_OK = 0,
    SPATIAL_EINVAL = -1
};

void spatial_bank_demo(spatial_bank *bank);
int spatial_bank_validate(const spatial_bank *bank);

/* fade_frames: 0 or 1 switches immediately; >=2 includes both endpoints.
 * Values above UINT32_MAX are rejected to keep fade arithmetic bounded.
 */
int spatial_init(spatial_processor *processor, const spatial_bank *bank,
                 spatial_mode mode, size_t fade_frames);

/* A request during a fade replaces the pending request; the active fade is
 * completed first. Repeating the active target cancels an obsolete pending
 * request. Call at block boundaries in an RPE adapter.
 */
int spatial_set_mode(spatial_processor *processor, spatial_mode mode);

/* Stereo S16 input is averaged to one source using a wide sum and division
 * toward zero. Bypass duplicates that mono source to both ears. FIR and fade
 * results round Q15 halfway values away from zero and saturate to int16.
 * Histories survive calls, including bypass, short blocks and mode changes.
 * Exact in-place operation is supported; other overlapping buffers are not.
 * frames=0 accepts NULL audio pointers. No heap allocation is performed.
 */
int spatial_process(spatial_processor *processor,
                    const int16_t *input_interleaved,
                    int16_t *output_interleaved, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
