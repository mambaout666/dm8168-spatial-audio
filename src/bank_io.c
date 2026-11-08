#include "bank_io.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>

static int integer_token(FILE *file, long *value)
{
    char token[64], *end;
    if (fscanf(file, "%63s", token) != 1) return -1;
    errno = 0;
    *value = strtol(token, &end, 10);
    return errno || *end ? -1 : 0;
}

int spatial_bank_load(const char *path, spatial_bank *bank, char *error, size_t size)
{
    FILE *file;
    char magic[32], tag[32];
    long rate, taps, value;
    int synthetic, d, ear;
    size_t i;
    file = fopen(path, "rb");
    if (!file) {
        snprintf(error, size, "Cannot open bank: %s", path);
        return -1;
    }
    memset(bank, 0, sizeof(*bank));
    if (fscanf(file, "%31s", magic) != 1 || strcmp(magic, "SPATIAL_HRIR_V1") ||
        integer_token(file, &rate) || integer_token(file, &taps) ||
        fscanf(file, "%31s", tag) != 1 ||
        (strcmp(tag, "synthetic=0") && strcmp(tag, "synthetic=1")) ||
        rate <= 0 || rate > 384000 || taps <= 0 || taps > SPATIAL_MAX_TAPS) {
        snprintf(error, size, "Invalid bank header or unsupported rate/tap count");
        fclose(file);
        return -1;
    }
    synthetic = tag[strlen(tag) - 1] - '0';
    bank->sample_rate = (uint32_t)rate;
    bank->taps = (size_t)taps;
    bank->synthetic = synthetic;
    for (d = 0; d < SPATIAL_DIRECTIONS; ++d) {
        for (ear = 0; ear < SPATIAL_EARS; ++ear) {
            for (i = 0; i < bank->taps; ++i) {
                if (integer_token(file, &value) || value < INT16_MIN || value > INT16_MAX) {
                    snprintf(error, size, "Bank must contain exactly 4*taps int16 coefficients");
                    fclose(file);
                    return -1;
                }
                bank->coeffs[d][ear][i] = (int16_t)value;
            }
        }
    }
    if (fscanf(file, " %31s", magic) == 1 || ferror(file)) {
        snprintf(error, size, "Unexpected data after bank coefficients");
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}
