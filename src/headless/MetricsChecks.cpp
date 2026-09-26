#include "core/World.h"
#include "core/AgentTagRegistryDocument.h"
#include "metrics/MetricsHttpServer.h"
#include "metrics/PrometheusTextFormatter.h"
#include <httplib.h>
#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <source_location>
#include <limits>

namespace {
void require(bool value, std::source_location location = std::source_location::current()) { if (!value) throw std::runtime_error("Metrics check failed at line " + std::to_string(location.line())); }
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
}
void runMetricsChecks() {
    core::MetricsRegistry exact;
    exact.add("pf_count", core::MetricType::Counter, {}, 2);
    require(metrics::PrometheusTextFormatter::format(exact) == "# HELP pf_count_total pf_count simulation metric\n# TYPE pf_count_total counter\npf_count_total 2\n");
    exact.add("pf_count_total", core::MetricType::Counter, {}, 1);
    bool rejected = false;
    try { (void)metrics::PrometheusTextFormatter::format(exact); } catch (std::invalid_argument const&) { rejected = true; }
    require(rejected);
    core::MetricsRegistry special;
    special.add("pf_nan", core::MetricType::Gauge, {}, std::numeric_limits<double>::quiet_NaN());
    special.add("pf_positive", core::MetricType::Gauge, {}, std::numeric_limits<double>::infinity());
    special.add("pf_negative", core::MetricType::Gauge, {}, -std::numeric_limits<double>::infinity());
    auto nonfinite = metrics::PrometheusTextFormatter::format(special);
    require(nonfinite.find("pf_nan NaN\n") != std::string::npos);
    require(nonfinite.find("pf_positive +Inf\n") != std::string::npos);
    require(nonfinite.find("pf_negative -Inf\n") != std::string::npos);
    core::MetricsRegistry registry;
    registry.add("pf_example_total", core::MetricType::Counter, {{"test", "a\"\\\nb"}}, 2);
    registry.observe("pf_wait_seconds", {}, 0.5);
    auto text = metrics::PrometheusTextFormatter::format(registry);
    require(text.find("test=\"a\\\"\\\\\\nb\"") != std::string::npos);
    require(text.find("pf_wait_seconds_bucket{le=\"+Inf\"} 1\n") != std::string::npos);
    require(text.find("pf_wait_seconds_sum 0.5\n") != std::string::npos);
    core::World world("Metrics checks", 8, 2);
    auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
    world.finishBuild();
    core::SimulationMetricsCollector collector(world);
    auto agent = world.createAgent("Metrics agent", room, 0, 0.5f);
    world.setSimulationObserver(&collector);
    metrics::MetricsHttpServer server(collector);
    require(server.start(0));
    httplib::Client client("127.0.0.1", server.port());
    auto result = client.Get("/metrics");
    require(result && result->status == 200 && result->get_header_value("Content-Type") == "text/plain; version=0.0.4");
    require(client.Get("/healthz")->status == 200);
    require(client.Get("/unknown")->status == 404);
    require(client.Post("/metrics", "", "text/plain")->status == 405);
    metrics::MetricsHttpServer conflict(collector);
    require(!conflict.start(server.port()));
    std::atomic<bool> valid{true};
    std::thread scrape([&] {
        double last = 0;
        for (int i = 0; i < 20; ++i) {
            auto response = client.Get("/metrics");
            if (!response || response->status != 200) { valid = false; continue; }
            std::string marker = "\npf_simulation_ticks_total ";
            auto pos = response->body.find(marker);
            if (pos != std::string::npos) { double value = std::stod(response->body.substr(pos + marker.size())); if (value < last) valid = false; last = value; }
        }
    });
    require(world.advanceTicks(100));
    scrape.join();
    require(valid);
    auto snapshot = collector.snapshot();
    require(snapshot.families.at("pf_simulation_ticks_total").samples.at({}).value == 100);
    require(snapshot.families.at("pf_agents_active").samples.at({}).value == 1);
    require(snapshot.families.at("pf_agent_events_total").samples.at({{"event", "added"}}).value == 1);
    require(world.consumeSimulationEvents().size() >= 600);
    world.setSimulationObserver(nullptr);
    require(world.advanceTicks(10));
    world.removeAgent(agent);
    world.setSimulationObserver(&collector);
    world.update(0.5f);
    require(collector.snapshot().families.at("pf_simulation_ticks_total").samples.at({}).value == 130);
    require(collector.snapshot().families.at("pf_agent_events_total").samples.at({{"event", "removed"}}).value == 1);
    world.setSimulationObserver(nullptr);
    core::SimulationMetricsCollector mapped(world);
    std::vector<core::SimulationEvent> events;
    for (auto reason : {core::TraversalFailureReason::NoReachableControl, core::TraversalFailureReason::ControlRejected, core::TraversalFailureReason::PreparationFailed, core::TraversalFailureReason::ResourceDisabled, core::TraversalFailureReason::LocalGoalUnreachable, core::TraversalFailureReason::PermitExpired}) {
        core::SimulationEvent event; event.type = core::SimulationEventType::TraversalRequestChanged;
        event.traversalRequest.state = core::TraversalRequestState::Denied; event.traversalRequest.failureReason = reason;
        events.push_back(event);
    }
    for (auto resultCode : {core::InteractionResult::Succeeded, core::InteractionResult::SucceededWithBestEffortFailure, core::InteractionResult::Failed, core::InteractionResult::Rejected, core::InteractionResult::Cancelled}) {
        core::SimulationEvent event; event.type = core::SimulationEventType::InteractionRequestChanged;
        event.interactionRequest.result = resultCode; events.push_back(event);
    }
    mapped.onTick(world.getSimulationTick(), events);
    require(mapped.snapshot().families.at("pf_traversal_denials_total").samples.size() == 6);
    require(mapped.snapshot().families.at("pf_agent_interactions_total").samples.size() == 5);

    core::World liftWorld("Per-lift metrics", 12, 4);
    while (liftWorld.getLayerCount() < 3) liftWorld.addLayer();
    liftWorld.addCorridor(1, 0, 0, 12, 1);
    liftWorld.addCorridor(1, 2, 0, 12, 1);
    core::World::CreateLiftOptions liftOptions;
    liftOptions.cellsWide = 1;
    liftOptions.stopOffsets = {0, 2};
    liftWorld.addLift(2, 0, 3, liftOptions);
    liftWorld.addLift(2, 0, 8, liftOptions);
    liftWorld.finishBuild();
    core::SimulationMetricsCollector liftCollector(liftWorld);
    auto const liftMetrics = liftCollector.snapshot();
    auto const& liftQueues = liftMetrics.families.at("pf_lift_admission_queue_length").samples;
    require(liftQueues.size() == 2);
    for (auto const& [labels, sample] : liftQueues) {
        require(labels.count("resource") == 1);
        require(labels.count("resource_id") == 1);
        require(labels.at("resource_type") == "lift");
        require(sample.value == 0);
    }

    for (int i = 0; i < 200; ++i) world.createTraversalResource("Resource " + std::to_string(i));
    mapped.refresh();
    auto const cardinalitySnapshot = mapped.snapshot();
    auto const& resourceQueues = cardinalitySnapshot.families.at("pf_traversal_queue_length").samples;
    require(resourceQueues.size() == 200);
    size_t series = 0;
    for (auto const& familyEntry : cardinalitySnapshot.families) for (auto const& sampleEntry : familyEntry.second.samples) {
        ++series;
        auto const& labels = sampleEntry.first;
        require(!labels.count("agent") && !labels.count("sector"));
    }
    require(series <= core::MetricsRegistry::maxSeries);
    server.stop();
}
int runMetricsEndpoint(int argc, char** argv) {
    int port = 9464; bool detail = false;
    std::string worldPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--metrics-port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--metrics-detail=sector,queue") detail = true;
        else if (arg == "--metrics-world" && i + 1 < argc) worldPath = argv[++i];
        else if (arg != "--metrics") throw std::invalid_argument("Unknown metrics option: " + arg);
    }
    auto ownedWorld = worldPath.empty() ? std::make_shared<core::World>("Metrics endpoint", 8, 2) : core::loadWorldDocument(worldPath);
    auto& world = *ownedWorld;
    if (worldPath.empty()) { world.addRoom("Room", 0, 0, 0, 8, 1); world.finishBuild(); }
    core::SimulationMetricsCollector collector(world, detail);
    world.setSimulationObserver(&collector);
    metrics::MetricsHttpServer server(collector);
    if (!server.start(port)) std::cerr << server.diagnostic() << '\n';
    else std::cout << "Metrics: http://127.0.0.1:" << server.port() << "/metrics (Ctrl-C to stop)\n" << std::flush;
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    while (!stopping) {
        if (!world.advanceTick()) collector.refresh();
        world.consumeSimulationEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    world.setSimulationObserver(nullptr);
    return 0;
}
