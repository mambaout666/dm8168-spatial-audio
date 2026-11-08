#include "backend.h"
#include "bank_io.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define os_read _read
#define os_write _write
#define os_close _close
#define os_open _open
#else
#include <poll.h>
#include <unistd.h>
#define os_read read
#define os_write write
#define os_close close
#define os_open open
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

typedef struct options {
    const char *input, *output, *bank, *stats, *backend, *plugin, *runtime;
    uint32_t rate;
    size_t fade;
    uint64_t max_blocks, switch_block;
    spatial_mode mode;
    int mute, pace;
} options;
typedef struct session {
    const options *options;
    spatial_backend backend;
    spatial_mode mode;
    int running, control_fd;
    char command[128];
    size_t command_size;
    int command_overflow;
    char telemetry_path[1024];
    uint64_t blocks, frames, misses, visual_drops;
    double process_us, next_telemetry;
    int synthetic, have_wave;
    int16_t wave_in[16], wave_left[16], wave_right[16];
} session;

static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal_number) { interrupted = signal_number; }

static double clock_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000000.0 / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000000.0 + now.tv_nsec / 1000.0;
#endif
}
static void sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000;
    nanosleep(&delay, NULL);
#endif
}
static void usage(FILE *stream)
{
    fputs("DM8168 spatial audio prototype 0.1.0\n"
          "Usage: spatial-audio [--input FILE|-] [--output FILE|-] [options]\n"
          "Raw audio: S16_LE, 2 interleaved channels, default 48000 Hz.\n"
          "  --bank FILE             Imported SPATIAL_HRIR_V1 coefficients\n"
          "  --backend cpu|rpe       Default cpu; rpe never falls back to cpu\n"
          "  --rpe-plugin FILE       Course-specific Linux RPE binding library\n"
          "  --direction 0|1         Initial direction (default 0)\n"
          "  --bypass                Duplicate the mono source, skip HRIR\n"
          "  --mute                  Start with muted output; keep stream active\n"
          "  --sample-rate HZ        Must match the coefficient bank\n"
          "  --fade-frames N         Transition duration (default 1024 frames)\n"
          "  --switch-at-block N     Switch once before zero-based block N\n"
          "  --max-blocks N          Stop after N processed blocks\n"
          "  --stats FILE            Write per-block processing CSV\n"
          "  --runtime-dir DIR       Linux control.fifo and telemetry.fifo\n"
          "  --pace                  Pace file input at the audio sample rate\n"
          "Without --bank, uses a SYNTHETIC delay/gain test bank, not HRIR.\n", stream);
}
static int number(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long result;
    if (!*text || *text == '-' || *text == '+') return -1;
    errno = 0;
    result = strtoull(text, &end, 10);
    if (errno || *end) return -1;
    *value = (uint64_t)result;
    return 0;
}
static int parse_options(int argc, char **argv, options *option)
{
    int i;
    uint64_t value;
    memset(option, 0, sizeof(*option));
    option->input = "-";
    option->output = "-";
    option->backend = "cpu";
    option->rate = SPATIAL_DEFAULT_RATE;
    option->fade = SPATIAL_BLOCK_FRAMES;
    option->max_blocks = UINT64_MAX;
    option->switch_block = UINT64_MAX;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *argument;
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { usage(stdout); return 1; }
        if (!strcmp(arg, "--bypass")) { option->mode.bypass = 1; continue; }
        if (!strcmp(arg, "--mute")) { option->mute = 1; continue; }
        if (!strcmp(arg, "--pace")) { option->pace = 1; continue; }
        if (i + 1 == argc) { fprintf(stderr, "Missing value for %s\n", arg); return -1; }
        argument = argv[++i];
        if (!strcmp(arg, "--input")) option->input = argument;
        else if (!strcmp(arg, "--output")) option->output = argument;
        else if (!strcmp(arg, "--bank")) option->bank = argument;
        else if (!strcmp(arg, "--backend")) option->backend = argument;
        else if (!strcmp(arg, "--rpe-plugin")) option->plugin = argument;
        else if (!strcmp(arg, "--stats")) option->stats = argument;
        else if (!strcmp(arg, "--runtime-dir")) option->runtime = argument;
        else {
            if (number(argument, &value)) { fprintf(stderr, "Invalid value for %s\n", arg); return -1; }
            if (!strcmp(arg, "--direction") && value < SPATIAL_DIRECTIONS) option->mode.direction = (unsigned)value;
            else if (!strcmp(arg, "--sample-rate") && value > 0 && value <= 384000) option->rate = (uint32_t)value;
            else if (!strcmp(arg, "--fade-frames") && value <= UINT32_MAX) option->fade = (size_t)value;
            else if (!strcmp(arg, "--max-blocks") && value > 0) option->max_blocks = value;
            else if (!strcmp(arg, "--switch-at-block")) option->switch_block = value;
            else { fprintf(stderr, "Unknown option or unsupported value: %s\n", arg); return -1; }
        }
    }
    return 0;
}
static int same_file(const char *a, const char *b)
{
    if (!a || !b || !strcmp(a, "-") || !strcmp(b, "-")) return 0;
    if (!strcmp(a, b)) return 1;
#ifdef _WIN32
    {
        HANDLE first = CreateFileA(a, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
        HANDLE second = CreateFileA(b, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
        BY_HANDLE_FILE_INFORMATION x, y;
        int same = first != INVALID_HANDLE_VALUE && second != INVALID_HANDLE_VALUE &&
                   GetFileInformationByHandle(first, &x) && GetFileInformationByHandle(second, &y) &&
                   x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
                   x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
        if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
        if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
        return same;
    }
#else
    {
        struct stat x, y;
        return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
    }
#endif
}

#ifndef _WIN32
static int ensure_fifo(const char *path)
{
    struct stat info;
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) return -1;
    if (lstat(path, &info) != 0 || !S_ISFIFO(info.st_mode) || info.st_uid != geteuid()) return -1;
    return 0;
}
#endif
static int setup_control(session *state)
{
    if (!state->options->runtime) return 0;
#ifdef _WIN32
    fprintf(stderr, "FIFO control requires Linux; use file mode on Windows\n");
    return -1;
#else
    {
        char path[1024];
        struct stat info;
        const char *directory = state->options->runtime;
        if (mkdir(directory, 0700) != 0 && errno != EEXIST) { perror("runtime directory"); return -1; }
        if (lstat(directory, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
            (info.st_mode & 0022)) {
            fprintf(stderr, "Runtime directory must be owned by this user and not group/world writable\n"); return -1;
        }
        if (snprintf(path, sizeof(path), "%s/control.fifo", directory) >= (int)sizeof(path) ||
            snprintf(state->telemetry_path, sizeof(state->telemetry_path), "%s/telemetry.fifo", directory) >= (int)sizeof(state->telemetry_path)) {
            fprintf(stderr, "Runtime path is too long\n"); return -1;
        }
        if (ensure_fifo(path) || ensure_fifo(state->telemetry_path)) {
            fprintf(stderr, "Cannot create or validate runtime FIFOs\n"); return -1;
        }
        state->control_fd = open(path, O_RDWR | O_NONBLOCK);
        if (state->control_fd < 0) { perror("control FIFO"); return -1; }
    }
    return 0;
#endif
}
#ifndef _WIN32
static void command(session *state, const char *text)
{
    spatial_mode next = state->mode;
    if (!strcmp(text, "START")) { state->running = 1; return; }
    if (!strcmp(text, "STOP")) { state->running = 0; return; }
    if (!strcmp(text, "DIRECTION 0")) next.direction = 0;
    else if (!strcmp(text, "DIRECTION 1")) next.direction = 1;
    else if (!strcmp(text, "BYPASS 0")) next.bypass = 0;
    else if (!strcmp(text, "BYPASS 1")) next.bypass = 1;
    else { fprintf(stderr, "Ignored invalid control command\n"); return; }
    /* Record the latest request; configure the backend only at a block boundary. */
    state->mode = next;
}
#endif
static void drain_control(session *state)
{
#ifndef _WIN32
    char data[256];
    ssize_t n;
    if (state->control_fd < 0) return;
    while ((n = read(state->control_fd, data, sizeof(data))) > 0) {
        ssize_t i;
        for (i = 0; i < n; ++i) {
            if (data[i] == '\n') {
                if (!state->command_overflow) {
                    state->command[state->command_size] = '\0';
                    command(state, state->command);
                }
                state->command_size = 0;
                state->command_overflow = 0;
            } else if (state->command_size + 1 < sizeof(state->command) && !state->command_overflow) {
                state->command[state->command_size++] = data[i];
            } else state->command_overflow = 1;
        }
    }
#else
    (void)state;
#endif
}
static void telemetry(session *state, int force)
{
#ifndef _WIN32
    char line[512];
    int fd, length, i;
    size_t used;
    double now = clock_us();
    if (!state->options->runtime || (!force && now < state->next_telemetry)) return;
    state->next_telemetry = now + 100000.0;
    fd = open(state->telemetry_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) { ++state->visual_drops; return; }
    length = snprintf(line, sizeof(line), "STATUS blocks=%" PRIu64 " frames=%" PRIu64
        " running=%d direction=%u bypass=%d process_us=%.3f deadline_misses=%" PRIu64
        " visual_drops=%" PRIu64 " synthetic=%d backend=%s\n",
        state->blocks, state->frames, state->running, state->mode.direction, state->mode.bypass,
        state->process_us, state->misses, state->visual_drops, state->synthetic, state->options->backend);
    if (length <= 0 || length >= (int)sizeof(line) || write(fd, line, (size_t)length) != length)
        ++state->visual_drops;
    if (state->have_wave) {
        memcpy(line, "WAVE", 4);
        used = 4;
        for (i = 0; i < 48; ++i) {
            int sample = i < 16 ? state->wave_in[i] : i < 32 ? state->wave_left[i - 16] : state->wave_right[i - 32];
            if (i == 16 || i == 32) line[used++] = ' ', line[used++] = '|';
            used += (size_t)snprintf(line + used, sizeof(line) - used, " %d", sample);
        }
        line[used++] = '\n';
        if (write(fd, line, used) != (ssize_t)used) ++state->visual_drops;
    }
    close(fd);
#else
    (void)state; (void)force;
#endif
}
static void service(session *state) { drain_control(state); telemetry(state, 0); }

static int open_audio(const char *path, int writing, session *state)
{
    int fd;
    if (!strcmp(path, "-")) {
        fd = writing ? 1 : 0;
#ifdef _WIN32
        _setmode(fd, O_BINARY);
#endif
        return fd;
    }
#ifdef _WIN32
    fd = os_open(path, (writing ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY) | O_BINARY,
                 _S_IREAD | _S_IWRITE);
    (void)state;
#else
    do {
        fd = open(path, (writing ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY) | O_NONBLOCK, 0600);
        if (fd >= 0 || !writing || errno != ENXIO) break;
        service(state);
        sleep_ms(10);
    } while (!interrupted);
#endif
    if (fd < 0) perror(writing ? "audio output" : "audio input");
    return fd;
}
/* Return 0 for EOF, a positive byte count for a complete/last block, -1 error. */
static int read_audio(int fd, unsigned char *data, size_t capacity, session *state, int *seen)
{
    size_t used = 0;
#ifndef _WIN32
    struct stat info;
    int fifo = strcmp(state->options->input, "-") != 0 &&
               fstat(fd, &info) == 0 && S_ISFIFO(info.st_mode);
#endif
    while (used < capacity && !interrupted) {
        int count;
        service(state);
#ifndef _WIN32
        {
            struct pollfd wait_fd;
            int result;
            wait_fd.fd = fd; wait_fd.events = POLLIN; wait_fd.revents = 0;
            result = poll(&wait_fd, 1, 100);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0) { perror("audio poll"); return -1; }
            if (!result) continue;
        }
#endif
        count = (int)os_read(fd, data + used, (unsigned)(capacity - used));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("audio read"); return -1;
        }
        if (!count) {
#ifndef _WIN32
            if (fifo && !*seen && !used) { sleep_ms(10); continue; }
#endif
            break;
        }
        *seen = 1;
        used += (size_t)count;
    }
    if (interrupted) return 0;
    if (used % 4) { fprintf(stderr, "Truncated PCM: input ends inside a stereo frame\n"); return -1; }
    return (int)used;
}
static int write_audio(int fd, const unsigned char *data, size_t bytes, session *state)
{
    size_t used = 0;
    while (used < bytes && !interrupted) {
        int count;
        service(state);
        count = (int)os_write(fd, data + used, (unsigned)(bytes - used));
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { sleep_ms(1); continue; }
            perror("audio write"); return -1;
        }
        if (!count) { fprintf(stderr, "Audio output stopped accepting data\n"); return -1; }
        used += (size_t)count;
    }
    return interrupted ? -1 : 0;
}

int main(int argc, char **argv)
{
    options option;
    session state;
    spatial_bank bank;
    unsigned char bytes[SPATIAL_BLOCK_FRAMES * 4];
    int16_t input[SPATIAL_BLOCK_FRAMES * 2], output[SPATIAL_BLOCK_FRAMES * 2];
    int input_fd = -1, output_fd = -1, seen = 0, result = 0, parsed;
    FILE *stats = NULL;
    char error[512] = "Backend initialization failed";
    double start;
    parsed = parse_options(argc, argv, &option);
    if (parsed) return parsed > 0 ? 0 : 2;
    if (same_file(option.input, option.output) || same_file(option.stats, option.input) ||
        same_file(option.stats, option.output) || same_file(option.bank, option.output) ||
        same_file(option.bank, option.stats)) {
        fprintf(stderr, "Input, output, bank and statistics must use separate files\n"); return 2;
    }
    if (option.bank) {
        if (spatial_bank_load(option.bank, &bank, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 2; }
    } else spatial_bank_demo(&bank);
    if (bank.sample_rate != option.rate) {
        fprintf(stderr, "Sample rate mismatch: bank=%" PRIu32 ", stream=%" PRIu32 "\n", bank.sample_rate, option.rate); return 2;
    }
    memset(&state, 0, sizeof(state));
    state.control_fd = -1;
    state.options = &option;
    state.mode = option.mode;
    state.running = !option.mute;
    state.synthetic = bank.synthetic;
    signal(SIGINT, interrupt_handler);
    signal(SIGTERM, interrupt_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (backend_open(&state.backend, option.backend, option.plugin, &bank, option.mode,
                     option.fade, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 2; }
    if (setup_control(&state)) { result = 2; goto cleanup; }
    if (option.stats) {
        stats = fopen(option.stats, "wb");
        if (!stats) { perror("statistics"); result = 2; goto cleanup; }
        if (fputs("block,frames,process_us,deadline_us,deadline_miss,running,direction,bypass,backend,synthetic\n", stats) == EOF) {
            perror("statistics header"); result = 1; goto cleanup;
        }
    }
    input_fd = open_audio(option.input, 0, &state);
    if (input_fd < 0) { result = 2; goto cleanup; }
    if (same_file(option.stats, option.output)) {
        fprintf(stderr, "Statistics and audio output resolve to the same file\n"); result = 2; goto cleanup;
    }
    output_fd = open_audio(option.output, 1, &state);
    if (output_fd < 0) { result = 2; goto cleanup; }
    fprintf(stderr, "backend=%s bank=%s rate=%" PRIu32 " taps=%zu\n", option.backend,
            bank.synthetic ? "SYNTHETIC_TEST_NOT_MEASURED_HRIR" : "IMPORTED_COEFFICIENTS", option.rate, bank.taps);
    start = clock_us();
    while (!interrupted && state.blocks < option.max_blocks) {
        int count = read_audio(input_fd, bytes, sizeof(bytes), &state, &seen);
        size_t frames, i;
        double before, deadline;
        spatial_mode block_mode;
        int block_running;
        if (count < 0) { result = 1; break; }
        if (!count) break;
        frames = (size_t)count / 4;
        for (i = 0; i < frames * 2; ++i) {
            uint16_t encoded = (uint16_t)bytes[2 * i] | (uint16_t)((uint16_t)bytes[2 * i + 1] << 8);
            input[i] = encoded < 32768 ? (int16_t)encoded : (int16_t)((int32_t)encoded - 65536);
        }
        drain_control(&state);
        if (state.blocks == option.switch_block) {
            spatial_mode mode = state.mode;
            mode.direction = 1 - mode.direction;
            state.mode = mode;
        }
        block_mode = state.mode;
        block_running = state.running;
        if (backend_mode(&state.backend, block_mode, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error); result = 1; break;
        }
        before = clock_us();
        if (backend_process(&state.backend, input, output, frames, error, sizeof(error))) {
            fprintf(stderr, "Audio processing failed: %s\n", error); result = 1; break;
        }
        state.process_us = clock_us() - before;
        deadline = frames * 1000000.0 / option.rate;
        if (state.process_us > deadline) ++state.misses;
        if (!block_running) memset(output, 0, frames * 2 * sizeof(int16_t));
        for (i = 0; i < 16; ++i) {
            size_t frame = i * (frames - 1) / 15;
            state.wave_in[i] = (int16_t)(((int32_t)input[2 * frame] + input[2 * frame + 1]) / 2);
            state.wave_left[i] = output[2 * frame]; state.wave_right[i] = output[2 * frame + 1];
        }
        state.have_wave = 1;
        for (i = 0; i < frames * 2; ++i) {
            uint16_t value = (uint16_t)output[i];
            bytes[2 * i] = (unsigned char)(value & 255); bytes[2 * i + 1] = (unsigned char)(value >> 8);
        }
        if (write_audio(output_fd, bytes, frames * 4, &state)) { result = 1; break; }
        if (stats && fprintf(stats, "%" PRIu64 ",%zu,%.3f,%.3f,%d,%d,%u,%d,%s,%d\n",
            state.blocks, frames, state.process_us, deadline, state.process_us > deadline,
            block_running, block_mode.direction, block_mode.bypass, option.backend, bank.synthetic) < 0) {
            perror("statistics write"); result = 1; break;
        }
        ++state.blocks; state.frames += frames;
        telemetry(&state, 0);
        if (option.pace) {
            const double due = start + state.frames * 1000000.0 / option.rate;
            while (!interrupted && clock_us() + 1000.0 < due) { service(&state); sleep_ms(1); }
        }
    }
    telemetry(&state, 1);
    fprintf(stderr, "processed_blocks=%" PRIu64 " frames=%" PRIu64 " deadline_misses=%" PRIu64 " visual_drops=%" PRIu64 "\n",
            state.blocks, state.frames, state.misses, state.visual_drops);
cleanup:
    if (input_fd > 2) os_close(input_fd);
    if (output_fd > 2) os_close(output_fd);
    if (state.control_fd >= 0) os_close(state.control_fd);
    if (stats) {
        if (ferror(stats)) { fprintf(stderr, "Statistics stream error\n"); result = 1; }
        if (fclose(stats)) { perror("statistics flush"); result = 1; }
    }
    backend_close(&state.backend);
    return interrupted ? 128 + interrupted : result;
}
