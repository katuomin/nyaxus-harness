#pragma once
// Infrastructure observability (docs/infra-observability.md): host memory,
// docker containers, and provider reachability in one bounded snapshot.
// The daemon is the only process that can see the host it runs on; the web
// view polls GET /api/v1/infra and renders whatever comes back.
#include <string>
#include <vector>

namespace nyaxus {
namespace infra {

struct HostStats {
    bool available = false;
    long long mem_total_mb = 0;
    long long mem_available_mb = 0;
    long long swap_total_mb = 0;
    long long swap_free_mb = 0;
    double load1 = 0;
    double load5 = 0;
    double load15 = 0;
    int cores = 0;
};

struct ContainerInfo {
    std::string name;
    std::string state;  // "running" | "exited" | "paused" | ...
    std::string health;  // "healthy" | "unhealthy" | "" (no healthcheck)
    long long restarts = 0;
    long long mem_mb = -1;  // -1 = not reported (stopped container)
};

struct ProviderLed {
    std::string name;
    std::string endpoint;
    bool online = false;
    long long latency_ms = 0;
};

struct GpuDevice {
    std::string name;
    long long vram_total_mb = 0;
    long long vram_used_mb = 0;
    int util_pct = 0;
};

struct GpuStats {
    bool available = false;
    std::string vendor;  // "amd" | "nvidia" | ""
    std::vector<GpuDevice> devices;
};

struct Report {
    HostStats host;
    GpuStats gpu;
    bool docker_available = false;
    std::vector<ContainerInfo> containers;
    std::vector<ProviderLed> providers;
    long long collected_at_ms = 0;
};

// Bounded collection: the docker snapshot is one shell round-trip with a
// hard deadline (a wedged dockerd must not wedge the daemon), and each
// endpoint probe is capped at 3 s. Returns a Report with whatever was
// collected; unavailable sources are flagged, not fatal. container_names
// scopes the container table (empty = all containers on the host).
Report collect(std::vector<ProviderLed> endpoints,
               std::vector<std::string> container_names = {});

}  // namespace infra
}  // namespace nyaxus
