# enkakumon — build the daemon and run its tests.
#
#   make            build ./enkakumond
#   make test       unit tests
#   make asan       unit tests under AddressSanitizer + UBSan
#   make memcheck   unit tests under valgrind
#   make soakcheck  run the daemon for SOAK_S seconds (default 1 h) and fail if
#                   RSS grows after warmup, the fd count changes, the heartbeat
#                   goes stale, or SIGTERM does not exit 0. Do not leave socket
#                   clients connected during the soak: they change the fd count.

CC      ?= gcc
CFLAGS  ?= -O2 -g
WARN     = -std=c17 -Wall -Wextra -Werror -Wvla
CPPFLAGS = -D_GNU_SOURCE -Isrc -MMD -MP
LDLIBS   = -lasound -lpthread -lm
ASAN     = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all

LIB_SRC  = src/conf.c src/i2c.c src/sensors.c src/mic.c src/ipc.c \
           src/vendor/sensirion_gas_index_algorithm.c
LIB_OBJ  = $(LIB_SRC:%.c=build/%.o)
MAIN_OBJ = build/src/main.o
TEST_OBJ = build/test/test_main.o

ASAN_OBJ = $(LIB_SRC:%.c=build/asan/%.o) build/asan/test/test_main.o

SOAK_S        ?= 3600
SOAK_WARMUP_S ?= 60
SOAK_CONF     ?= enkakumon.conf

.PHONY: all test asan memcheck soakcheck clean

all: enkakumond

enkakumond: $(MAIN_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

build/asan/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(ASAN) $(CPPFLAGS) -c -o $@ $<

build/test_main: $(TEST_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/asan/test_main: $(ASAN_OBJ)
	$(CC) $(ASAN) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: build/test_main
	./build/test_main

asan: build/asan/test_main
	ASAN_OPTIONS=detect_leaks=1 ./build/asan/test_main

memcheck: build/test_main
	valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible \
		--errors-for-leak-kinds=definite,indirect,possible --error-exitcode=1 \
		./build/test_main

soakcheck: enkakumond
	@set -u; \
	hb=$$(awk -F= '/^[ \t]*HEARTBEAT_PATH[ \t]*=/ { v = $$2; sub(/#.*/, "", v); gsub(/[ \t]/, "", v); print v }' $(SOAK_CONF)); \
	hb=$${hb:-/dev/shm/enkakumon.heartbeat}; \
	mkdir -p build; \
	./enkakumond $(SOAK_CONF) > build/soak.log 2>&1 & pid=$$!; \
	fail() { echo "soakcheck: FAIL: $$*"; kill $$pid 2>/dev/null; echo "--- build/soak.log (tail)"; tail -n 20 build/soak.log; exit 1; }; \
	rss() { awk '/^VmRSS:/ { print $$2 }' /proc/$$pid/status; }; \
	fds() { ls /proc/$$pid/fd | wc -l; }; \
	sleep 2; \
	kill -0 $$pid 2>/dev/null || fail "daemon exited at startup"; \
	t=0; rss0=""; fd0=""; \
	echo "soakcheck: pid $$pid, $(SOAK_S) s, warmup $(SOAK_WARMUP_S) s, heartbeat $$hb"; \
	while [ $$t -lt $(SOAK_S) ]; do \
		sleep 5; t=$$((t + 5)); \
		kill -0 $$pid 2>/dev/null || fail "daemon died after $$t s"; \
		age=$$(( $$(date +%s) - $$(stat -c %Y "$$hb") )); \
		[ $$age -le 5 ] || fail "heartbeat $$age s old at $$t s"; \
		if [ -z "$$rss0" ] && [ $$t -ge $(SOAK_WARMUP_S) ]; then \
			rss0=$$(rss); fd0=$$(fds); \
			echo "soakcheck: after warmup RSS $$rss0 kB, $$fd0 fds"; \
		fi; \
	done; \
	[ -n "$$rss0" ] || fail "SOAK_S must be longer than SOAK_WARMUP_S"; \
	rss1=$$(rss); fd1=$$(fds); \
	echo "soakcheck: at end RSS $$rss1 kB, $$fd1 fds"; \
	[ $$rss1 -le $$rss0 ] || fail "RSS grew from $$rss0 to $$rss1 kB"; \
	[ $$fd1 -eq $$fd0 ] || fail "fd count changed from $$fd0 to $$fd1"; \
	kill -TERM $$pid; rc=0; wait $$pid || rc=$$?; \
	[ $$rc -eq 0 ] || fail "SIGTERM exit status $$rc"; \
	echo "soakcheck: PASS"

clean:
	rm -rf build enkakumond

-include $(shell find build -name '*.d' 2>/dev/null)
