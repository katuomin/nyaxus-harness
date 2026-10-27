// daemon_config.cpp - the daemon's config, typed settings registry, and
// path/OS utils (split from daemon.cpp; see docs/daemon-split-analysis.md).
// Declarations live in include/nyaxus/daemon.hpp; this file carries only the
// definitions.
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "nyaxus/daemon.hpp"
#include "nyaxus/provider.hpp"

namespace nyaxus {

// Forward-declare the Win32 call so we can locate the executable without
// pulling in <windows.h> (whose min/max macros would collide with std::min).
// The types must match the zig libc header exactly: HMODULE is the opaque
// HINSTANCE__ handle and DWORD is unsigned long on Windows.
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long GetModuleFileNameA(
    struct HINSTANCE__* hModule, char* lpFilename, unsigned long nSize);
#endif

// Directory containing the running executable ("" if it cannot be determined).
// Anchors bare model filenames to the daemon install so the same config works
// no matter which directory the daemon is launched from.
std::string exe_directory() {
    std::string path;
#if defined(_WIN32)
    char buf[4096];
    const unsigned long n = GetModuleFileNameA(nullptr, buf, sizeof(buf) - 1);
    if (n > 0 && n < sizeof(buf)) { buf[n] = '\0'; path = buf; }
#else
    char buf[4096];
    const long n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0 && n < sizeof(buf)) { buf[n] = '\0'; path = buf; }
#endif
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "" : path.substr(0, pos);
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// Resolve a configured model path to an existing file. Accepts an absolute
// path, a CWD-relative path, or a bare filename. Bare filenames are looked up
// under the conventional models directory (models/nyx) relative to the daemon
// executable and its parent, so a single config works regardless of launch
// directory. Returns the resolved path, or "" with `error` set.
std::string resolve_model_path(const std::string& configured, std::string& error) {
    if (configured.empty()) { error = "model path is empty"; return ""; }
    if (file_exists(configured)) return configured;  // absolute or CWD-relative
    const bool bare = configured.find('/') == std::string::npos &&
                      configured.find('\\') == std::string::npos;
    std::vector<std::string> bases = {"."};
    const std::string exe_dir = exe_directory();
    if (!exe_dir.empty()) bases.push_back(exe_dir);
    if (!exe_dir.empty()) bases.push_back(exe_dir + "/..");
    std::vector<std::string> tried;
    for (const std::string& base : bases) {
        if (bare) {
            const std::string cand = base + "/models/nyx/" + configured;
            tried.push_back(cand);
            if (file_exists(cand)) return cand;
        }
        const std::string cand = base + "/" + configured;
        tried.push_back(cand);
        if (file_exists(cand)) return cand;
    }
    std::string joined;
    for (std::size_t i = 0; i < tried.size(); ++i) {
        if (i) joined += ", ";
        joined += tried[i];
    }
    error = "model not found (tried: " + joined + ")";
    return "";
}

// Fixed-width ISO-UTC (same shape as the store's now_utc) so the outbox's
// lexicographic due-date comparison stays correct.
std::string utc_timestamp(std::chrono::system_clock::time_point when) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) % 1000;
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(3)
        << std::setfill('0') << milliseconds.count() << "Z";
    return out.str();
}

// A human UTC stamp for a turn, from the ledger's created_at (epoch
// nanoseconds, microseconds, milliseconds, seconds, or ISO-8601). Grounds
// the journey historian's segment dates in each turn's actual time. Returns
// "" when the value is not a recognizable timestamp.
std::string turn_stamp(const std::string& created_at) {
    std::time_t seconds = 0;
    bool ok = false;
    if (!created_at.empty() && created_at[0] >= '0' && created_at[0] <= '9') {
        bool all_digits = true;
        for (const char c : created_at) {
            if (c < '0' || c > '9') {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            try {
                const unsigned long long value = std::stoull(created_at);
                const std::size_t n = created_at.size();
                if (n >= 18) {
                    seconds = static_cast<std::time_t>(value / 1000000000ULL);
                    ok = true;
                } else if (n >= 16) {
                    seconds = static_cast<std::time_t>(value / 1000000ULL);
                    ok = true;
                } else if (n == 13) {
                    seconds = static_cast<std::time_t>(value / 1000ULL);
                    ok = true;
                } else if (n == 10) {
                    seconds = static_cast<std::time_t>(value);
                    ok = true;
                }
            } catch (...) {
            }
        }
    }
    if (!ok && created_at.size() >= 16 && created_at[10] == 'T') {
        // ISO-8601 "YYYY-MM-DDTHH:MM:SS..." - keep date + time as-is.
        return created_at.substr(0, 10) + " " + created_at.substr(11, 5) + " UTC";
    }
    if (!ok) return "";
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &utc);
    return buf;
}

Config load_config() {
    Config config;
    const auto env = [](const char* name, const std::string& fallback) {
        const char* value = std::getenv(name);
        return value ? std::string(value) : fallback;
    };
    const auto env_flag = [&](const char* name, bool fallback) {
        const std::string value = env(name, fallback ? "1" : "0");
        return value == "1" || value == "true" || value == "TRUE" || value == "yes";
    };
    const auto env_source = [&](const char* name, const char* key) {
        const char* value = std::getenv(name);
        if (value && *value) config.env_sources.insert(key);
    };
    try {
        config.port = std::stoi(env("NYAXUS_PORT", "8787"));
    } catch (...) {
        config.port = 8787;
    }
    env_source("NYAXUS_PORT", "port");
    config.workspace = env("NYAXUS_WORKSPACE", ".");
    env_source("NYAXUS_WORKSPACE", "workspace");
    config.password = env("NYAXUS_PASSWORD", "");
    env_source("NYAXUS_PASSWORD", "password");
    config.provider_type = env("NYAXUS_PROVIDER", "fake");
    env_source("NYAXUS_PROVIDER", "provider");
    config.provider.endpoint = env("NYAXUS_LLAMA_URL", "");
    env_source("NYAXUS_LLAMA_URL", "provider.endpoint");
    config.provider.model = env("NYAXUS_MODEL", "");
    env_source("NYAXUS_MODEL", "provider.model");
    config.provider.api_key = env("NYAXUS_API_KEY", "");
    env_source("NYAXUS_API_KEY", "provider.api_key");
    try {
        config.provider.temperature = std::stod(env("NYAXUS_TEMPERATURE", "0.7"));
    } catch (...) {
        config.provider.temperature = 0.7;
    }
    env_source("NYAXUS_TEMPERATURE", "provider.temperature");
    try {
        config.provider.max_tokens = std::stoi(env("NYAXUS_MAX_TOKENS", "0"));
    } catch (...) {
        config.provider.max_tokens = 0;
    }
    env_source("NYAXUS_MAX_TOKENS", "provider.max_tokens");
    config.default_provider = env("NYAXUS_DEFAULT_PROVIDER", "");
    env_source("NYAXUS_DEFAULT_PROVIDER", "provider.default");

    provider::Config auxiliary;
    auxiliary.endpoint = env("NYAXUS_AUXILIARY_URL", "");
    env_source("NYAXUS_AUXILIARY_URL", "auxiliary.endpoint");
    auxiliary.model = env("NYAXUS_AUXILIARY_MODEL", "");
    env_source("NYAXUS_AUXILIARY_MODEL", "auxiliary.model");
    auxiliary.api_key = env("NYAXUS_AUXILIARY_API_KEY", "");
    env_source("NYAXUS_AUXILIARY_API_KEY", "auxiliary.api_key");
    try {
        auxiliary.temperature = std::stod(env("NYAXUS_AUXILIARY_TEMPERATURE", "0.2"));
    } catch (...) {
        auxiliary.temperature = 0.2;
    }
    env_source("NYAXUS_AUXILIARY_TEMPERATURE", "auxiliary.temperature");
    try {
        auxiliary.max_tokens = std::stoi(env("NYAXUS_AUXILIARY_MAX_TOKENS", "2048"));
    } catch (...) {
        auxiliary.max_tokens = 2048;
    }
    env_source("NYAXUS_AUXILIARY_MAX_TOKENS", "auxiliary.max_tokens");
    auxiliary.vision = env_flag("NYAXUS_AUXILIARY_VISION", false);
    env_source("NYAXUS_AUXILIARY_VISION", "auxiliary.vision");
    auxiliary.no_thinking = env_flag("NYAXUS_AUXILIARY_NO_THINKING", false);
    env_source("NYAXUS_AUXILIARY_NO_THINKING", "auxiliary.no_thinking");
    if (!auxiliary.endpoint.empty() || !auxiliary.model.empty() ||
        !auxiliary.api_key.empty()) {
        config.auxiliary = auxiliary;
    }
    config.summary_include_reasoning =
        env_flag("NYAXUS_SUMMARY_INCLUDE_REASONING", false);
    env_source("NYAXUS_SUMMARY_INCLUDE_REASONING", "summary.include_reasoning");
    try {
        config.summary_source_budget_bytes =
            std::stoll(env("NYAXUS_SUMMARY_SOURCE_BUDGET_BYTES", "0"));
    } catch (...) {
        config.summary_source_budget_bytes = 0;
    }
    env_source("NYAXUS_SUMMARY_SOURCE_BUDGET_BYTES", "summary.source_budget_bytes");
    config.memory_enabled = env_flag("NYAXUS_MEMORY_ENABLED", false);
    env_source("NYAXUS_MEMORY_ENABLED", "memory.enabled");
    config.memory_provider = env("NYAXUS_MEMORY_PROVIDER", "hindsight");
    env_source("NYAXUS_MEMORY_PROVIDER", "memory.provider");
    config.memory_endpoint = env("NYAXUS_MEMORY_URL", "");
    env_source("NYAXUS_MEMORY_URL", "memory.endpoint");
    config.memory_db_path = env("NYAXUS_MEMORY_DB_PATH", "memory.db");
    env_source("NYAXUS_MEMORY_DB_PATH", "memory.db_path");
    config.memory_embed_model = env("NYAXUS_MEMORY_EMBED_MODEL", "");
    env_source("NYAXUS_MEMORY_EMBED_MODEL", "memory.embed_model");
    config.memory_rerank_model = env("NYAXUS_MEMORY_RERANK_MODEL", "");
    env_source("NYAXUS_MEMORY_RERANK_MODEL", "memory.rerank_model");
    config.memory_extract_endpoint = env("NYAXUS_MEMORY_EXTRACT_URL", "");
    env_source("NYAXUS_MEMORY_EXTRACT_URL", "memory.extract_endpoint");
    config.memory_extract_model = env("NYAXUS_MEMORY_EXTRACT_MODEL", "");
    env_source("NYAXUS_MEMORY_EXTRACT_MODEL", "memory.extract_model");
    config.memory_extract_api_key = env("NYAXUS_MEMORY_EXTRACT_API_KEY", "");
    env_source("NYAXUS_MEMORY_EXTRACT_API_KEY", "memory.extract_api_key");
    config.title_model = env("NYAXUS_TITLE_MODEL", "");
    env_source("NYAXUS_TITLE_MODEL", "title.model");
    config.title_enabled = env_flag("NYAXUS_TITLE_ENABLED", true);
    env_source("NYAXUS_TITLE_ENABLED", "title.enabled");
    try {
        config.memory_rerank_top = std::stoll(env("NYAXUS_MEMORY_RERANK_TOP", "8"));
    } catch (...) {
        config.memory_rerank_top = 8;
    }
    env_source("NYAXUS_MEMORY_RERANK_TOP", "memory.rerank_top");
    config.memory_bank = env("NYAXUS_MEMORY_BANK", "nyaxus");
    env_source("NYAXUS_MEMORY_BANK", "memory.bank");
    config.memory_api_key = env("NYAXUS_MEMORY_API_KEY", "");
    env_source("NYAXUS_MEMORY_API_KEY", "memory.api_key");
    try {
        config.memory_retain_every_n_turns =
            std::stoll(env("NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS", "1"));
    } catch (...) {
        config.memory_retain_every_n_turns = 1;
    }
    env_source("NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS", "memory.retain_every_n_turns");
    try {
        config.memory_sweep_idle_seconds =
            std::stoll(env("NYAXUS_MEMORY_SWEEP_IDLE_SECONDS", "30"));
    } catch (...) {
        config.memory_sweep_idle_seconds = 30;
    }
    env_source("NYAXUS_MEMORY_SWEEP_IDLE_SECONDS", "memory.sweep_idle_seconds");
    try {
        config.memory_sweep_cosine_threshold =
            std::stod(env("NYAXUS_MEMORY_SWEEP_COSINE_THRESHOLD", "0.92"));
    } catch (...) {
        config.memory_sweep_cosine_threshold = 0.92;
    }
    env_source("NYAXUS_MEMORY_SWEEP_COSINE_THRESHOLD", "memory.sweep_cosine_threshold");
    config.memory_recall_on_turn = env_flag("NYAXUS_MEMORY_RECALL_ON_TURN", false);
    env_source("NYAXUS_MEMORY_RECALL_ON_TURN", "memory.recall_on_turn");
    try {
        config.memory_recall_floor =
            std::stod(env("NYAXUS_MEMORY_RECALL_FLOOR", "0"));
    } catch (...) {
        config.memory_recall_floor = 0.0;
    }
    env_source("NYAXUS_MEMORY_RECALL_FLOOR", "memory.recall_floor");
    config.terminal_readonly_auto_approve =
        env_flag("NYAXUS_TERMINAL_READONLY_AUTO_APPROVE", true);
    env_source("NYAXUS_TERMINAL_READONLY_AUTO_APPROVE",
               "terminal.readonly_auto_approve");
    config.terminal_spill_full_output =
        env_flag("NYAXUS_TERMINAL_SPILL_FULL_OUTPUT", true);
    env_source("NYAXUS_TERMINAL_SPILL_FULL_OUTPUT",
               "terminal.spill_full_output");
    config.terminal_child_env = env("NYAXUS_TERMINAL_CHILD_ENV", "");
    env_source("NYAXUS_TERMINAL_CHILD_ENV", "terminal.child_env");
    config.context_microcompact =
        env_flag("NYAXUS_CONTEXT_MICROCOMPACT", true);
    env_source("NYAXUS_CONTEXT_MICROCOMPACT", "context.microcompact");
    config.web_enabled = env_flag("NYAXUS_WEB_ENABLED", true);
    env_source("NYAXUS_WEB_ENABLED", "web.enabled");
    config.web_search_endpoint =
        env("NYAXUS_WEB_SEARCH_ENDPOINT", "http://<deployment-lan>:8080");
    env_source("NYAXUS_WEB_SEARCH_ENDPOINT", "web.search_endpoint");
    try {
        config.web_fetch_timeout_ms =
            std::stoll(env("NYAXUS_WEB_FETCH_TIMEOUT_MS", "15000"));
    } catch (...) {
        config.web_fetch_timeout_ms = 15000;
    }
    env_source("NYAXUS_WEB_FETCH_TIMEOUT_MS", "web.fetch_timeout_ms");
    try {
        config.web_fetch_max_bytes =
            std::stoll(env("NYAXUS_WEB_FETCH_MAX_BYTES", "1048576"));
    } catch (...) {
        config.web_fetch_max_bytes = 1024 * 1024;
    }
    env_source("NYAXUS_WEB_FETCH_MAX_BYTES", "web.fetch_max_bytes");
    try {
        config.web_fetch_max_chars =
            std::stoll(env("NYAXUS_WEB_FETCH_MAX_CHARS", "12000"));
    } catch (...) {
        config.web_fetch_max_chars = 12000;
    }
    env_source("NYAXUS_WEB_FETCH_MAX_CHARS", "web.fetch_max_chars");
    config.web_firecrawl_endpoint =
        env("NYAXUS_WEB_FIRECRAWL_ENDPOINT", "http://<deployment-lan>:3002");
    env_source("NYAXUS_WEB_FIRECRAWL_ENDPOINT", "web.firecrawl_endpoint");
    config.web_ca_bundle = env("NYAXUS_WEB_CA_BUNDLE", "");
    env_source("NYAXUS_WEB_CA_BUNDLE", "web.ca_bundle");
    config.web_camofox_endpoint = env("NYAXUS_WEB_CAMOFOX_ENDPOINT", "");
    env_source("NYAXUS_WEB_CAMOFOX_ENDPOINT", "web.camofox_endpoint");
    config.web_camofox_token = env("NYAXUS_WEB_CAMOFOX_TOKEN", "");
    env_source("NYAXUS_WEB_CAMOFOX_TOKEN", "web.camofox_token");
    config.web_camofox_sticky = env_flag("NYAXUS_WEB_CAMOFOX_STICKY", false);
    env_source("NYAXUS_WEB_CAMOFOX_STICKY", "web.camofox_sticky");
    try {
        const long long ttl = std::stoll(env("NYAXUS_WEB_CAMOFOX_STICKY_TTL_MINUTES", "15"));
        config.web_camofox_sticky_ttl_minutes = ttl < 1 ? 15 : (ttl > 1440 ? 1440 : ttl);
    } catch (...) {
    }
    env_source("NYAXUS_WEB_CAMOFOX_STICKY_TTL_MINUTES", "web.camofox_sticky_ttl");
    return config;
}

void ensure_auxiliary_config(Config& config) {
    if (!config.auxiliary.has_value()) {
        provider::Config auxiliary;
        auxiliary.temperature = 0.2;
        auxiliary.max_tokens = 2048;
        config.auxiliary = auxiliary;
    }
}

const std::vector<SettingSpec>& settings_registry() {
    static const std::vector<SettingSpec> registry = {
        {"port", "integer", false, true,
         [](const Config& c) { return std::to_string(c.port); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long port = std::stoll(value);
                 if (port <= 0 || port > 65535) {
                     error = "must be between 1 and 65535";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.port = std::stoi(value); }},
        {"workspace", "string", false, true,
         [](const Config& c) { return c.workspace; },
         [](const std::string& value, std::string& error) {
             if (value.empty()) {
                 error = "must not be empty";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.workspace = value; }},
        {"password", "string", true, true,
         [](const Config& c) { return c.password; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.password = value; }},
        {"provider", "string", false, true,
         [](const Config& c) { return c.provider_type; },
         [](const std::string& value, std::string& error) {
             if (value != "fake" && value != "openai") {
                 error = "must be 'fake' or 'openai'";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.provider_type = value; }},
        {"provider.endpoint", "string", false, true,
         [](const Config& c) { return c.provider.endpoint; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.provider.endpoint = value; }},
        {"provider.model", "string", false, false,
         [](const Config& c) { return c.provider.model; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.provider.model = value; }},
        {"provider.api_key", "string", true, false,
         [](const Config& c) { return c.provider.api_key; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.provider.api_key = value; }},
        {"provider.temperature", "number", false, false,
         [](const Config& c) { return std::to_string(c.provider.temperature); },
         [](const std::string& value, std::string& error) {
             try {
                 const double temperature = std::stod(value);
                 if (temperature < 0.0 || temperature > 2.0) {
                     error = "must be between 0 and 2";
                     return false;
                 }
             } catch (...) {
                 error = "must be a number";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.provider.temperature = std::stod(value);
         }},
        {"provider.max_tokens", "integer", false, false,
         [](const Config& c) { return std::to_string(c.provider.max_tokens); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long tokens = std::stoll(value);
                 if (tokens < 0) {
                     error = "must be >= 0";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.provider.max_tokens = std::stoi(value);
         }},
        {"provider.context_window", "integer", false, false,
         [](const Config& c) { return std::to_string(c.provider.context_window); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long window = std::stoll(value);
                 if (window < 0) {
                     error = "must be >= 0";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.provider.context_window = std::stoi(value);
         }},
        {"provider.default", "string", false, false,
         [](const Config& c) { return c.default_provider; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.default_provider = value; }},
        {"auxiliary.endpoint", "string", false, true,
         [](const Config& c) { return c.auxiliary ? c.auxiliary->endpoint : ""; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->endpoint = value;
         }},
        {"auxiliary.model", "string", false, false,
         [](const Config& c) { return c.auxiliary ? c.auxiliary->model : ""; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->model = value;
         }},
        {"auxiliary.api_key", "string", true, false,
         [](const Config& c) { return c.auxiliary ? c.auxiliary->api_key : ""; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->api_key = value;
         }},
        {"auxiliary.temperature", "number", false, false,
         [](const Config& c) {
             return c.auxiliary ? std::to_string(c.auxiliary->temperature)
                                : std::to_string(0.2);
         },
         [](const std::string& value, std::string& error) {
             try {
                 const double temperature = std::stod(value);
                 if (temperature < 0.0 || temperature > 2.0) {
                     error = "must be between 0 and 2";
                     return false;
                 }
             } catch (...) {
                 error = "must be a number";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->temperature = std::stod(value);
         }},
        {"auxiliary.max_tokens", "integer", false, false,
         [](const Config& c) {
             return c.auxiliary ? std::to_string(c.auxiliary->max_tokens)
                                : std::to_string(2048);
         },
         [](const std::string& value, std::string& error) {
             try {
                 const long long tokens = std::stoll(value);
                 if (tokens < 0) {
                     error = "must be >= 0";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->max_tokens = std::stoi(value);
         }},
        {"auxiliary.vision", "boolean", false, false,
         [](const Config& c) {
             return (c.auxiliary ? c.auxiliary->vision : false) ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->vision = value == "true";
         }},
        {"auxiliary.no_thinking", "boolean", false, false,
         [](const Config& c) {
             return (c.auxiliary ? c.auxiliary->no_thinking : false) ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             ensure_auxiliary_config(c);
             c.auxiliary->no_thinking = value == "true";
         }},
        {"summary.include_reasoning", "boolean", false, false,
         [](const Config& c) {
             return c.summary_include_reasoning ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.summary_include_reasoning = value == "true";
         }},
        {"summary.source_budget_bytes", "integer", false, false,
         [](const Config& c) { return std::to_string(c.summary_source_budget_bytes); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long bytes = std::stoll(value);
                 if (bytes < 0) {
                     error = "must be >= 0";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.summary_source_budget_bytes = std::stoll(value);
         }},
        {"tool.timeout_ms", "integer", false, false,
         [](const Config& c) { return std::to_string(c.tool_timeout_ms); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long timeout = std::stoll(value);
                 if (timeout < 0 || timeout > 3600000) {
                     error = "must be 0 (no timeout) or between 1 and 3600000";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.tool_timeout_ms = std::stoll(value);
         }},
        {"memory.enabled", "boolean", false, false,
         [](const Config& c) {
             return c.memory_enabled ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_enabled = value == "true";
         }},
        {"memory.endpoint", "string", false, false,
         [](const Config& c) { return c.memory_endpoint; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_endpoint = value; }},
        {"memory.bank", "string", false, false,
         [](const Config& c) { return c.memory_bank; },
         [](const std::string& value, std::string& error) {
             if (value.empty()) {
                 error = "must not be empty";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.memory_bank = value; }},
        {"memory.api_key", "string", true, false,
         [](const Config& c) { return c.memory_api_key; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_api_key = value; }},
        {"memory.retain_every_n_turns", "integer", false, false,
         [](const Config& c) { return std::to_string(c.memory_retain_every_n_turns); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long turns = std::stoll(value);
                 if (turns < 1) {
                     error = "must be >= 1";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_retain_every_n_turns = std::stoll(value);
         }},
        {"memory.sweep_idle_seconds", "integer", false, false,
         [](const Config& c) {
             return std::to_string(c.memory_sweep_idle_seconds);
         },
         [](const std::string& value, std::string& error) {
             try {
                 const long long seconds = std::stoll(value);
                 if (seconds < 1) {
                     error = "must be >= 1";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_sweep_idle_seconds = std::stoll(value);
         }},
        {"memory.sweep_cosine_threshold", "number", false, false,
         [](const Config& c) {
             std::ostringstream out;
             out << std::setprecision(4) << c.memory_sweep_cosine_threshold;
             return out.str();
         },
         [](const std::string& value, std::string& error) {
             try {
                 const double threshold = std::stod(value);
                 if (threshold <= 0.0 || threshold > 1.0) {
                     error = "must be in (0, 1]";
                     return false;
                 }
             } catch (...) {
                 error = "must be a number";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_sweep_cosine_threshold = std::stod(value);
         }},
        {"memory.recall_on_turn", "boolean", false, false,
         [](const Config& c) {
             return c.memory_recall_on_turn ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_recall_on_turn = value == "true";
         }},
        {"memory.recall_floor", "number", false, false,
         [](const Config& c) {
             std::ostringstream out;
             out << std::setprecision(4) << c.memory_recall_floor;
             return out.str();
         },
         [](const std::string& value, std::string& error) {
             try {
                 const double floor = std::stod(value);
                 if (floor < 0.0 || floor > 10.0) {
                     error = "must be in [0, 10] (rerank-logit scale; 0 = off)";
                     return false;
                 }
             } catch (...) {
                 error = "must be a number";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_recall_floor = std::stod(value);
         }},
        {"terminal.readonly_auto_approve", "boolean", false, false,
         [](const Config& c) {
             return c.terminal_readonly_auto_approve ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.terminal_readonly_auto_approve = value == "true";
         }},
        {"terminal.child_env", "string", false, false,
         [](const Config& c) { return c.terminal_child_env; },
         [](const std::string& value, std::string& error) {
             // Comma-separated environment-variable names, identifier-shaped.
             std::size_t begin = 0;
             while (begin <= value.size()) {
                 const std::size_t comma = value.find(',', begin);
                 std::string name = value.substr(
                     begin,
                     comma == std::string::npos ? std::string::npos
                                               : comma - begin);
                 while (!name.empty() &&
                        (name.front() == ' ' || name.front() == '"'))
                     name.erase(0, 1);
                 while (!name.empty() &&
                        (name.back() == ' ' || name.back() == '"'))
                     name.pop_back();
                 const bool valid =
                     !name.empty() &&
                     (std::isalpha(static_cast<unsigned char>(name[0])) ||
                      name[0] == '_') &&
                     std::all_of(name.begin() + 1, name.end(), [](char ch) {
                         return std::isalnum(static_cast<unsigned char>(ch)) ||
                                ch == '_';
                     });
                 if (!valid) {
                     error = "must be a comma-separated list of variable "
                             "names (invalid entry: " + name + ")";
                     return false;
                 }
                 if (comma == std::string::npos) break;
                 begin = comma + 1;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.terminal_child_env = value;
         }},
        {"terminal.spill_full_output", "boolean", false, false,
         [](const Config& c) {
             return c.terminal_spill_full_output ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.terminal_spill_full_output = value == "true";
         }},
        {"terminal.sudo_ttl_minutes", "integer", false, false,
         [](const Config& c) {
             return std::to_string(c.terminal_sudo_ttl_minutes);
         },
         [](const std::string& value, std::string& error) {
             try {
                 const long long minutes = std::stoll(value);
                 if (minutes < 1 || minutes > 120) {
                     error = "must be between 1 and 120";
                     return false;
                 }
                 return true;
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
         },
         [](Config& c, const std::string& value) {
             c.terminal_sudo_ttl_minutes =
                 static_cast<int>(std::stoll(value));
         }},
        {"context.microcompact", "boolean", false, false,
         [](const Config& c) {
             return c.context_microcompact ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.context_microcompact = value == "true";
         }},
        {"run.stall_timeout_s", "integer", false, false,
         [](const Config& c) { return std::to_string(c.run_stall_timeout_s); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long seconds = std::stoll(value);
                 if (seconds < 0 || seconds > 3600) {
                     error = "must be between 0 and 3600 (0 disables the watchdog)";
                     return false;
                 }
                 return true;
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
         },
         [](Config& c, const std::string& value) {
             c.run_stall_timeout_s = std::stoll(value);
         }},
        {"approval.ttl_s", "integer", false, false,
         [](const Config& c) { return std::to_string(c.approval_ttl_s); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long seconds = std::stoll(value);
                 if (seconds < 0 || seconds > 3600) {
                     error = "must be between 0 and 3600 (0 uses the legacy 600s cap)";
                     return false;
                 }
                 return true;
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
         },
         [](Config& c, const std::string& value) {
             c.approval_ttl_s = std::stoll(value);
         }},
        {"spawn.deadline_s", "integer", false, false,
         [](const Config& c) { return std::to_string(c.spawn_deadline_s); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long seconds = std::stoll(value);
                 if (seconds < 0 || seconds > 3600) {
                     error = "must be between 0 and 3600 (0 removes the deadline)";
                     return false;
                 }
                 return true;
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
         },
         [](Config& c, const std::string& value) {
             c.spawn_deadline_s = std::stoll(value);
         }},
        {"group.max_rounds", "integer", false, false,
         [](const Config& c) { return std::to_string(c.group_max_rounds); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long rounds = std::stoll(value);
                 if (rounds < 1 || rounds > 20) {
                     error = "must be between 1 and 20";
                     return false;
                 }
                 return true;
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
         },
         [](Config& c, const std::string& value) {
             c.group_max_rounds = std::stoll(value);
         }},
        {"web.enabled", "boolean", false, false,
         [](const Config& c) {
             return c.web_enabled ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.web_enabled = value == "true"; }},
        {"web.search_endpoint", "string", false, false,
         [](const Config& c) { return c.web_search_endpoint; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.web_search_endpoint = value; }},
        {"web.fetch_timeout_ms", "integer", false, false,
         [](const Config& c) { return std::to_string(c.web_fetch_timeout_ms); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long timeout = std::stoll(value);
                 if (timeout < 1000 || timeout > 300000) {
                     error = "must be between 1000 and 300000";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.web_fetch_timeout_ms = std::stoll(value);
         }},
        {"web.fetch_max_bytes", "integer", false, false,
         [](const Config& c) { return std::to_string(c.web_fetch_max_bytes); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long bytes = std::stoll(value);
                 if (bytes < 0 || bytes > 64 * 1024 * 1024) {
                     error = "must be between 0 and 67108864";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.web_fetch_max_bytes = std::stoll(value);
         }},
        {"web.fetch_max_chars", "integer", false, false,
         [](const Config& c) { return std::to_string(c.web_fetch_max_chars); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long chars = std::stoll(value);
                 if (chars < 0 || chars > 200000) {
                     error = "must be between 0 and 200000";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.web_fetch_max_chars = std::stoll(value);
         }},
        {"web.firecrawl_endpoint", "string", false, false,
         [](const Config& c) { return c.web_firecrawl_endpoint; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.web_firecrawl_endpoint = value; }},
        {"web.ca_bundle", "string", false, false,
         [](const Config& c) { return c.web_ca_bundle; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.web_ca_bundle = value; }},
        {"web.camofox_endpoint", "string", false, false,
         [](const Config& c) { return c.web_camofox_endpoint; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.web_camofox_endpoint = value; }},
        {"web.camofox_token", "string", false, false,
         [](const Config& c) { return c.web_camofox_token; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.web_camofox_token = value; }},
        {"web.camofox_sticky", "boolean", false, false,
         [](const Config& c) {
             return c.web_camofox_sticky ? "true" : "false";
         },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.web_camofox_sticky = value == "true"; }},
        {"web.camofox_sticky_ttl", "integer", false, false,
         [](const Config& c) { return std::to_string(c.web_camofox_sticky_ttl_minutes); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long ttl = std::stoll(value);
                 if (ttl < 0 || ttl > 1440) {
                     error = "must be between 0 and 1440";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.web_camofox_sticky_ttl_minutes = std::stoll(value); }},
        {"memory.provider", "string", false, false,
         [](const Config& c) { return c.memory_provider; },
         [](const std::string& value, std::string& error) {
             if (value != "hindsight" && value != "custom") {
                 error = "must be 'hindsight' or 'custom'";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.memory_provider = value; }},
        {"memory.db_path", "string", false, false,
         [](const Config& c) { return c.memory_db_path; },
         [](const std::string& value, std::string& error) {
             if (value.empty()) {
                 error = "must not be empty";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.memory_db_path = value; }},
        {"memory.embed_model", "string", false, false,
         [](const Config& c) { return c.memory_embed_model; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_embed_model = value; }},
        {"memory.rerank_model", "string", false, false,
         [](const Config& c) { return c.memory_rerank_model; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_rerank_model = value; }},
        {"memory.extract_endpoint", "string", false, false,
         [](const Config& c) { return c.memory_extract_endpoint; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_extract_endpoint = value; }},
        {"memory.extract_model", "string", false, false,
         [](const Config& c) { return c.memory_extract_model; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_extract_model = value; }},
        {"memory.extract_api_key", "string", true, false,
         [](const Config& c) { return c.memory_extract_api_key; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.memory_extract_api_key = value; }},
        {"title.enabled", "boolean", false, false,
         [](const Config& c) { return c.title_enabled ? "true" : "false"; },
         [](const std::string& value, std::string& error) {
             if (value != "true" && value != "false") {
                 error = "must be true or false";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) { c.title_enabled = value == "true"; }},
        {"title.model", "string", false, false,
         [](const Config& c) { return c.title_model; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.title_model = value; }},
        {"memory.rerank_top", "integer", false, false,
         [](const Config& c) { return std::to_string(c.memory_rerank_top); },
         [](const std::string& value, std::string& error) {
             try {
                 const long long top = std::stoll(value);
                 if (top < 1 || top > 64) {
                     error = "must be between 1 and 64";
                     return false;
                 }
             } catch (...) {
                 error = "must be an integer";
                 return false;
             }
             return true;
         },
         [](Config& c, const std::string& value) {
             c.memory_rerank_top = std::stoll(value);
         }},
        {"infra.containers", "string", false, false,
         [](const Config& c) { return c.infra_container_filter; },
         [](const std::string&, std::string&) { return true; },
         [](Config& c, const std::string& value) { c.infra_container_filter = value; }},
    };
    return registry;
}

const SettingSpec* find_setting(const std::string& key) {
    for (const SettingSpec& spec : settings_registry()) {
        if (spec.key == key) return &spec;
    }
    return nullptr;
}

}  // namespace nyaxus
