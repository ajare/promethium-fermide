#pragma once
#include "core/Simulation.h"
#include <map>
#include <mutex>

namespace core {
using MetricLabels = std::map<std::string, std::string>;
enum class MetricType { Counter, Gauge, Histogram };
struct MetricSample {
    MetricLabels labels;
    double value{};
    uint64_t count{};
    std::map<double, uint64_t> buckets;
};
struct MetricFamily {
    std::string help;
    MetricType type{};
    std::map<MetricLabels, MetricSample> samples;
};
// Value-only registry snapshots are safe to pass to another thread.
class MetricsRegistry {
public:
    std::map<std::string, MetricFamily> families;
    void add(std::string const& name, MetricType type, MetricLabels labels, double value);
    void observe(std::string const& name, MetricLabels labels, double seconds);
    void clearGauges();
    static constexpr size_t maxSeries = 4096;
};
class World;
class SimulationMetricsCollector final : public SimulationObserver {
public:
    explicit SimulationMetricsCollector(World const& world, bool detail = false);
    void onTick(uint64_t tick, std::vector<SimulationEvent> const& events) noexcept override;
    MetricsRegistry snapshot() const;
    void refresh() noexcept;
private:
    World const& world;
    bool detail;
    mutable std::mutex mutex;
    MetricsRegistry registry;
    std::map<TraversalResourceId, TraversalResourceSnapshot> previous;
    std::map<AgentId, AgentSnapshot> previousAgents;
    std::map<DeviceOperationId, DeviceOperationState> previousOperations;
    bool initialized{false};
    std::map<TraversalRequestId, uint64_t> granted;
    std::map<std::pair<TraversalResourceId, std::string>, uint64_t> started;
    std::map<std::pair<TraversalResourceId, AgentId>, uint64_t> boarded;
    void collect(SimulationSnapshot const& snapshot, std::vector<SimulationEvent> const& events);
};
}
