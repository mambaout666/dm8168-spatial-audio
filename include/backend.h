#ifndef SPATIAL_BACKEND_H
#define SPATIAL_BACKEND_H
#include "spatial_rpe_plugin.h"
typedef struct spatial_backend {
    spatial_processor cpu;
    void *library;
    void *session;
    const spatial_rpe_api *api;
    const char *name;
} spatial_backend;
int backend_open(spatial_backend *, const char *name, const char *plugin,
                 const spatial_bank *, spatial_mode, size_t, char *, size_t);
int backend_mode(spatial_backend *, spatial_mode, char *, size_t);
int backend_process(spatial_backend *, const int16_t *, int16_t *, size_t, char *, size_t);
void backend_close(spatial_backend *);
#endif
