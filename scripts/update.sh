#!/usr/bin/env bash
# Server-side nyaxus updater: git pull + full Linux build + systemd roll.
# Run on the deployment box (deployment root /agent):
#   bash /agent/scripts/update.sh
# Safe to re-run: the build is incremental (objects skip unless their
# sources changed) and systemd restarts the daemon only after a build.
# One sudo use: the service restart (systemd grants are not in place).
set -eu

ROOT="${ROOT:-/agent}"
cd "$ROOT" || { echo "deployment root missing: $ROOT" >&2; exit 1; }

echo "== git pull =="
if [ "${UPDATE_SKIP_PULL:-0}" = "1" ]; then
    echo "(skipped: UPDATE_SKIP_PULL=1 - offline run with files staged)"
elif ! git pull --ff-only; then
    echo "git pull failed; keeping the $ROOT service untouched" >&2
    exit 1
fi

echo "== daemon build =="
# Vendored third_party (sqlite + TLS + miniz + SIMD + all modules), matching
# the harness/README.md Linux recipe. Object paths skip unchanged sources,
# so after the first run this is incremental.
mkdir -p harness/build/mbedtls-obj

VENDORED=""
for src in harness/third_party/mbedtls/library/*.c \
           harness/third_party/miniz/miniz.c \
           harness/third_party/miniz/miniz_tdef.c \
           harness/third_party/miniz/miniz_tinfl.c \
           harness/third_party/miniz/miniz_zip.c; do
    echo "$src"
done | xargs -P "$(nproc)" -I{} sh -c '
    src="{}"
    name=$(basename "${src%.c}")
    obj="harness/build/mbedtls-obj/$name.o"
    [ -f "$obj" ] && [ ! "$src" -nt "$obj" ] && exit 0
    gcc -O2 -DNYAXUS_HAS_MBEDTLS -I harness/third_party/mbedtls/include \
        -c "$src" -o "$obj"
' || { echo "vendored build failed" >&2; exit 1; }

if [ ! -f harness/build/nyaxus_sqlite3.o ] ||
   [ harness/third_party/sqlite/sqlite3.c -nt harness/build/nyaxus_sqlite3.o ]; then
    # sqlite compiles as C (gcc), not C++ - extern "C" name mangling.
    gcc -O2 -DSQLITE_ENABLE_FTS5 -c harness/third_party/sqlite/sqlite3.c \
        -o harness/build/nyaxus_sqlite3.o
fi

# Rebuild trigger: missing object, a newer source, or a newer tracked header.
# The source mtime check alone cannot see header-only changes - a daemon.hpp
# layout change once left daemon_config.o stale, corrupting Config at link
# time and spinning the daemon in set::count() before it ever listened.
needs_rebuild() {
    local src="$1" obj="$2"
    [ ! -f "$obj" ] && return 0
    [ "$src" -nt "$obj" ] && return 0
    [ -n "$(find harness/include -name '*.hpp' -newer "$obj" -print -quit)" ]
}

# SIMD variants: per-file -m flags (see CMakeLists.txt). SSE2 is baseline.
if needs_rebuild harness/src/qwen3_sse2.cpp harness/build/qwen3_sse2.o; then
    g++ -std=c++20 -O2 -I harness/include -c harness/src/qwen3_sse2.cpp \
        -o harness/build/qwen3_sse2.o
fi
if needs_rebuild harness/src/qwen3_avx.cpp harness/build/qwen3_avx.o; then
    g++ -std=c++20 -O2 -I harness/include -mavx -mfma \
        -c harness/src/qwen3_avx.cpp -o harness/build/qwen3_avx.o
fi
if needs_rebuild harness/src/qwen3_avx2.cpp harness/build/qwen3_avx2.o; then
    g++ -std=c++20 -O2 -I harness/include -mavx2 -mfma \
        -c harness/src/qwen3_avx2.cpp -o harness/build/qwen3_avx2.o
fi

# Core modules, sequential: each takes seconds and shares no state.
MODULES="argvpolicy commandplane core denyguard fingerprint frozen gatepolicy
json http infra loopguard memory memory_custom memorydb promptpacks provider qwen3
skills store streamrules summary daemon_config daemon_http_util daemon_summary"

for mod in $MODULES; do
    src="harness/src/$mod.cpp"
    obj="harness/build/$mod.o"
    if needs_rebuild "$src" "$obj"; then
        g++ -std=c++20 -O2 -DNYAXUS_HAS_MBEDTLS -I harness/include \
            -I harness/third_party/sqlite -I harness/third_party/mbedtls/include \
            -c "$src" -o "$obj"
    fi
done

# The daemon TU only recompiles when its own source or a header moves
# (it is huge). The miniz include serves the OOXML attachment extraction.
if needs_rebuild harness/src/daemon.cpp harness/build/daemon.o; then
    g++ -std=c++20 -O2 -DNYAXUS_HAS_MBEDTLS -I harness/include \
        -I harness/third_party/sqlite -I harness/third_party/mbedtls/include \
        -I harness/third_party/miniz \
        -c harness/src/daemon.cpp -o harness/build/daemon.o
fi

echo "== daemon link =="
g++ harness/build/*.o harness/build/mbedtls-obj/*.o \
    -o harness/nyaxus_daemon_linux -lpthread -ldl
echo "linked: harness/nyaxus_daemon_linux"

echo "== web client =="
if [ -d web/node_modules ] && [ -f web/dist/index.html ]; then
    # Incremental re-build; node_modules already provisioned.
    (cd web && VITE_HARNESS_MODE=http npm run build)
else
    echo "skipping web build (node_modules not provisioned)" >&2
    echo "  first setup: cd web && npm ci && VITE_HARNESS_MODE=http npm run build" >&2
fi

echo "== systemd roll =="
# Sync the tracked unit files first: they carry deployment hardening choices
# (e.g. the NoNewPrivileges removal so sudo works in the terminal tool), and
# a unit change is a no-op until daemon-reload re-reads it.
for unit in scripts/systemd/*.service; do
    sudo cp "$unit" /etc/systemd/system/
done
sudo systemctl daemon-reload
sudo systemctl restart nyaxus-daemon
sleep 2
if curl -sf "http://127.0.0.1:${NYAXUS_PORT:-8787}/api/v1/health" > /dev/null; then
    echo "daemon healthy on :${NYAXUS_PORT:-8787}"
else
    echo "daemon service restarted but not healthy yet - check:" >&2
    echo "  journalctl -u nyaxus-daemon -n 40" >&2
fi
