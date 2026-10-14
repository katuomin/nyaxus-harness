cmake_minimum_required(VERSION 3.20)

project(nyaxus_harness VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored SQLite amalgamation keeps the build self-contained and portable
# across Windows/MinGW, Linux, and Zig cross-compilation.
set(NYAXUS_SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)

add_library(nyaxus_sqlite STATIC
    ${NYAXUS_SQLITE_DIR}/sqlite3.c
)
target_include_directories(nyaxus_sqlite PUBLIC ${NYAXUS_SQLITE_DIR})
# The vendored amalgamation compiles FTS5 only when requested (the standard
# amalgamation ships it on by default). PUBLIC so every TU that includes
# sqlite3.h sees the same FTS5 API surface.
target_compile_definitions(nyaxus_sqlite PUBLIC SQLITE_ENABLE_FTS5)
find_package(Threads REQUIRED)
target_link_libraries(nyaxus_sqlite PUBLIC Threads::Threads)

# SIMD matvec variants for the Qwen3 engine, each compiled with its own -m
# flag and selected at runtime via cpuid (SSE2 is the x86-64 baseline; the
# 256-bit path needs only AVX+FMA, not AVX2).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i686)$")
    add_library(nyaxus_simd_sse2 STATIC src/qwen3_sse2.cpp)
    add_library(nyaxus_simd_avx STATIC src/qwen3_avx.cpp)
    if(MSVC)
        # MSVC gates FMA intrinsics behind /arch:AVX2.
        target_compile_options(nyaxus_simd_avx PRIVATE /arch:AVX2)
    else()
        target_compile_options(nyaxus_simd_avx PRIVATE -mavx -mfma)
    endif()
    # AVX2 integer GEMM (vpmaddubsw/vpmaddwd); runtime-gated by cpuid.
    add_library(nyaxus_simd_avx2 STATIC src/qwen3_avx2.cpp)
    if(MSVC)
        target_compile_options(nyaxus_simd_avx2 PRIVATE /arch:AVX2)
    else()
        target_compile_options(nyaxus_simd_avx2 PRIVATE -mavx2 -mfma)
    endif()
endif()

add_library(nyaxus_core STATIC
    src/argvpolicy.cpp
    src/commandplane.cpp
    src/core.cpp
    src/denyguard.cpp
    src/fingerprint.cpp
    src/frozen.cpp
    src/gatepolicy.cpp
    src/json.cpp
    src/http.cpp
    src/infra.cpp
    src/loopguard.cpp
    src/memory.cpp
    src/memory_custom.cpp
    src/memorydb.cpp
    src/promptpacks.cpp
    src/provider.cpp
    src/qwen3.cpp
    src/store.cpp
    src/streamrules.cpp
    src/summary.cpp
    src/skills.cpp
)

target_include_directories(nyaxus_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nyaxus_core PUBLIC nyaxus_sqlite)

# Vendored TLS + gzip for the native https fetch: mbedtls + miniz. The
# default config is used; NYAXUS_HAS_MBEDTLS turns the http::Transport TLS
# path on. Windows also needs bcrypt (mbedtls entropy seeding).
file(GLOB NYAXUS_MBEDTLS_SOURCES CONFIGURE_DEPENDS
     third_party/mbedtls/library/*.c)
file(GLOB NYAXUS_MINIZ_SOURCES CONFIGURE_DEPENDS third_party/miniz/*.c)
add_library(nyaxus_tls STATIC ${NYAXUS_MBEDTLS_SOURCES}
                              ${NYAXUS_MINIZ_SOURCES})
target_include_directories(nyaxus_tls PUBLIC third_party/mbedtls/include
                                             third_party/miniz)
target_link_libraries(nyaxus_tls PUBLIC Threads::Threads)
if(WIN32)
    # ws2_32 must link AFTER the TLS archive: mbedtls's x509_crt references
    # inet_pton, and static link order is single-pass. Declaring it on
    # nyaxus_tls (not nyaxus_core) keeps it at the end of the link line.
    target_link_libraries(nyaxus_tls PUBLIC bcrypt ws2_32)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i686)$")
    target_link_libraries(nyaxus_core PUBLIC nyaxus_simd_sse2 nyaxus_simd_avx
                                            nyaxus_simd_avx2)
endif()

if(MSVC)
    target_compile_options(nyaxus_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(nyaxus_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(nyaxus_daemon
    src/daemon.cpp
    src/daemon_config.cpp
    src/daemon_http_util.cpp
    src/daemon_summary.cpp
)
target_compile_definitions(nyaxus_core PUBLIC NYAXUS_HAS_MBEDTLS)
target_link_libraries(nyaxus_core PUBLIC nyaxus_tls)
target_link_libraries(nyaxus_daemon PRIVATE nyaxus_core)

add_executable(nyaxus_demo
    src/main.cpp
)
target_link_libraries(nyaxus_demo PRIVATE nyaxus_core)

enable_testing()

add_executable(nyaxus_core_smoke
    tests/core_smoke.cpp
)
target_link_libraries(nyaxus_core_smoke PRIVATE nyaxus_core)
add_test(NAME nyaxus_core_smoke COMMAND nyaxus_core_smoke)

add_executable(nyaxus_memory_outbox
    tests/memory_outbox.cpp
)
target_link_libraries(nyaxus_memory_outbox PRIVATE nyaxus_core)
add_test(NAME nyaxus_memory_outbox COMMAND nyaxus_memory_outbox)

add_executable(nyaxus_memory_lifecycle
    tests/memory_lifecycle.cpp
)
target_link_libraries(nyaxus_memory_lifecycle PRIVATE nyaxus_core)
add_test(NAME nyaxus_memory_lifecycle COMMAND nyaxus_memory_lifecycle)

# Smoke test against the INT8-quantized artifacts (the only .nyx set kept
# on disk; the BF16 originals were removed for space). If a BF16 reranker
# is present, NYAXUS_RERANK_REF_MODEL re-enables the rank-stability gate
# (top-1 agreement, bounded score deltas, Spearman rho) against the
# higher-precision reference.
add_executable(nyaxus_qwen3_smoke
    tests/qwen3_smoke.cpp
)
target_link_libraries(nyaxus_qwen3_smoke PRIVATE nyaxus_core)
add_test(NAME nyaxus_qwen3_smoke COMMAND nyaxus_qwen3_smoke)
set_tests_properties(nyaxus_qwen3_smoke PROPERTIES ENVIRONMENT
    "NYAXUS_EMBED_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-embedding-0.6b-int8.nyx;NYAXUS_RERANK_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-reranker-0.6b-int8.nyx")

add_executable(nyaxus_memory_custom
    tests/memory_custom.cpp
)
target_link_libraries(nyaxus_memory_custom PRIVATE nyaxus_core)
add_test(NAME nyaxus_memory_custom COMMAND nyaxus_memory_custom)
set_tests_properties(nyaxus_memory_custom PROPERTIES ENVIRONMENT
    "NYAXUS_EMBED_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-embedding-0.6b-int8.nyx;NYAXUS_RERANK_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-reranker-0.6b-int8.nyx")

# Recall-quality eval fixture: seeded bank + paraphrased queries, recall@5
# gate (memory-compare-2026-10 backlog item 1). The out-of-domain probes
# print the null baseline used to calibrate memory.recall_floor.
add_executable(nyaxus_memory_recall_eval
    tests/memory_recall_eval.cpp
)
target_link_libraries(nyaxus_memory_recall_eval PRIVATE nyaxus_core)
add_test(NAME nyaxus_memory_recall_eval COMMAND nyaxus_memory_recall_eval)
set_tests_properties(nyaxus_memory_recall_eval PROPERTIES ENVIRONMENT
    "NYAXUS_EMBED_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-embedding-0.6b-int8.nyx;NYAXUS_RERANK_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-reranker-0.6b-int8.nyx")

# Loop guard: inert-command classifier, canonical args, and the
# sliding-window repeat verdicts (pure logic, no model artifacts).
add_executable(nyaxus_loopguard
    tests/loopguard_test.cpp
)
target_link_libraries(nyaxus_loopguard PRIVATE nyaxus_core)
add_test(NAME nyaxus_loopguard COMMAND nyaxus_loopguard)

# Stream rules: dormant regex rules over the streamed assistant text
# (pure logic, no model artifacts).
add_executable(nyaxus_streamrules
    tests/streamrules_test.cpp
)
target_link_libraries(nyaxus_streamrules PRIVATE nyaxus_core)
add_test(NAME nyaxus_streamrules COMMAND nyaxus_streamrules)

# Read-only argv policy: the fail-closed terminal auto-approve
# classifier (pure logic, no model artifacts).
add_executable(nyaxus_argvpolicy tests/argvpolicy_test.cpp)
target_link_libraries(nyaxus_argvpolicy PRIVATE nyaxus_core)
add_test(NAME nyaxus_argvpolicy COMMAND nyaxus_argvpolicy)

# Fingerprint rulebook: argument-fingerprint derivation and matching for
# the tiered always-allow (pure logic, no model artifacts).
add_executable(nyaxus_fingerprint tests/fingerprint_test.cpp)
target_link_libraries(nyaxus_fingerprint PRIVATE nyaxus_core)
add_test(NAME nyaxus_fingerprint COMMAND nyaxus_fingerprint)

# Structured permission decisions: the risk/side-effect taxonomy attached
# to every gate decision (pure logic, no model artifacts).
add_executable(nyaxus_gatepolicy tests/gatepolicy_test.cpp)
target_link_libraries(nyaxus_gatepolicy PRIVATE nyaxus_core)
add_test(NAME nyaxus_gatepolicy COMMAND nyaxus_gatepolicy)

# Frozen sections: the "these exact spans must survive" invariant for AI
# self-edits - marker parsing and the verbatim survival check (pure logic,
# no model artifacts).
add_executable(nyaxus_frozen tests/frozen_test.cpp)
target_link_libraries(nyaxus_frozen PRIVATE nyaxus_core)
add_test(NAME nyaxus_frozen COMMAND nyaxus_frozen)

# Deny-only monotonic guards: the guard layer in front of the approval
# waterfall (destructive argv + secret arguments; can only deny, never
# allow - pure logic, no model artifacts).
add_executable(nyaxus_denyguard tests/denyguard_test.cpp)
target_link_libraries(nyaxus_denyguard PRIVATE nyaxus_core)
add_test(NAME nyaxus_denyguard COMMAND nyaxus_denyguard)

# Skill envelope + catalog spec: strict frontmatter validation, XML
# escaping, the <skill_content> envelope (tag neutralization), and the
# fixed 50KB catalog budget (pure logic, no model artifacts).
add_executable(nyaxus_skillenv tests/skillenv_test.cpp)
target_link_libraries(nyaxus_skillenv PRIVATE nyaxus_core)
add_test(NAME nyaxus_skillenv COMMAND nyaxus_skillenv)

# Command plane: the pure drain evaluator (dedupe -> Skip, TTL -> Expired,
# newer-interrupt-supersedes, interrupt-on-past-turn -> Superseded, else
# Execute) for the idempotent, restart-safe command queue (pure logic).
add_executable(nyaxus_commandplane tests/commandplane_test.cpp)
target_link_libraries(nyaxus_commandplane PRIVATE nyaxus_core)
add_test(NAME nyaxus_commandplane COMMAND nyaxus_commandplane)

# Prompt packs: durable pack/file storage, manifest validation, plan
# validation, and exact-prompt resolution (no model artifacts needed).
add_executable(nyaxus_promptpacks
    tests/promptpacks.cpp
)
target_link_libraries(nyaxus_promptpacks PRIVATE nyaxus_core)
add_test(NAME nyaxus_promptpacks COMMAND nyaxus_promptpacks)

# Unit test for the AVX2 int16×int8 GEMM: scalar reference vs naive FP32
# (algorithm accuracy) and, on AVX2 machines, bit-identity of the SIMD
# kernel against the scalar reference.
add_executable(nyaxus_i16i8_ref
    tests/i16i8_ref.cpp
)
target_link_libraries(nyaxus_i16i8_ref PRIVATE nyaxus_core)
add_test(NAME nyaxus_i16i8_ref COMMAND nyaxus_i16i8_ref)

# Reference test for the SIMD softmax/SwiGLU fast-exp kernels vs scalar expf.
add_executable(nyaxus_simd_exp_ref
    tests/simd_exp_ref.cpp
)
target_link_libraries(nyaxus_simd_exp_ref PRIVATE nyaxus_core)
add_test(NAME nyaxus_simd_exp_ref COMMAND nyaxus_simd_exp_ref)

# KV-cache correctness: split-sequence decode must be bit-identical to a
# single-pass forward (same attended set, same op order).
add_executable(nyaxus_kvcache_ref
    tests/kvcache_ref.cpp
)
target_link_libraries(nyaxus_kvcache_ref PRIVATE nyaxus_core)
add_test(NAME nyaxus_kvcache_ref COMMAND nyaxus_kvcache_ref)
set_tests_properties(nyaxus_kvcache_ref PROPERTIES ENVIRONMENT
    "NYAXUS_EMBED_MODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/nyx/qwen3-embedding-0.6b-int8.nyx")

# First-step digit block in generate(): a synthetic model whose greedy first
# token is a digit-leading one must pick the next-best token when
# no_leading_digit is set (model-independent, no .nyx load).
add_executable(nyaxus_digit_mask_ref
    tests/digit_mask_ref.cpp
)
target_link_libraries(nyaxus_digit_mask_ref PRIVATE nyaxus_core)
add_test(NAME nyaxus_digit_mask_ref COMMAND nyaxus_digit_mask_ref)

if(NOT MSVC)
    # Embed the MinGW C++ runtime so the executables do not depend on a
    # specific libstdc++/libgcc DLL version being first on the loader path.
    # The daemon uses -static (not just -static-libgcc -static-libstdc++)
    # because it also needs libwinpthread; a stale libwinpthread-1.dll earlier
    # on the loader path breaks the clock_gettime64 entry point at launch.
    target_link_options(nyaxus_daemon PRIVATE -static)
    target_link_options(nyaxus_demo PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_core_smoke PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_memory_outbox PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_memory_lifecycle PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_qwen3_smoke PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_memory_custom PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_memory_recall_eval PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_loopguard PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_streamrules PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_argvpolicy PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_fingerprint PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_gatepolicy PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_frozen PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_denyguard PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_skillenv PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_commandplane PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_promptpacks PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_i16i8_ref PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_kvcache_ref PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_digit_mask_ref PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(nyaxus_simd_exp_ref PRIVATE -static-libgcc -static-libstdc++)
endif()