#include "spatial_dsp.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (0)
int main(void)
{
    spatial_bank bank;
    spatial_dsp_context dsp;
    spatial_processor reference;
    spatial_mode mode = {0, 0};
    spatial_dsp_control control = {SPATIAL_DSP_CONTROL_VERSION, 0, 0, 0};
    int16_t input[SPATIAL_BLOCK_FRAMES * 2];
    int16_t output[SPATIAL_BLOCK_FRAMES * 2];
    int16_t expected[SPATIAL_BLOCK_FRAMES * 2];
    unsigned i;
    spatial_bank_demo(&bank);
    CHECK(spatial_dsp_init(&dsp, &bank, SPATIAL_BLOCK_FRAMES) == 0);
    CHECK(spatial_init(&reference, &bank, mode, SPATIAL_BLOCK_FRAMES) == 0);
    for (i = 0; i < SPATIAL_BLOCK_FRAMES * 2; ++i) input[i] = (int16_t)(i * 13 % 10000 - 5000);
    CHECK(spatial_dsp_process(&dsp, input, output, SPATIAL_BLOCK_FRAMES) == 0);
    CHECK(spatial_process(&reference, input, expected, SPATIAL_BLOCK_FRAMES) == 0);
    CHECK(!memcmp(output, expected, sizeof(output)));
    control.direction = 1;
    CHECK(spatial_dsp_configure(&dsp, &control) == 0);
    mode.direction = 1;
    CHECK(spatial_set_mode(&reference, mode) == 0);
    CHECK(spatial_dsp_process(&dsp, input, output, SPATIAL_BLOCK_FRAMES) == 0);
    CHECK(spatial_process(&reference, input, expected, SPATIAL_BLOCK_FRAMES) == 0);
    CHECK(!memcmp(output, expected, sizeof(output)));
    control.version = 77;
    CHECK(spatial_dsp_configure(&dsp, &control) != 0);
    control.version = SPATIAL_DSP_CONTROL_VERSION;
    control.reset_history = 1;
    CHECK(spatial_dsp_configure(&dsp, &control) == 0);
    CHECK(spatial_dsp_process(&dsp, input, output, SPATIAL_BLOCK_FRAMES + 1) != 0);
    CHECK(spatial_dsp_process(NULL, input, output, 1) != 0);
    puts("DSP wrapper tests passed (host execution; not a TI hardware test)");
    return 0;
}
