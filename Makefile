# Native Linux or the existing CodeSourcery/EZSDK cross compiler.
# Example: make CC=arm-none-linux-gnueabi-gcc
CC = cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Irpe -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
LDLIBS += -ldl -lm -lrt
BUILD ?= build
CORE = src/spatial_core.c
APP = $(CORE) src/bank_io.c src/backend.c src/main.c

.PHONY: all test clean
all: $(BUILD)/spatial-audio $(BUILD)/test-core $(BUILD)/test-dsp

$(BUILD):
	mkdir -p "$@"
$(BUILD)/spatial-audio: $(APP) $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(APP) -o "$@" $(LDLIBS)
$(BUILD)/test-core: $(CORE) tests/test_core.c include/spatial_core.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_core.c -o "$@"
$(BUILD)/test-dsp: $(CORE) rpe/spatial_dsp.c tests/test_dsp.c rpe/spatial_dsp.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) rpe/spatial_dsp.c tests/test_dsp.c -o "$@"
test: all
	$(BUILD)/test-core
	$(BUILD)/test-dsp
# Remove only this Makefile's known outputs; do not delete arbitrary BUILD trees.
clean:
	rm -f "$(BUILD)/spatial-audio" "$(BUILD)/test-core" "$(BUILD)/test-dsp"
