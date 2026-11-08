#include "spatial_core.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static spatial_bank bank;
static spatial_processor first;
static spatial_processor second;

static spatial_mode mode(unsigned direction, int bypass)
{
    spatial_mode result;
    result.direction = direction;
    result.bypass = bypass;
    return result;
}

static void empty_bank(size_t taps)
{
    memset(&bank, 0, sizeof(bank));
    bank.sample_rate = SPATIAL_DEFAULT_RATE;
    bank.taps = taps;
}

static void test_impulse_order_and_history(void)
{
    int16_t input[12] = { 16384, 16384, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int16_t output[12] = { 0 };
    const int16_t expected[12] = { 8192, -8192, 4096, 0, 2048, 8192,
                                 0, 0, 0, 0, 0, 0 };
    empty_bank(3);
    bank.coeffs[0][0][0] = 16384;
    bank.coeffs[0][0][1] = 8192;
    bank.coeffs[0][0][2] = 4096;
    bank.coeffs[0][1][0] = -16384;
    bank.coeffs[0][1][2] = 16384;
    CHECK(spatial_init(&first, &bank, mode(0, 0), 0) == 0);
    CHECK(spatial_process(&first, input, output, 1) == 0);
    CHECK(spatial_process(&first, input + 2, output + 2, 2) == 0);
    CHECK(spatial_process(&first, input + 6, output + 6, 3) == 0);
    CHECK(memcmp(output, expected, sizeof(expected)) == 0);
}

static void test_partition_invariance_and_in_place(void)
{
    enum { FRAMES = 2207 };
    static int16_t input[2 * FRAMES];
    static int16_t whole[2 * FRAMES];
    static int16_t partitioned[2 * FRAMES];
    size_t position = 0;
    size_t block = 1;
    size_t i;
    uint32_t random = 0x17319u;
    spatial_bank_demo(&bank);
    for (i = 0; i < 2 * FRAMES; ++i) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        input[i] = (int16_t)((int32_t)(random % 50001u) - 25000);
    }
    CHECK(spatial_init(&first, &bank, mode(1, 0), 0) == 0);
    CHECK(spatial_init(&second, &bank, mode(1, 0), 0) == 0);
    CHECK(spatial_process(&first, input, whole, FRAMES) == 0);
    while (position < FRAMES) {
        size_t count = block;
        if (count > FRAMES - position) {
            count = FRAMES - position;
        }
        CHECK(spatial_process(&second, input + 2 * position,
                              partitioned + 2 * position, count) == 0);
        position += count;
        block = (block * 17) % 107 + 1;
    }
    CHECK(memcmp(whole, partitioned, sizeof(whole)) == 0);
    memcpy(partitioned, input, sizeof(input));
    CHECK(spatial_init(&second, &bank, mode(1, 0), 0) == 0);
    CHECK(spatial_process(&second, partitioned, partitioned, FRAMES) == 0);
    CHECK(memcmp(whole, partitioned, sizeof(whole)) == 0);
}

static void test_rounding_saturation_and_wide_accumulator(void)
{
    int16_t input[8] = { 1, 1, -1, -1, 3, 3, -3, -3 };
    int16_t output[8];
    const int16_t expected[8] = { 1, -1, -1, 1, 2, -2, -2, 2 };
    size_t i;
    static int16_t loud[2 * SPATIAL_MAX_TAPS];
    static int16_t result[2 * SPATIAL_MAX_TAPS];
    empty_bank(1);
    bank.coeffs[0][0][0] = 16384;
    bank.coeffs[0][1][0] = -16384;
    CHECK(spatial_init(&first, &bank, mode(0, 0), 0) == 0);
    CHECK(spatial_process(&first, input, output, 4) == 0);
    CHECK(memcmp(output, expected, sizeof(expected)) == 0);

    empty_bank(SPATIAL_MAX_TAPS);
    for (i = 0; i < SPATIAL_MAX_TAPS; ++i) {
        bank.coeffs[0][0][i] = INT16_MAX;
        bank.coeffs[0][1][i] = INT16_MIN;
        loud[2 * i] = INT16_MAX;
        loud[2 * i + 1] = INT16_MAX;
    }
    CHECK(spatial_init(&first, &bank, mode(0, 0), 0) == 0);
    CHECK(spatial_process(&first, loud, result, SPATIAL_MAX_TAPS) == 0);
    CHECK(result[2 * (SPATIAL_MAX_TAPS - 1)] == INT16_MAX);
    CHECK(result[2 * (SPATIAL_MAX_TAPS - 1) + 1] == INT16_MIN);
}

static void test_single_source_and_bypass_history(void)
{
    int16_t input[8] = { 32767, 32767, -32768, -32768, -1, 0, 100, -100 };
    int16_t output[8];
    const int16_t expected[8] = { 32767, 32767, -32768, -32768, 0, 0, 0, 0 };
    int16_t impulse[2] = { 16384, 16384 };
    int16_t zeros[2] = { 0, 0 };
    empty_bank(2);
    bank.coeffs[0][0][1] = 16384;
    bank.coeffs[0][1][1] = -16384;
    CHECK(spatial_init(&first, &bank, mode(0, 1), 0) == 0);
    CHECK(spatial_process(&first, input, output, 4) == 0);
    CHECK(memcmp(output, expected, sizeof(expected)) == 0);
    CHECK(spatial_init(&first, &bank, mode(0, 1), 0) == 0);
    CHECK(spatial_process(&first, impulse, output, 1) == 0);
    CHECK(spatial_set_mode(&first, mode(0, 0)) == 0);
    CHECK(spatial_process(&first, zeros, output, 1) == 0);
    CHECK(output[0] == 8192 && output[1] == -8192);
}

static void constant_mode_bank(void)
{
    empty_bank(1);
    bank.coeffs[0][0][0] = 16384;
    bank.coeffs[0][1][0] = 16384;
    bank.coeffs[1][0][0] = -16384;
    bank.coeffs[1][1][0] = -16384;
}

static void test_fades_and_latest_pending_request(void)
{
    const int16_t input[16] = { 12000, 12000, 12000, 12000,
                               12000, 12000, 12000, 12000,
                               12000, 12000, 12000, 12000,
                               12000, 12000, 12000, 12000 };
    int16_t output[16];
    size_t i;
    constant_mode_bank();
    CHECK(spatial_init(&first, &bank, mode(0, 0), 4) == 0);
    CHECK(spatial_set_mode(&first, mode(1, 0)) == 0);
    CHECK(spatial_process(&first, input, output, 1) == 0);
    CHECK(output[0] == 6000);
    CHECK(spatial_set_mode(&first, mode(0, 1)) == 0);
    CHECK(spatial_set_mode(&first, mode(0, 0)) == 0);
    CHECK(spatial_process(&first, input, output + 2, 7) == 0);
    CHECK(output[6] == -6000);
    CHECK(output[8] == -6000);
    CHECK(output[14] == 6000);
    CHECK(first.current.direction == 0 && first.current.bypass == 0);
    for (i = 0; i < 8; ++i) {
        CHECK(output[2 * i] == output[2 * i + 1]);
    }

    /* Repeating the active target must not restart the fade or retain stale
     * pending requests. The output reaches the target on schedule.
     */
    CHECK(spatial_init(&first, &bank, mode(0, 0), 4) == 0);
    CHECK(spatial_set_mode(&first, mode(1, 0)) == 0);
    CHECK(spatial_process(&first, input, output, 1) == 0);
    CHECK(spatial_set_mode(&first, mode(0, 0)) == 0);
    CHECK(spatial_set_mode(&first, mode(1, 0)) == 0);
    CHECK(spatial_process(&first, input, output, 7) == 0);
    CHECK(output[4] == -6000 && output[12] == -6000);
    CHECK(first.current.direction == 1 && !first.transitioning);

    CHECK(spatial_init(&first, &bank, mode(0, 0), 1) == 0);
    CHECK(spatial_set_mode(&first, mode(1, 0)) == 0);
    CHECK(spatial_process(&first, input, output, 1) == 0);
    CHECK(output[0] == -6000);
}

static void test_fade_partition_invariance(void)
{
    int16_t input[32];
    int16_t whole[32];
    int16_t partitioned[32];
    size_t i;
    constant_mode_bank();
    for (i = 0; i < 32; ++i) {
        input[i] = (int16_t)((int)i * 197 - 3000);
    }
    CHECK(spatial_init(&first, &bank, mode(0, 0), 9) == 0);
    CHECK(spatial_init(&second, &bank, mode(0, 0), 9) == 0);
    CHECK(spatial_set_mode(&first, mode(1, 0)) == 0);
    CHECK(spatial_set_mode(&second, mode(1, 0)) == 0);
    CHECK(spatial_process(&first, input, whole, 16) == 0);
    CHECK(spatial_process(&second, input, partitioned, 3) == 0);
    CHECK(spatial_process(&second, input + 6, partitioned + 6, 2) == 0);
    CHECK(spatial_process(&second, input + 10, partitioned + 10, 11) == 0);
    CHECK(memcmp(whole, partitioned, sizeof(whole)) == 0);
}

static void test_invalid_arguments(void)
{
    int16_t samples[2] = { 0, 0 };
    spatial_bank_demo(&bank);
    CHECK(spatial_bank_validate(NULL) != 0);
    CHECK(spatial_init(NULL, &bank, mode(0, 0), 0) != 0);
    CHECK(spatial_init(&first, NULL, mode(0, 0), 0) != 0);
    CHECK(spatial_init(&first, &bank, mode(2, 0), 0) != 0);
    CHECK(spatial_init(&first, &bank, mode(0, -1), 0) != 0);
    bank.taps = SPATIAL_MAX_TAPS + 1;
    CHECK(spatial_bank_validate(&bank) != 0);
    bank.taps = 0;
    CHECK(spatial_bank_validate(&bank) != 0);
    spatial_bank_demo(&bank);
    CHECK(spatial_init(&first, &bank, mode(0, 0), 0) == 0);
    CHECK(spatial_process(&first, NULL, NULL, 0) == 0);
    CHECK(spatial_process(&first, NULL, samples, 1) != 0);
    CHECK(spatial_process(&first, samples, NULL, 1) != 0);
    CHECK(spatial_process(NULL, samples, samples, 1) != 0);
    CHECK(spatial_set_mode(&first, mode(0, 2)) != 0);
    CHECK(spatial_set_mode(NULL, mode(0, 0)) != 0);
    first.write_index = bank.taps;
    CHECK(spatial_process(&first, samples, samples, 1) != 0);
    CHECK(spatial_set_mode(&first, mode(1, 0)) != 0);
}

int main(void)
{
    test_impulse_order_and_history();
    test_partition_invariance_and_in_place();
    test_rounding_saturation_and_wide_accumulator();
    test_single_source_and_bypass_history();
    test_fades_and_latest_pending_request();
    test_fade_partition_invariance();
    test_invalid_arguments();
    if (failures != 0) {
        fprintf(stderr, "%d core checks failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("core tests passed: impulse, stream partitions, saturation, fades, bypass, validation");
    return EXIT_SUCCESS;
}
