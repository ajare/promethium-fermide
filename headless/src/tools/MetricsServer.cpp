#include "ToolSupport.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "metrics/MetricsHttpServer.h"
#include <chrono>
#include <csignal>
#include <thread>

namespace
{
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
}

int main(int argc, char** argv)
{
    constexpr auto usage = "Usage: pf-metrics-server [--port 0..65535] [--world <world>] "
        "[--detail=sector,queue] [--ticks 1..1000000]";
    return tool::main("pf-metrics-server", usage, [&]
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << usage << '\n';
            return 0;
        }
        unsigned port = 9464, ticks = 0;
        bool detail = false, portSeen = false, worldSeen = false, ticksSeen = false;
        std::string worldPath;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg(argv[i]);
            auto value = [&](bool& seen) -> std::string_view
            {
                if (seen) throw tool::UsageError("Duplicate option: " + std::string(arg));
                seen = true;
                if (++i == argc) throw tool::UsageError("Missing value for " + std::string(arg));
                return argv[i];
            };
            if (arg == "--port") port = tool::number(value(portSeen), 0, 65535, "Port");
            else if (arg == "--ticks") ticks = tool::number(value(ticksSeen), 1, 1000000, "Ticks");
            else if (arg == "--world")
            {
                worldPath = value(worldSeen);
                tool::pathArgument(worldPath);
            }
            else if (arg == "--detail=sector,queue" && !detail) detail = true;
            else throw tool::UsageError("Unknown or duplicate option: " + std::string(arg));
        }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        auto ownedWorld = worldPath.empty() ? std::make_shared<core::World>("Metrics endpoint", 8, 2)
            : core::loadWorldDocument(worldPath);
        auto& world = *ownedWorld;
        if (worldPath.empty())
        {
            world.addRoom("Room", 0, 0, 0, 8, 1);
            world.finishBuild();
        }
        core::SimulationMetricsCollector collector(world, detail);
        world.setSimulationObserver(&collector);
        struct Detach
        {
            core::World& world;
            ~Detach() { world.setSimulationObserver(nullptr); }
        } detach{world};
        metrics::MetricsHttpServer server(collector);
        if (!server.start(static_cast<int>(port))) throw std::runtime_error(server.diagnostic());
        std::cout << "Metrics: http://127.0.0.1:" << server.port()
            << "/metrics (Ctrl-C to stop)\n" << std::flush;
        for (unsigned tick = 0; !stopping && (ticks == 0 || tick < ticks); ++tick)
        {
            if (!world.advanceTick()) collector.refresh();
            world.consumeSimulationEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return 0;
    });
}
