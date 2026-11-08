#include "backend.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

int backend_open(spatial_backend *backend, const char *name, const char *plugin,
                 const spatial_bank *bank, spatial_mode mode, size_t fade,
                 char *error, size_t size)
{
    memset(backend, 0, sizeof(*backend));
    backend->name = name;
    if (!strcmp(name, "cpu")) {
        return spatial_init(&backend->cpu, bank, mode, fade);
    }
    if (strcmp(name, "rpe")) {
        snprintf(error, size, "Backend must be cpu or rpe");
        return -1;
    }
#ifdef _WIN32
    (void)plugin;
    snprintf(error, size, "RPE backend requires target Linux and the course RPE plugin");
    return -1;
#else
    {
        spatial_rpe_get_api_fn get_api;
        void *symbol;
        if (!plugin || !*plugin) {
            snprintf(error, size, "--backend rpe requires --rpe-plugin PATH; CPU fallback is disabled");
            return -1;
        }
        backend->library = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        if (!backend->library) {
            snprintf(error, size, "Cannot load RPE plugin: %s", dlerror());
            return -1;
        }
        symbol = dlsym(backend->library, "spatial_rpe_get_api");
        /* POSIX specifies dlsym conversion; memcpy avoids ISO C cast warnings. */
        if (!symbol || sizeof(symbol) != sizeof(get_api)) {
            snprintf(error, size, "Missing spatial_rpe_get_api symbol");
            backend_close(backend);
            return -1;
        }
        memcpy(&get_api, &symbol, sizeof(get_api));
        backend->api = get_api();
        if (!backend->api || backend->api->abi_version != SPATIAL_RPE_PLUGIN_ABI ||
            !backend->api->open || !backend->api->set_mode ||
            !backend->api->process || !backend->api->close) {
            snprintf(error, size, "Invalid RPE plugin ABI");
            backend_close(backend);
            return -1;
        }
        backend->session = backend->api->open(bank, fade, mode, error, size);
        if (!backend->session) {
            backend_close(backend);
            return -1;
        }
        return 0;
    }
#endif
}

int backend_mode(spatial_backend *backend, spatial_mode mode, char *error, size_t size)
{
    if (backend->api) return backend->api->set_mode(backend->session, mode, error, size);
    return spatial_set_mode(&backend->cpu, mode);
}
int backend_process(spatial_backend *backend, const int16_t *in, int16_t *out,
                    size_t frames, char *error, size_t size)
{
    if (backend->api) return backend->api->process(backend->session, in, out, frames, error, size);
    return spatial_process(&backend->cpu, in, out, frames);
}
void backend_close(spatial_backend *backend)
{
    if (backend->session && backend->api) backend->api->close(backend->session);
#ifndef _WIN32
    if (backend->library) dlclose(backend->library);
#endif
    backend->session = NULL;
    backend->library = NULL;
    backend->api = NULL;
}
