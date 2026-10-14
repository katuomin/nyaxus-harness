#include "nyaxus/infra.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "nyaxus/http.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nyaxus {
namespace infra {
namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trim(const std::string& text) {
    std::string out(text);
    while (!out.empty() && (out.front() == ' ' || out.front() == '\n' ||
                            out.front() == '\r' || out.front() == '\t')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n' ||
                            out.back() == '\r' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

// ── bounded shell round-trip ────────────────────────────────────────────
// The docker snapshot is one `sh -c` with a hard deadline: a wedged dockerd
// must not wedge the daemon (the terminal tool's spawn machinery is too
// heavy for a 5 s poll helper). POSIX: fork + pipe + poll against the
// deadline, SIGKILL on overrun. Windows: CreateProcessW + pipe,
// TerminateProcess on overrun.
struct Captured {
    bool ok = false;
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
};

#if defined(_WIN32)
Captured run_shell(const std::string& command, long long timeout_ms) {
    Captured result;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return result;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // cmd /d /c: /d skips AUTORUN (a hook there could hang the snapshot).
    std::wstring line = L"cmd.exe /d /c \"\"";
    for (const char c : command) line += static_cast<wchar_t>(c);
    line += L"\"";
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return result;
    }
    CloseHandle(pi.hThread);
    CloseHandle(write_end);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        const DWORD wait =
            WaitForSingleObject(read_end, static_cast<DWORD>(remaining));
        if (wait == WAIT_OBJECT_0) {
            DWORD n = 0;
            if (PeekNamedPipe(read_end, buf, sizeof(buf), &n, nullptr, nullptr) &&
                n == 0) {
                break;  // EOF
            }
            DWORD got = 0;
            if (!ReadFile(read_end, buf, sizeof(buf), &got, nullptr) || got == 0) {
                break;
            }
            result.output.append(buf, got);
            continue;
        }
        if (wait == WAIT_TIMEOUT) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        break;  // WAIT_FAILED / WAIT_ABANDONED
    }
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, 2000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    result.ok = !result.timed_out && result.exit_code == 0;
    return result;
}
#else
Captured run_shell(const std::string& command, long long timeout_ms) {
    Captured result;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return result;
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return result;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        const int nul = open("/dev/null", O_RDONLY);
        if (nul != -1) {
            dup2(nul, STDIN_FILENO);
            close(nul);
        }
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipe_fds[1]);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            break;
        }
        pollfd pfd;
        pfd.fd = pipe_fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int rc = poll(&pfd, 1, static_cast<int>(remaining));
        if (rc > 0) {
            const ssize_t n = read(pipe_fds[0], buf, sizeof(buf));
            if (n > 0) {
                result.output.append(buf, static_cast<size_t>(n));
                continue;
            }
            break;  // EOF
        }
        if (rc < 0 && errno == EINTR) continue;
        result.timed_out = true;  // deadline
        kill(pid, SIGKILL);
        break;
    }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    result.ok = !result.timed_out && result.exit_code == 0;
    return result;
}
#endif

// ── host stats ──────────────────────────────────────────────────────────
#if defined(_WIN32)
HostStats host_stats() {
    HostStats stats;
    MEMORYSTATUSEX mex;
    mex.dwLength = sizeof(mex);
    if (GlobalMemoryStatusEx(&mex)) {
        stats.available = true;
        stats.mem_total_mb =
            static_cast<long long>(mex.ullTotalPhys) / (1024 * 1024);
        stats.mem_available_mb =
            static_cast<long long>(mex.ullAvailPhys) / (1024 * 1024);
        stats.swap_total_mb =
            static_cast<long long>(mex.ullTotalPageFile) / (1024 * 1024);
        stats.swap_free_mb =
            static_cast<long long>(mex.ullAvailPageFile) / (1024 * 1024);
    }
    stats.cores = static_cast<int>(std::thread::hardware_concurrency());
    return stats;
}
#else
HostStats host_stats() {
    HostStats stats;
    std::map<std::string, long long> meminfo;
    std::ifstream info("/proc/meminfo");
    std::string line;
    while (std::getline(info, line)) {
        const std::size_t colon = line.find(':');
        const std::size_t kb = line.rfind(" kB");
        if (colon == std::string::npos || kb == std::string::npos) continue;
        meminfo[trim(line.substr(0, colon))] =
            std::stoll(trim(line.substr(colon + 1, kb - colon - 1)));
    }
    if (meminfo.count("MemTotal")) {
        stats.available = true;
        stats.mem_total_mb = meminfo["MemTotal"] / 1024;
        stats.mem_available_mb =
            meminfo.count("MemAvailable") ? meminfo["MemAvailable"] / 1024
                                          : meminfo["MemFree"] / 1024;
        if (meminfo.count("SwapTotal")) {
            stats.swap_total_mb = meminfo["SwapTotal"] / 1024;
            stats.swap_free_mb =
                meminfo.count("SwapFree") ? meminfo["SwapFree"] / 1024 : 0;
        }
    }
    std::ifstream load("/proc/loadavg");
    load >> stats.load1 >> stats.load5 >> stats.load15;
    stats.cores = static_cast<int>(std::thread::hardware_concurrency());
    return stats;
}
#endif

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

// ── docker snapshot ─────────────────────────────────────────────────────
// One shell round-trip, three sections (--- separated):
//   1. docker ps -a: name|status ("Up 5 minutes (healthy)" / "Exited (137)")
//   2. docker stats --no-stream: name|MemUsage ("15.2GiB / 25GiB")
//   3. docker inspect: name|RestartCount (the OOM-kill counter)
// A missing docker CLI (dev machines) fails fast on section 1; the report
// flags docker_available=false and the view shows the rest.
const char kDockerSnapshot[] =
    "docker ps -a --format '{{.Names}}|{{.Status}}' 2>/dev/null; "
    "printf '\\n---\\n'; "
    "docker stats --no-stream --format '{{.Name}}|{{.MemUsage}}' 2>/dev/null; "
    "printf '\\n---\\n'; "
    "docker ps -aq | xargs -r docker inspect --format "
    "'{{.Name}}|{{.RestartCount}}' 2>/dev/null";

long long parse_mem_usage_mb(const std::string& usage) {
    // "15.2GiB / 25GiB" -> the first value in MB.
    const std::size_t slash = usage.find('/');
    std::string value = trim(slash == std::string::npos ? usage
                                                        : usage.substr(0, slash));
    const std::size_t digits_end = value.find_first_not_of("0123456789.");
    if (digits_end == 0) return -1;
    double number = 0.0;
    try {
        number = std::stod(value.substr(0, digits_end));
    } catch (...) {
        return -1;
    }
    const std::string unit = trim(value.substr(digits_end));
    if (unit == "B") return static_cast<long long>(number / (1024 * 1024));
    if (unit == "KiB") return static_cast<long long>(number / 1024);
    if (unit == "MiB") return static_cast<long long>(number);
    if (unit == "GiB") return static_cast<long long>(number * 1024);
    if (unit == "TiB") return static_cast<long long>(number * 1024 * 1024);
    return -1;
}

void parse_docker(const std::string& output, Report& report,
                  const std::vector<std::string>& keep_names) {
    std::vector<std::string> sections;
    std::string current;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line == "---") {
            sections.push_back(current);
            current.clear();
        } else {
            current += line;
            current += '\n';
        }
    }
    sections.push_back(current);
    if (sections.size() < 3) return;  // unexpected shape; leave flags as-is

    std::map<std::string, ContainerInfo> by_name;
    for (const std::string& raw : split_lines(sections[0])) {
        const std::string row = trim(raw);
        if (row.empty()) continue;
        const std::size_t sep = row.find('|');
        if (sep == std::string::npos) continue;
        ContainerInfo info;
        info.name = trim(row.substr(0, sep));
        const std::string status = trim(row.substr(sep + 1));
        // docker's status word -> stable state ("Up 5 min (healthy)").
        info.state = status.substr(0, status.find(' '));
        if (info.state.empty()) info.state = status;
        if (info.state == "Up") info.state = "running";
        else if (info.state == "Exited") info.state = "exited";
        else if (info.state == "Paused") info.state = "paused";
        else if (info.state == "Restarting") info.state = "restarting";
        if (status.find("(healthy)") != std::string::npos) {
            info.health = "healthy";
        } else if (status.find("(unhealthy)") != std::string::npos) {
            info.health = "unhealthy";
        }
        by_name[info.name] = std::move(info);
    }
    for (const std::string& raw : split_lines(sections[1])) {
        const std::string row = trim(raw);
        const std::size_t sep = row.find('|');
        if (sep == std::string::npos) continue;
        const std::string name = trim(row.substr(0, sep));
        auto it = by_name.find(name);
        if (it != by_name.end()) it->second.mem_mb = parse_mem_usage_mb(row.substr(sep + 1));
    }
    for (const std::string& raw : split_lines(sections[2])) {
        const std::string row = trim(raw);
        const std::size_t sep = row.find('|');
        if (sep == std::string::npos) continue;
        // inspect's .Name carries a leading '/'.
        std::string name = trim(row.substr(0, sep));
        if (!name.empty() && name.front() == '/') name.erase(0, 1);
        auto it = by_name.find(name);
        if (it != by_name.end()) {
            try {
                it->second.restarts = std::stoll(trim(row.substr(sep + 1)));
            } catch (...) {
            }
        }
    }
    report.docker_available = !by_name.empty();
    // Optional scoping (infra.containers): a name allowlist; empty = all.
    if (!keep_names.empty()) {
        std::set<std::string> allowed(keep_names.begin(), keep_names.end());
        for (auto it = by_name.begin(); it != by_name.end();) {
            it = (allowed.count(it->first) ? std::next(it) : by_name.erase(it));
        }
    }
    for (auto& [name, info] : by_name) report.containers.push_back(std::move(info));
}

// ── gpu (VRAM + utilization) ────────────────────────────────────────────
// AMD (rocm-smi) first - this box's llama containers are ROCm builds -
// then NVIDIA (nvidia-smi). One bounded shell round-trip; neither tool
// present (dev machines) leaves gpu.available false.
const char kGpuSnapshot[] =
    "if command -v rocm-smi >/dev/null 2>&1; then "
    "rocm-smi --showproductname --showuse --showmeminfo vram --csv "
    "2>/dev/null; "
    "elif command -v nvidia-smi >/dev/null 2>&1; then "
    "nvidia-smi --query-gpu=name,memory.total,memory.used,utilization.gpu "
    "--format=csv,noheader 2>/dev/null; "
    "fi";

std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (const char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (c == ',' && !in_quotes) {
            fields.push_back(field);
            field.clear();
            continue;
        }
        field += c;
    }
    fields.push_back(field);
    for (auto& f : fields) f = trim(f);
    return fields;
}

long long parse_mib_field(const std::string& field) {
    // nvidia-smi sizes: "32768 MiB".
    try {
        return std::stoll(trim(field.substr(0, field.find(' '))));
    } catch (...) {
        return 0;
    }
}

void parse_gpu(const std::string& output, GpuStats& gpu) {
    const std::vector<std::string> lines = split_lines(output);
    if (lines.empty() || trim(lines.front()).empty()) return;
    // rocm-smi: header line ("device,GPU use (%),VRAM ..."), data rows
    // card0,93,34208743424,31523266560,AMD Radeon AI PRO R9700,....
    // nvidia-smi (noheader): "Name", 32768 MiB, 29312 MiB, 56.
    const bool rocm = lines.front().find("VRAM") != std::string::npos;
    gpu.vendor = rocm ? "amd" : "nvidia";
    size_t start = rocm ? 1 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        const std::string row = trim(lines[i]);
        if (row.empty()) continue;
        const std::vector<std::string> f = csv_fields(row);
        if (rocm) {
            if (f.size() < 4) continue;
            GpuDevice device;
            // Card Series (f[4]) when present; the card index otherwise.
            device.name = f.size() > 4 && !f[4].empty() ? f[4] : f[0];
            try {
                device.util_pct = static_cast<int>(std::stoll(f[1]));
                device.vram_total_mb =
                    static_cast<long long>(std::stoll(f[2])) / (1024 * 1024);
                device.vram_used_mb =
                    static_cast<long long>(std::stoll(f[3])) / (1024 * 1024);
            } catch (...) {
                continue;
            }
            gpu.devices.push_back(std::move(device));
        } else {
            if (f.size() < 4) continue;
            GpuDevice device;
            device.name = f[0];
            device.vram_total_mb = parse_mib_field(f[1]);
            device.vram_used_mb = parse_mib_field(f[2]);
            try {
                device.util_pct = static_cast<int>(std::stoll(f[3]));
            } catch (...) {
                device.util_pct = 0;
            }
            gpu.devices.push_back(std::move(device));
        }
    }
    gpu.available = !gpu.devices.empty();
}

// ── provider leds ───────────────────────────────────────────────────────
void probe_endpoints(std::vector<ProviderLed>& leds) {
    for (auto& led : leds) {
        if (led.endpoint.empty()) continue;
        if (led.endpoint == "in-process") continue;  // the daemon is the endpoint
        std::string base = led.endpoint;
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string url = base;
        if (url.size() < 7 ||
            url.compare(url.size() - 7, 7, "/models") != 0) {
            url += "/models";
        }
        int status = 0;
        std::string body;
        std::string error;
        const auto started = std::chrono::steady_clock::now();
        const bool ok = http::get_full(url, {}, &status, body, 3000, 65536,
                                       &error);
        led.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
        led.online = ok && status >= 200 && status < 400;
    }
}

}  // namespace

Report collect(std::vector<ProviderLed> endpoints,
               std::vector<std::string> container_names) {
    Report report;
    report.collected_at_ms = now_ms();
    report.host = host_stats();

    const Captured docker = run_shell(kDockerSnapshot, 5000);
    if (docker.ok) parse_docker(docker.output, report, container_names);

    const Captured gpu = run_shell(kGpuSnapshot, 10000);
    if (gpu.ok) parse_gpu(gpu.output, report.gpu);
    // A non-zero exit with no section 1 rows means the CLI is absent (or the
    // daemon user lacks socket access); the flags already say "unavailable".

    probe_endpoints(endpoints);
    report.providers = std::move(endpoints);
    return report;
}

}  // namespace infra
}  // namespace nyaxus
