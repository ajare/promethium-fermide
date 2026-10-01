#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "metrics/MetricsHttpServer.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
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
