#ifndef SPATIAL_RPE_PLUGIN_H
#define SPATIAL_RPE_PLUGIN_H
#include "spatial_core.h"

/* Host-side plugin ABI. This struct is NOT an ARM-to-DSP wire format.
 * The plugin owns all course-specific RPE handles, shared allocations and
 * cache operations. process() is synchronous and returns a full frame block.
 */
#define SPATIAL_RPE_PLUGIN_ABI 1
typedef struct spatial_rpe_api {
    uint32_t abi_version;
    const char *name;
    void *(*open)(const spatial_bank *, size_t fade_frames, spatial_mode,
                  char *error, size_t error_size);
    int (*set_mode)(void *, spatial_mode, char *, size_t);
    int (*process)(void *, const int16_t *, int16_t *, size_t, char *, size_t);
    void (*close)(void *);
} spatial_rpe_api;
typedef const spatial_rpe_api *(*spatial_rpe_get_api_fn)(void);
#endif
