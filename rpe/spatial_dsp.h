#ifndef SPATIAL_DSP_H
#define SPATIAL_DSP_H
#include "spatial_core.h"

/* Fixed-width values for a course-specific Rpe_control adapter. No pointers,
 * 16-bit block counters, or overloaded extendedError status fields are sent.
 * This descriptor is copied by the RPE binding, not cast across shared memory.
 */
#define SPATIAL_DSP_CONTROL_VERSION 1
typedef struct spatial_dsp_control {
    uint32_t version;
    uint32_t direction;
    uint32_t bypass;
    uint32_t reset_history;
} spatial_dsp_control;
typedef struct spatial_dsp_context {
    spatial_processor processor;
    const spatial_bank *bank;
    size_t fade_frames;
} spatial_dsp_context;
int spatial_dsp_init(spatial_dsp_context *, const spatial_bank *, size_t);
int spatial_dsp_configure(spatial_dsp_context *, const spatial_dsp_control *);
int spatial_dsp_process(spatial_dsp_context *, const int16_t *, int16_t *, size_t);
#endif
