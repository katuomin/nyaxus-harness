# nyaxus harness

This is the standalone native harness core. It is intentionally independent of
any external agent harness or browser framework.

The core contains the durable domain primitives:

- versioned profile resolution with explicit inherit/disable semantics
- immutable profile fingerprints for session and child-run snapshots
- cancellable runtime primitives
- validated run lifecycle transitions
- append-only SQLite event ledger with global row cursors and replay

Provider transport, the HTTP API, terminal execution, and the browser contract
were added in later slices and are now all present (2026-09-22). The
`nyaxus_demo` executable is a small separate command that exercises the ledger
and is not a model server. There is no `--demo` flag.

The `nyaxus_daemon` is the HTTP/SSE model server. It serves `/api/v1`
(a route table of roughly 80 endpoints), streams runs to connected clients,
persists events to a SQLite ledger, and can talk to any OpenAI-compatible
endpoint (llama.cpp, vLLM).

## build the daemon

CMake:

```sh
cmake -S harness -B harness/build
cmake --build harness/build
```

Zig (self-contained, also cross-compiles to Linux). The bundled SQLite
amalgamation is C, so compile it separately and link the object into the C++
build:

```sh
zig cc -O2 -c harness/third_party/sqlite/sqlite3.c \
    -I harness/third_party/sqlite -o build/nyaxus_sqlite3.o
# SIMD matvec/GEMM variants, each compiled with its own -m flag and selected
# at runtime via cpuid (see CMakeLists.txt):
zig c++ -std=c++20 -O2 -I harness/include -c harness/src/qwen3_sse2.cpp \
    -o build/qwen3_sse2.o
zig c++ -std=c++20 -O2 -I harness/include -mavx -mfma \
    -c harness/src/qwen3_avx.cpp -o build/qwen3_avx.o
zig c++ -std=c++20 -O2 -I harness/include -mavx2 -mfma \
    -c harness/src/qwen3_avx2.cpp -o build/qwen3_avx2.o
zig c++ -std=c++20 -O2 -I harness/include -I harness/third_party/sqlite \
    harness/src/core.cpp harness/src/json.cpp harness/src/http.cpp \
    harness/src/memory.cpp harness/src/memory_custom.cpp \
    harness/src/memorydb.cpp harness/src/promptpacks.cpp \
    harness/src/provider.cpp harness/src/qwen3.cpp harness/src/skills.cpp \
    harness/src/store.cpp harness/src/summary.cpp \
    harness/src/loopguard.cpp harness/src/argvpolicy.cpp \
    harness/src/commandplane.cpp harness/src/denyguard.cpp \
    harness/src/fingerprint.cpp harness/src/frozen.cpp \
    harness/src/gatepolicy.cpp harness/src/infra.cpp \
    harness/src/streamrules.cpp \
    harness/src/daemon_config.cpp harness/src/daemon_http_util.cpp \
    harness/src/daemon_summary.cpp \
    build/qwen3_sse2.o build/qwen3_avx.o build/qwen3_avx2.o \
    build/nyaxus_sqlite3.o harness/src/daemon.cpp \
    harness/third_party/mbedtls/library/*.c \
    harness/third_party/miniz/miniz.c \
    harness/third_party/miniz/miniz_tdef.c \
    harness/third_party/miniz/miniz_tinfl.c \
    harness/third_party/miniz/miniz_zip.c \
    -DNYAXUS_HAS_MBEDTLS \
    -I harness/third_party/mbedtls/include \
    -lws2_32 -lbcrypt -o harness/nyaxus_daemon.exe      # Windows
# NYAXUS_HAS_MBEDTLS + mbedtls/miniz are the vendored TLS + gzip stacks for
# the native https fetch (web.fetch tier 0); -lbcrypt is mbedtls entropy
# seeding on Windows. Linux needs no extra libs.
# or for Linux (cross-compile from Windows):
zig cc -O2 -target x86_64-linux-gnu.2.31 \
    -c harness/third_party/sqlite/sqlite3.c \
    -I harness/third_party/sqlite -o build/nyaxus_sqlite3_linux.o
zig c++ -std=c++20 -O2 -target x86_64-linux-gnu.2.31 -I harness/include \
    -c harness/src/qwen3_sse2.cpp -o build/qwen3_sse2_linux.o
zig c++ -std=c++20 -O2 -target x86_64-linux-gnu.2.31 -I harness/include \
    -mavx -mfma -c harness/src/qwen3_avx.cpp -o build/qwen3_avx_linux.o
zig c++ -std=c++20 -O2 -target x86_64-linux-gnu.2.31 -I harness/include \
    -mavx2 -mfma -c harness/src/qwen3_avx2.cpp -o build/qwen3_avx2_linux.o
zig c++ -std=c++20 -O2 -target x86_64-linux-gnu.2.31 \
    -I harness/include -I harness/third_party/sqlite \
    harness/src/core.cpp harness/src/json.cpp harness/src/http.cpp \
    harness/src/memory.cpp harness/src/memory_custom.cpp \
    harness/src/memorydb.cpp harness/src/promptpacks.cpp \
    harness/src/provider.cpp harness/src/qwen3.cpp harness/src/skills.cpp \
    harness/src/store.cpp harness/src/summary.cpp \
    harness/src/loopguard.cpp harness/src/argvpolicy.cpp \
    harness/src/commandplane.cpp harness/src/denyguard.cpp \
    harness/src/fingerprint.cpp harness/src/frozen.cpp \
    harness/src/gatepolicy.cpp harness/src/infra.cpp \
    harness/src/streamrules.cpp \
    harness/src/daemon_config.cpp harness/src/daemon_http_util.cpp \
    harness/src/daemon_summary.cpp \
    build/qwen3_sse2_linux.o build/qwen3_avx_linux.o build/qwen3_avx2_linux.o \
    build/nyaxus_sqlite3_linux.o harness/src/daemon.cpp \
    harness/third_party/mbedtls/library/*.c \
    harness/third_party/miniz/miniz.c \
    harness/third_party/miniz/miniz_tdef.c \
    harness/third_party/miniz/miniz_tinfl.c \
    harness/third_party/miniz/miniz_zip.c \
    -DNYAXUS_HAS_MBEDTLS \
    -I harness/third_party/mbedtls/include \
    -o harness/nyaxus_daemon_linux
```

## run the daemon against a real model

Point it at your llama-cpp-manager (or any OpenAI-compatible server):

```sh
./nyaxus_daemon \
  --port 8787 \
  --provider openai \
  --llama http://llama-cpp-manager:8080/v1 \
  --model /models/Swift-Qwen3.8-27B-Uncensored-BF16.i1-Q4_K_M.gguf \
  --workspace /path/to/workspace
```

The same settings can be supplied as environment variables:
`NYAXUS_PORT`, `NYAXUS_PROVIDER`, `NYAXUS_LLAMA_URL`, `NYAXUS_MODEL`,
`NYAXUS_API_KEY`, `NYAXUS_WORKSPACE`, `NYAXUS_PASSWORD`.

On startup with `--provider openai`, the daemon queries the endpoint's
`/v1/models` endpoint and auto-resolves the model id and context window when
`--model` is not given. `GET /health` reports the discovered model and context.

Without `--provider openai` (or `NYAXUS_PROVIDER=openai`) the daemon uses the
deterministic fake provider so you can exercise the transport before a model is
reachable.

Verified endpoints (`/api/v1`):

- `POST /sessions` create a session
- `GET /sessions` list sessions
- `GET /sessions/{id}` session snapshot (messages with `parent_id`/`is_active`,
  events with global `id` cursors; image attachment bytes are omitted and
  replaced by `content_url` metadata)
- `GET /sessions/{id}/attachments/{attachment_id}` authenticated raw image
  content for an image attachment
- `POST /sessions/{id}/runs` start a run (`{"session_id":..., "message":...}`);
  image attachments use `kind: "image"` with a normalized
  `data:image/*;base64,...` payload and require a profile with `vision: true`
- `POST /sessions/{id}/select` switch the active branch (`{"run_id":...}`)
- `GET /sessions/{id}/events` SSE stream; replays ledger rows after
  `Last-Event-ID` or `?after=<row>`, then continues live. Every data frame
  carries an `id:` line.
- `POST /runs/{id}/commands` `{"command": "cancel|approve|deny"}`
- `GET /health`
- `GET /profiles`, `POST /profiles`, `POST /profiles/{id}` profile management;
  profiles include the `vision` capability flag

The web client targets `localhost:8787` in `http` mode
(`VITE_HARNESS_MODE=http`). `start.cmd` (Windows) and `start.sh`
(Linux/macOS) at the repository root start the daemon and the web
server in a single window, prod bundle by default (`--dev` restores
the dev server); the daemon stops when the web server exits.

## build

CMake with a native toolchain:

```sh
cmake -S harness -B harness/build
cmake --build harness/build
ctest --test-dir harness/build --output-on-failure
```

Zig (no other toolchain required; works on Windows without MSVC/MinGW on
PATH). SQLite compiles as C, and the SIMD variants each need their own -m
flag, so build those objects first (see the daemon recipe above), then link
them:

```sh
zig c++ -std=c++20 -O2 -I harness/include -I harness/third_party/sqlite \
    harness/src/core.cpp harness/src/json.cpp harness/src/http.cpp \
    harness/src/memory.cpp harness/src/memory_custom.cpp \
    harness/src/memorydb.cpp harness/src/promptpacks.cpp \
    harness/src/provider.cpp harness/src/qwen3.cpp harness/src/skills.cpp \
    harness/src/store.cpp harness/src/summary.cpp \
    build/qwen3_sse2.o build/qwen3_avx.o build/qwen3_avx2.o \
    build/nyaxus_sqlite3.o harness/src/main.cpp \
    -lws2_32 -o harness/nyaxus_demo.exe
zig c++ -std=c++20 -O2 -I harness/include -I harness/third_party/sqlite \
    harness/src/core.cpp harness/src/json.cpp harness/src/http.cpp \
    harness/src/memory.cpp harness/src/memory_custom.cpp \
    harness/src/memorydb.cpp harness/src/promptpacks.cpp \
    harness/src/provider.cpp harness/src/qwen3.cpp harness/src/skills.cpp \
    harness/src/store.cpp harness/src/summary.cpp \
    build/qwen3_sse2.o build/qwen3_avx.o build/qwen3_avx2.o \
    build/nyaxus_sqlite3.o harness/tests/core_smoke.cpp \
    -lws2_32 -o harness/nyaxus_core_smoke.exe
harness/nyaxus_core_smoke.exe
```

End-to-end SSE resume check against a running daemon (fake provider):

```sh
harness/tests/sse_resume_test.sh harness/nyaxus_daemon.exe
```

Run the demo (`--ledger` is the SQLite store path):

```sh
harness/nyaxus_demo.exe --ledger harness/nyaxus-events.db
```
