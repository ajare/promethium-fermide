#pragma once
#include "core/SimulationMetricsCollector.h"
namespace metrics {
class PrometheusTextFormatter {
public:
    static std::string format(core::MetricsRegistry const& registry);
};
}
