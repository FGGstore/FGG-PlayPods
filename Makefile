# FGG-PlayPods
#
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    ifneq ($(MAKECMDGOALS),check-offline)
    $(error PS5_PAYLOAD_SDK is undefined)
    endif
endif

ELF   := fgg-playpods.elf
BUILD := build

CFLAGS     := -DHCI_OUT_WAIT_REARM=0 -DSTREAM_LIMIT_MS=0 -std=c11 -Wall -Wextra -Werror -O2 -Isrc -Ithird_party/sbc
# Vendored code is built as upstream wrote it, without this project's
# warning policy.
SBC_CFLAGS := -std=gnu11 -O2 -w -Ithird_party/sbc

SRCS     := src/main.c src/capture.c src/hci.c src/bt.c src/sdp.c src/a2dp.c src/log.c src/session_lock.c
SBC_SRCS := third_party/sbc/sbc.c third_party/sbc/sbc_primitives.c
OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(SRCS) $(SBC_SRCS))

.PHONY: all clean test check-offline

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SBC_CFLAGS) -c -o $@ $<

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

# Run the accepted bounds/capture/lock suites and the resident RFC fixtures.
# Only Linux executables are run; this target never invokes PS5_DEPLOY.
HOST_CC ?= gcc
HOST_TEST_FLAGS := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g \
                   -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
check-offline:
	@mkdir -p $(BUILD)/offline
	$(HOST_CC) $(HOST_TEST_FLAGS) tests/bt_bounds_test.c src/sdp.c -o $(BUILD)/offline/bt_bounds_test
	$(HOST_CC) $(HOST_TEST_FLAGS) tests/sdp_bounds_test.c -o $(BUILD)/offline/sdp_bounds_test
	$(HOST_CC) $(HOST_TEST_FLAGS) -ffunction-sections -fdata-sections -Ithird_party/sbc tests/a2dp_bounds_test.c -Wl,--gc-sections -o $(BUILD)/offline/a2dp_bounds_test
	$(HOST_CC) $(HOST_TEST_FLAGS) -Itests/stubs tests/capture_test.c -o $(BUILD)/offline/capture_test
	$(HOST_CC) $(HOST_TEST_FLAGS) tests/session_lock_test.c -o $(BUILD)/offline/session_lock_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/offline/bt_bounds_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/offline/sdp_bounds_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/offline/a2dp_bounds_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/offline/capture_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/offline/session_lock_test
	python3 tools/check-resident.py

clean:
	rm -rf $(BUILD) $(ELF)
