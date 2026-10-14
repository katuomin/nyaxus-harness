#!/usr/bin/env bash
# run_all.sh - build the daemon, then run the full e2e suite.
#
# The individual test scripts assume a prebuilt daemon at
# harness/nyaxus_daemon.exe (they fail with "daemon binary not found"
# otherwise), so a suite that skips the build can silently test a stale
# binary. This runner closes that gap: it builds first (zig recipe, see
# harness/README.md) and then runs every harness/tests/*_test.sh, reporting
# a pass/fail summary at the end.
#
# Usage:
#   harness/tests/run_all.sh            build + full suite
#   harness/tests/run_all.sh --no-build full suite, existing binary
#   harness/tests/run_all.sh --filter budget   only scripts matching "budget"
#   NODE=/path/to/node harness/tests/run_all.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="$ROOT/harness/nyaxus_daemon.exe"
ZIG="${ZIG:-zig}"
BUILD=1
FILTER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0; shift ;;
        --filter=*) FILTER="${1#--filter=}"; shift ;;
        --filter)   shift; FILTER="${1:-}"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

fail() { echo "FAIL: $1" >&2; exit 1; }

if [ "$BUILD" = "1" ]; then
    command -v "$ZIG" > /dev/null 2>&1 || fail "zig not found on PATH (ZIG=${ZIG})"
    echo "== build daemon =="
    (
        set -e
        cd "$ROOT"
        mkdir -p harness/build
        # SQLITE_ENABLE_FTS5 is required: the memory bank's lexical projection
        # (memory_fts) is an FTS5 table. Without the define the daemon builds
        # fine but memory.save fails at runtime with "no such module: fts5"
        # (the CMake build defines it; the zig path must match).
        "$ZIG" cc -O2 -c harness/third_party/sqlite/sqlite3.c \
            -I harness/third_party/sqlite -DSQLITE_ENABLE_FTS5 \
            -o harness/build/nyaxus_sqlite3.o
        "$ZIG" c++ -std=c++20 -O2 -I harness/include \
            -c harness/src/qwen3_sse2.cpp -o harness/build/qwen3_sse2.o
        "$ZIG" c++ -std=c++20 -O2 -I harness/include -mavx -mfma \
            -c harness/src/qwen3_avx.cpp -o harness/build/qwen3_avx.o
        "$ZIG" c++ -std=c++20 -O2 -I harness/include -mavx2 -mfma \
            -c harness/src/qwen3_avx2.cpp -o harness/build/qwen3_avx2.o
        # Vendored TLS + gzip for the native https fetch (web.fetch tier 0):
        # mbedtls with the default config; NYAXUS_HAS_MBEDTLS turns the
        # http::Transport TLS path on. bcrypt: mbedtls entropy seeding on
        # Windows. Not defining the macro keeps the legacy build viable.
        mkdir -p harness/build/mbedtls-obj
        for src in harness/third_party/mbedtls/library/*.c \
                   harness/third_party/miniz/miniz.c \
                   harness/third_party/miniz/miniz_tdef.c \
                   harness/third_party/miniz/miniz_tinfl.c \
                   harness/third_party/miniz/miniz_zip.c; do
            name="$(basename "${src%.c}")"
            obj="harness/build/mbedtls-obj/$name.o"
            # Incremental: rebuilt only when the source is newer than the
            # object - 110+ vendored objects would otherwise recompile on
            # every suite run and put the box under load for minutes.
            if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
                "$ZIG" cc -O2 -DNYAXUS_HAS_MBEDTLS \
                    -I harness/third_party/mbedtls/include \
                    -c "$src" -o "$obj" || \
                    fail "vendored TLS compile failed for $src"
            fi
        done
        "$ZIG" c++ -std=c++20 -O2 -DNYAXUS_HAS_MBEDTLS \
            -I harness/include -I harness/third_party/sqlite \
            -I harness/third_party/mbedtls/include \
            harness/src/argvpolicy.cpp harness/src/commandplane.cpp \
            harness/src/core.cpp harness/src/denyguard.cpp \
            harness/src/fingerprint.cpp \
            harness/src/frozen.cpp harness/src/gatepolicy.cpp \
            harness/src/json.cpp harness/src/http.cpp harness/src/infra.cpp \
            harness/src/loopguard.cpp harness/src/memory.cpp \
            harness/src/memory_custom.cpp harness/src/memorydb.cpp \
            harness/src/promptpacks.cpp \
            harness/src/provider.cpp harness/src/qwen3.cpp harness/src/skills.cpp \
            harness/src/store.cpp harness/src/streamrules.cpp \
            harness/src/summary.cpp \
            harness/src/daemon_config.cpp harness/src/daemon_http_util.cpp \
            harness/src/daemon_summary.cpp \
            harness/build/qwen3_sse2.o harness/build/qwen3_avx.o \
            harness/build/qwen3_avx2.o harness/build/nyaxus_sqlite3.o \
            harness/src/daemon.cpp \
            harness/build/mbedtls-obj/*.o \
            -lws2_32 -lbcrypt -o harness/nyaxus_daemon.exe
    ) || fail "daemon build failed"
    echo "ok"
else
    [ -f "$DAEMON" ] || fail "daemon binary not found: $DAEMON (run without --no-build)"
fi

echo
passed=0
failed=0
failed_names=""

for script in "$ROOT"/harness/tests/*_test.sh; do
    name="$(basename "$script")"
    [ "$name" = "run_all.sh" ] && continue
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then
        continue
    fi
    # Leaked processes from an interrupted previous run (SIGKILL skips the
    # EXIT trap) pin the 9xxx test ports and mock ports; the per-test
    # e2e_port_free pre-flight is the backstop with a clear message.
    pkill -f "nyaxus_daemon.*--port 9[0-9][0-9][0-9]" 2>/dev/null || true
    pkill -f "harness/tests/(mock_|tls_fixture)" 2>/dev/null || true
    echo "== $name =="
    if bash "$script"; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        failed_names="$failed_names $name"
        echo "FAILED: $name" >&2
    fi
done

echo
echo "e2e suite: $passed passed, $failed failed"
if [ "$failed" -gt 0 ]; then
    echo "failed:$failed_names" >&2
    exit 1
fi
echo "E2E_SUITE_PASSED"
