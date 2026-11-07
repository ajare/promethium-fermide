#pragma once
#include "core/SimulationMetricsCollector.h"
#include <memory>
namespace metrics {
class MetricsHttpServer {
public:
    explicit MetricsHttpServer(core::SimulationMetricsCollector const& collector);
    ~MetricsHttpServer();
    MetricsHttpServer(MetricsHttpServer const&) = delete;
    MetricsHttpServer& operator=(MetricsHttpServer const&) = delete;
    bool start(int port = 9464);
    void stop();
    int port() const;
    std::string const& diagnostic() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
