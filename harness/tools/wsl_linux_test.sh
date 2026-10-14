#!/usr/bin/env bash
# wsl_linux_test.sh - build the daemon natively under WSL (Debian) and run
# the e2e suite there, so the POSIX terminal-tool path (fork/exec, process
# group kill) is exercised on real Linux instead of only by inspection.
#
# Usage (from Git Bash):
#   wsl -e bash /mnt/c/<repo>/agent/harness/tools/wsl_linux_test.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
HARNESS="$SRC/harness"
DEST="$HOME/nyaxus/harness"

echo "== copy source to WSL home =="
mkdir -p "$DEST"
(cd "$HARNESS" && tar cf - \
    --exclude=./build --exclude=./build-mingw --exclude=./build-verify \
    --exclude=./models --exclude='*.exe' --exclude='*.pdb' .) \
    | (cd "$DEST" && tar xf -)

echo "== configure + build =="
cd "$DEST"
cmake -B build -DCMAKE_BUILD_TYPE=Release . > cmake_configure.log 2>&1
cmake --build build --target nyaxus_daemon -j"$(nproc)" > cmake_build.log 2>&1
if [ ! -x build/nyaxus_daemon ]; then
    echo "build failed:" >&2
    tail -30 cmake_build.log >&2
    exit 1
fi
echo "build ok: $DEST/build/nyaxus_daemon"

echo "== e2e: tool_stream =="
NODE=/usr/bin/node bash tests/tool_stream_test.sh build/nyaxus_daemon
echo "== e2e: session_delete =="
NODE=/usr/bin/node bash tests/session_delete_test.sh build/nyaxus_daemon
echo "== e2e: process_tree =="
NODE=/usr/bin/node bash tests/process_tree_test.sh build/nyaxus_daemon

echo "WSL_LINUX_E2E_ALL_PASSED"
