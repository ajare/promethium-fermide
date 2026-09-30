// Generate a complete, validated binary World using the public core API.
// Build/run instructions: docs/world-generator.md.
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/AgentTagRegistry.h"
#include "core/BinarySerializer.h"
#include "core/DoorSectorObject.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/Location.h"
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include "core/Transit.h"
#include "core/Vertex.h"
#include "core/World.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PF_SOURCE_DIR
#define PF_SOURCE_DIR "."
#endif

namespace
{
    namespace fs = std::filesystem;
    constexpr unsigned Base = 3, Floors = 32, Left = 1, Right = 14;
    using core::World;

    void require(bool condition, std::string const& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string readText(fs::path const& path)
    {
        std::ifstream file(path, std::ios::binary);
        require(bool(file), "Cannot read " + path.string());
        std::ostringstream text;
        text << file.rdbuf();
        require(!file.bad(), "Read failed: " + path.string());
        return text.str();
    }

    void writeText(fs::path const& path, std::string const& text)
    {
        std::ofstream file(path, std::ios::binary);
        require(bool(file), "Cannot write " + path.string());
        file << text;
        file.close();
        require(bool(file), "Write failed: " + path.string());
    }

    struct Options
    {
        fs::path output = fs::path(PF_SOURCE_DIR) / "resources/test-worlds/new-world.world";
        fs::path tags = fs::path(PF_SOURCE_DIR) / "resources/test-worlds/test.tags.yaml";
        fs::path behaviour = fs::path(PF_SOURCE_DIR)
            / "resources/test-worlds/new-world.behaviours/random-marker-wander.lua";
        uint64_t seed = 0;
        bool force = false;
    };

    struct Random
    {
        std::mt19937_64 engine;
        explicit Random(uint64_t seed) : engine(seed) {}
        unsigned integer(unsigned low, unsigned high)
        {
            // Defined rejection sampling, rather than implementation-specific distributions.
            uint64_t span = uint64_t(high) - low + 1;
            uint64_t threshold = (uint64_t(0) - span) % span;
            uint64_t value;
            do { value = engine(); } while (value < threshold);
            return low + unsigned(value % span);
        }
        template<class T> void shuffle(std::vector<T>& values)
        {
            for (size_t n = values.size(); n > 1; --n)
                std::swap(values[n - 1], values[integer(0, unsigned(n - 1))]);
        }
    };

    struct Point { unsigned x, y; };
    struct RoomDoor { unsigned room, corridor, x, y; };
    struct Layout
    {
        std::unique_ptr<World> world;
        std::vector<RoomDoor> doors;
        std::vector<Point> bulkheadPositions;
        unsigned rooms = 0, buttonDoors = 0, doorwayMarkers = 0;
        std::array<unsigned, 4> roomHeights{};
    };

    std::shared_ptr<const core::Sector> locationAt(World const& world, unsigned layer,
        unsigned x, unsigned y)
    {
        for (auto sector : world.getSectors(layer))
            if (std::dynamic_pointer_cast<const core::Location>(sector)
                && x >= sector->getCellX0() && x <= sector->getCellX1()
                && y >= sector->getCellY0() && y <= sector->getCellY1()) return sector;
        throw std::runtime_error("No Location at requested cell");
    }

    std::vector<std::shared_ptr<const core::Sector>> rooms(World const& world)
    {
        std::vector<std::shared_ptr<const core::Sector>> result;
        for (auto sector : world.getSectors(1))
            if (auto room = std::dynamic_pointer_cast<const core::Location>(sector))
                if (!room->isCorridor()) result.push_back(sector);
        return result;
    }

    void corridors(World& world, Random& random)
    {
        for (unsigned y = Base; y < Base + Floors; ++y)
        {
            // Alternate 2/3 Corridors, with small, independent width variations.
            std::vector<unsigned> widths;
            if ((y - Base) % 2 == 0)
            {
                unsigned first = random.integer(6, 8);
                widths = { first, 14 - first };
            }
            else
            {
                unsigned first = random.integer(4, 6), second, third;
                do { second = random.integer(3, 5); third = 14 - first - second; }
                while (third < 4 || third > 6);
                widths = { first, second, third };
            }
            unsigned x = Left;
            for (unsigned width : widths)
            {
                world.addCorridor(0, y, x, width, 1);
                x += width;
            }
        }
    }

    void transport(World& world, Random& random)
    {
        // Four overlapping height bands; exactly two shafts start at the building base.
        struct Shaft { unsigned x, bottom, height, width; };
        constexpr Shaft shafts[] = {
            {3, 0, 9, 2}, {11, 0, 10, 1}, {5, 7, 10, 1}, {13, 8, 10, 1},
            {2, 15, 10, 1}, {10, 16, 10, 1}, {4, 23, 9, 1}, {12, 24, 8, 1}
        };
        struct LiftPlan { Shaft shaft; std::vector<unsigned> stops; };
        std::vector<LiftPlan> plans;
        for (auto shaft : shafts)
        {
            auto rows = world.getLiftLandingRows(1, Base + shaft.bottom, shaft.x,
                shaft.width, shaft.height);
            std::vector<unsigned> eligible, stops;
            for (unsigned offset = 0; offset < shaft.height; ++offset)
            {
                auto floor = shaft.bottom + offset;
                // These two levels rely on the stationary staircases instead of Lift stops.
                if (floor == 14 || floor == 27) continue;
                if (rows.at(offset).usableForStop()) eligible.push_back(offset);
            }
            require(std::find(eligible.begin(), eligible.end(), 0u) != eligible.end()
                && std::find(eligible.begin(), eligible.end(), shaft.height - 1) != eligible.end(),
                "Lift needs valid first and last landings");
            stops = { 0, shaft.height - 1 };
            random.shuffle(eligible);
            unsigned desired = random.integer((shaft.height + 1) / 2,
                std::max((shaft.height + 1) / 2, unsigned(eligible.size() * 3 / 4)));
            for (auto offset : eligible)
                if (stops.size() < desired && std::find(stops.begin(), stops.end(), offset) == stops.end())
                    stops.push_back(offset);
            require(stops.size() >= desired, "Insufficient Lift landings");
            plans.push_back({shaft, stops});
        }
        // Avoid accidentally isolating a whole floor. Rooms can subsequently join Corridors.
        for (unsigned floor = 0; floor < Floors; ++floor)
        {
            if (floor == 14 || floor == 27) continue;
            bool covered = false;
            std::vector<unsigned> possible;
            for (unsigned i = 0; i < plans.size(); ++i)
            {
                auto const& plan = plans[i];
                if (floor < plan.shaft.bottom || floor >= plan.shaft.bottom + plan.shaft.height) continue;
                unsigned offset = floor - plan.shaft.bottom;
                covered |= std::find(plan.stops.begin(), plan.stops.end(), offset) != plan.stops.end();
                auto rows = world.getLiftLandingRows(1, Base + plan.shaft.bottom,
                    plan.shaft.x, plan.shaft.width, plan.shaft.height);
                if (rows.at(offset).usableForStop()) possible.push_back(i);
            }
            require(!possible.empty(), "No Lift can cover floor " + std::to_string(floor));
            if (!covered)
            {
                auto& plan = plans[possible[random.integer(0, unsigned(possible.size() - 1))]];
                plan.stops.push_back(floor - plan.shaft.bottom);
            }
        }
        for (auto& plan : plans)
        {
            std::sort(plan.stops.begin(), plan.stops.end());
            World::CreateLiftOptions options;
            options.cellsWide = plan.shaft.width;
            options.levelsHigh = plan.shaft.height;
            options.stopOffsets = plan.stops;
            world.addLift(1, Base + plan.shaft.bottom, plan.shaft.x, options);
        }

        struct Well { unsigned x, floor, height; };
        for (auto well : {Well{1, 10, 2}, Well{7, 18, 3}, Well{11, 20, 4}})
        {
            std::string diagnostic;
            require(world.canAddStairwell(1, Base + well.floor, well.x, well.height, &diagnostic), diagnostic);
            world.addStairwell(1, Base + well.floor, well.x, well.height,
                random.integer(0, 1) ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT);
        }

        // Spaced over the full height, not concentrated near the ground.
        struct Flight { unsigned x, floor; bool moving; };
        for (auto flight : {Flight{1, 2, true}, Flight{6, 5, false}, Flight{10, 13, false},
                            Flight{11, 18, true}, Flight{10, 26, false}, Flight{9, 30, true}})
        {
            std::string diagnostic;
            require(world.canAddStaircase(1, Base + flight.floor, flight.x, 2,
                CORE_SIDE_RIGHT, &diagnostic), diagnostic);
            world.addStaircase(1, Base + flight.floor, flight.x, 2, CORE_SIDE_RIGHT,
                flight.moving ? 1.0f : 0.0f);
        }
    }

    double clearance(World const& world, unsigned x, unsigned y, unsigned ignoreSector = ~0u)
    {
        double nearest = std::numeric_limits<double>::max();
        for (auto sector : world.getSectors(1))
        {
            if (sector->getIndex() == ignoreSector || !std::dynamic_pointer_cast<const core::Transit>(sector)) continue;
            double dx = std::max({0.0, double(sector->getCellX0()) - (x + 1),
                double(x) - (sector->getCellX0() + sector->getCellsWide())});
            double dy = std::max({0.0, double(sector->getCellY0()) - (y + 2),
                double(y) - (sector->getCellY0() + sector->getLevelsHigh())});
            nearest = std::min(nearest, dx * dx + dy * dy);
        }
        return nearest;
    }

    void ladders(World& world, Random& random)
    {
        struct Candidate { unsigned x, y; double distance; };
        std::vector<Candidate> candidates;
        for (unsigned y = Base; y + 1 < Base + Floors; ++y)
            for (unsigned x = Left; x <= Right; ++x)
                if (world.canAddLadder(1, y, x, 2))
                    candidates.push_back({x, y, clearance(world, x, y)});
        random.shuffle(candidates);
        std::vector<Candidate> chosen;
        double best = -1;
        for (auto a : candidates) for (auto b : candidates)
        {
            if (std::abs(int(a.y) - int(b.y)) < 8) continue;
            double score = 1000 * std::min(a.distance, b.distance) + a.distance + b.distance;
            if (std::min(a.distance, b.distance) >= 4 && score > best)
            {
                best = score;
                chosen = {a, b};
            }
        }
        require(chosen.size() == 2, "No separated Ladder positions");
        for (auto c : chosen)
            world.addLadder(1, c.y, c.x, World::CreateLadderOptions{2, false, true});
    }

    void fillRooms(Layout& layout, Random& random)
    {
        auto& world = *layout.world;
        bool occupied[64][16]{};
        for (auto sector : world.getSectors(1))
            for (unsigned y = sector->getCellY0(); y <= sector->getCellY1(); ++y)
                for (unsigned x = sector->getCellX0(); x <= sector->getCellX1(); ++x) occupied[y][x] = true;
        auto fits = [&](unsigned x, unsigned y, unsigned width, unsigned height)
        {
            if (x + width > Right + 1 || y + height > Base + Floors) return false;
            for (unsigned yy = y; yy < y + height; ++yy)
                for (unsigned xx = x; xx < x + width; ++xx)
                    if (occupied[yy][xx]) return false;
            return true;
        };
        auto corridorCount = [&](unsigned x, unsigned y, unsigned width)
        {
            std::set<unsigned> ids;
            for (unsigned xx = x; xx < x + width; ++xx) ids.insert(locationAt(world, 0, xx, y)->getIndex());
            return ids.size();
        };
        constexpr char const* names[] = {"Alder", "Birch", "Cedar", "Elm", "Hazel", "Juniper", "Maple", "Oak", "Willow"};
        constexpr char const* uses[] = {"Office", "Studio", "Lounge", "Workshop", "Archive", "Library", "Lab", "Meeting Room", "Store"};
        for (unsigned y = Base; y < Base + Floors; ++y) for (unsigned x = Left; x <= Right; ++x)
        {
            if (occupied[y][x]) continue;
            unsigned run = 0;
            while (x + run <= Right && !occupied[y][x + run]) ++run;
            if (run < 2) continue;
            std::vector<unsigned> widths;
            for (unsigned width = 2; width <= 5 && width <= run; ++width)
                if (run - width != 1 && corridorCount(x, y, width) <= 2) widths.push_back(width);
            // Avoid three-Corridor Rooms rather than violating the two-Door maximum.
            if (widths.empty()) widths.push_back(2);
            unsigned width = widths[random.integer(0, unsigned(widths.size() - 1))];
            unsigned roll = random.integer(0, 99), height = roll < 70 ? 1 : roll < 90 ? 2 : 3;
            while (height > 1 && !fits(x, y, width, height)) --height;
            require(fits(x, y, width, height), "Room packing overlap");
            std::string name = std::string(names[random.integer(0, 8)]) + " "
                + uses[random.integer(0, 8)] + " " + std::to_string(++layout.rooms);
            world.addRoom(name, 1, y, x, width, height);
            ++layout.roomHeights[height];
            for (unsigned yy = y; yy < y + height; ++yy)
                for (unsigned xx = x; xx < x + width; ++xx) occupied[yy][xx] = true;
        }
        require(layout.roomHeights[1] > layout.rooms / 2 && layout.roomHeights[2] && layout.roomHeights[3],
            "Room height mix was unsuitable");
    }

    void roomDoors(Layout& layout, Random& random)
    {
        auto& world = *layout.world;
        for (auto room : rooms(world))
        {
            unsigned count = 0;
            for (auto corridor : world.getSectors(0))
            {
                unsigned y = room->getCellY0();
                if (corridor->getCellY0() != y) continue;
                unsigned left = std::max(room->getCellX0(), corridor->getCellX0());
                unsigned right = std::min(room->getCellX1(), corridor->getCellX1());
                if (left > right) continue;
                std::vector<unsigned> cells;
                for (unsigned x = left; x <= right; ++x) cells.push_back(x);
                random.shuffle(cells);
                bool added = false;
                for (auto x : cells)
                {
                    if (!world.canAddCorridorDoor(0, y, x)) continue;
                    World::CreateDoorOptions options;
                    options.openStyle = core::Door::OpenStyle::OpenUp;
                    world.addSectorDoor(0, y, x, options);
                    layout.doors.push_back({room->getIndex(), corridor->getIndex(), x, y});
                    ++count;
                    added = true;
                    break;
                }
                require(added, "No valid Door for " + room->getName());
            }
            require(count >= 1 && count <= 2, "Room Door count outside 1-2");
        }
        auto candidates = layout.doors;
        random.shuffle(candidates);
        unsigned desired = unsigned(std::lround(layout.doors.size() * 0.25));
        for (auto door : candidates)
        {
            if (layout.buttonDoors == desired) break;
            auto corridor = world.getSector(door.corridor);
            for (unsigned i = 0; i < corridor->getNumObjects(); ++i)
            {
                auto object = std::dynamic_pointer_cast<const core::DoorSectorObject>(corridor->getObject(i));
                if (!object || object->getCellX() != door.x || object->getCellY() != door.y) continue;
                if (!world.canAddSectorDoorButton(corridor->getIndex(), i)) continue;
                // This operation validates both sides before mutation. A cramped Door is skipped.
                try { world.addSectorDoorButton(corridor->getIndex(), i); ++layout.buttonDoors; }
                catch (std::exception const&) { }
                break;
            }
        }
        require(layout.buttonDoors == desired, "Not enough space for Door Buttons");
    }

    bool hasExit(std::shared_ptr<const core::Sector> const& sector)
    {
        for (unsigned i = 0; i < sector->getNumObjects(); ++i)
            if (std::dynamic_pointer_cast<const core::DoorSectorObject>(sector->getObject(i))) return true;
        return false;
    }

    bool bulkheadClear(World const& world, unsigned x, unsigned y)
    {
        for (auto sector : world.getSectors(1)) if (std::dynamic_pointer_cast<const core::Transit>(sector))
        {
            if (y < sector->getCellY0() || y > sector->getCellY1()) continue;
            double gap = std::max({0.0, double(sector->getCellX0()) - x,
                double(x) - (sector->getCellX0() + sector->getCellsWide())});
            if (gap < 2) return false;
        }
        for (auto sector : world.getSectors(0)) for (unsigned i = 0; i < sector->getNumObjects(); ++i)
            if (auto door = std::dynamic_pointer_cast<const core::DoorSectorObject>(sector->getObject(i)))
            {
                if (door->getCellY() != y) continue;
                double gap = std::max({0.0, double(door->getCellX()) - x,
                    double(x) - (door->getCellX() + door->getDoor()->getCellsWide())});
                if (gap < 1) return false;
            }
        return true;
    }

    void bulkheads(Layout& layout, Random& random)
    {
        auto& world = *layout.world;
        std::vector<Point> candidates;
        World::CreateBulkheadDoorOptions options;
        for (auto left : world.getSectors(0)) for (auto right : world.getSectors(0))
        {
            if (left->getCellY0() != right->getCellY0() || left->getCellX1() + 1 != right->getCellX0()) continue;
            unsigned x = right->getCellX0(), y = right->getCellY0();
            if (hasExit(left) && hasExit(right) && bulkheadClear(world, x, y)
                && world.canAddSectorBulkheadDoor(0, y, x, CORE_SIDE_LEFT, options)) candidates.push_back({x, y});
        }
        random.shuffle(candidates);
        std::vector<Point> selected;
        for (auto point : candidates)
        {
            if (!selected.empty() && std::abs(int(point.y) - int(selected.front().y)) < 5) continue;
            try { world.addSectorBulkheadDoor(0, point.y, point.x, CORE_SIDE_LEFT, options); }
            catch (std::exception const&) { continue; }
            selected.push_back(point);
            if (selected.size() == 2) break;
        }
        require(selected.size() == 2, "No two safe Bulkhead Door locations");
        layout.bulkheadPositions = std::move(selected);
    }

    void markers(Layout& layout, Random& random)
    {
        auto& world = *layout.world;
        for (auto room : rooms(world))
        {
            std::vector<unsigned> cells;
            std::set<unsigned> doorCells;
            for (auto door : layout.doors) if (door.room == room->getIndex()) doorCells.insert(door.x);
            for (unsigned x = room->getCellX0(); x <= room->getCellX1(); ++x) cells.push_back(x);
            random.shuffle(cells);
            unsigned chosen = ~0u;
            double best = -1e9;
            for (auto x : cells)
            {
                float offset = float(x - room->getCellX0()) + 0.5f;
                if (!world.canAddSectorMarker(room->getIndex(), 0, offset)) continue;
                double distance = 100;
                for (auto door : doorCells) distance = std::min(distance, double(std::abs(int(x) - int(door))));
                double score = (doorCells.contains(x) ? -10000 : 0) + distance;
                if (score > best) { best = score; chosen = x; }
            }
            require(chosen != ~0u, "No Marker position");
            if (doorCells.contains(chosen)) ++layout.doorwayMarkers;
            world.addSectorMarker(room->getIndex(), 0, float(chosen - room->getCellX0()) + 0.5f, room->getName());
        }
    }

    void verifyReachability(World& world)
    {
        world.finishBuild();
        auto home = locationAt(world, 0, Left, Base);
        auto id = world.createAgent("Generator reachability probe", home->getIndex(), 0, 0.5f);
        auto agent = world.lookupAgent(id).entity;
        auto graph = world.getGraph();
        std::shared_ptr<const core::Vertex> source;
        for (auto vertex : graph->getVertices())
            if (vertex->getSector() && vertex->getSector()->getIndex() == home->getIndex()) { source = vertex; break; }
        std::set<unsigned> levels;
        if (source) for (auto vertex : graph->getVertices())
        {
            auto sector = vertex->getSector();
            if (!sector || sector->getLayerIndex() != 0 || levels.contains(sector->getCellY0())) continue;
            if (vertex == source || (graph->calculatePath(agent, source, vertex)
                && graph->calculatePath(agent, vertex, source))) levels.insert(sector->getCellY0());
        }
        world.removeAgent(id);
        require(levels.size() == Floors, "Not every level has a route from ground and back");
    }

    Layout generate(uint64_t seed, unsigned attempt)
    {
        Random random(seed + 0x9e3779b97f4a7c15ULL * attempt);
        Layout layout;
        layout.world = std::make_unique<World>("New World", 128, 64);
        auto& world = *layout.world;
        world.pauseSimulation();
        if (world.getRandomSeed() != seed)
            require(world.setRandomSeed(seed), "Could not set World random seed");
        corridors(world, random);
        transport(world, random);
        ladders(world, random);
        fillRooms(layout, random);
        roomDoors(layout, random);
        bulkheads(layout, random);
        markers(layout, random);
        verifyReachability(world);
        return layout;
    }

    void populate(Layout& layout, Random& random, std::shared_ptr<core::AgentTagRegistry> const& tags)
    {
        auto& world = *layout.world;
        core::AgentTagId male{}, female{};
        for (auto id : tags->getAgentTagIds())
        {
            if (tags->getAgentTagName(id) == "male") male = id;
            if (tags->getAgentTagName(id) == "female") female = id;
        }
        require(male.value && female.value, "Tag registry must contain male and female");
        world.attachAgentTagRegistry("test.tags.yaml", tags);
        for (unsigned y = Base; y < Base + Floors; ++y) for (unsigned i = 0; i < 10; ++i)
        {
            unsigned cell = Left + unsigned(std::lround(13.0 * i / 9.0));
            std::shared_ptr<const core::Sector> sector;
            if (random.integer(0, 1)) for (auto room : rooms(world))
                if (room->getCellY0() == y && cell >= room->getCellX0() && cell <= room->getCellX1()) sector = room;
            if (!sector) sector = locationAt(world, 0, cell, y);
            auto id = world.createAgent("Level " + std::to_string(y) + " - Agent " + std::to_string(i + 1),
                sector->getIndex(), 0, float(cell - sector->getCellX0()) + 0.5f);
            std::string diagnostic;
            require(world.assignAgentTag(id, random.integer(0, 1) ? male : female, &diagnostic), diagnostic);
        }
    }

    std::string manifest(World const& world)
    {
        auto registry = core::AgentBehaviourRegistry::create();
        std::ostringstream out;
        out << "version: 1\nuuid: " << registry->getUuid()
            << "\nrevision: 1\nnextBehaviourId: 2\nbehaviours:\n  - id: 1\n"
            << "    name: Random Marker Wander\n    revision: 1\n    source: random-marker-wander.lua\n"
            << "    schema:\n      - name: markers\n        type: list\n        required: false\n"
            << "        children:\n          - name: marker\n            type: marker\n        default:\n";
        for (auto id : world.getMarkerIds()) out << "          - type: marker\n            value: " << id.value << '\n';
        return out.str();
    }

    std::vector<core::AgentId> agentIds(World const& world)
    {
        std::vector<core::AgentId> result;
        for (unsigned layer = 0; layer < world.getLayerCount(); ++layer)
            for (auto sector : world.getSectors(layer)) for (auto agent : sector->getAgents()) result.push_back(world.getAgentId(agent));
        std::sort(result.begin(), result.end(), [](auto a, auto b) { return a.value < b.value; });
        return result;
    }

    void assign(World& world, std::string const& package, std::shared_ptr<core::AgentBehaviourRegistry> const& registry)
    {
        auto ids = registry->getBehaviourIds();
        require(ids.size() == 1, "Expected one behaviour");
        auto behaviour = registry->lookupAgentBehaviour(ids.front());
        require(behaviour->getModuleStatus() == core::AgentBehaviourModuleStatus::Loaded, behaviour->getModuleDiagnostic());
        world.attachAgentBehaviourRegistry(package, registry);
        for (auto id : agentIds(world))
        {
            std::string diagnostic;
            require(world.setAgentBehaviourAssignment(id, ids.front(), behaviour->getRevision(), {}, &diagnostic), diagnostic);
        }
    }

    void validate(World& world, Layout const& expected)
    {
        require(world.getCellsWide() == 128 && world.getLevelsHigh() == 64 && world.getLayerCount() == 2,
            "Wrong World dimensions");
        std::array<std::vector<float>, Floors> positions;
        unsigned lifts = 0, wide = 0, baseLifts = 0, wells = 0, laddersCount = 0, flights = 0, moving = 0, bulkheadHalves = 0;
        std::array<unsigned, 4> roomHeights{};
        std::set<std::string> names;
        std::set<unsigned> flightLevels, wellHeights;
        for (unsigned layer = 0; layer < 2; ++layer) for (auto sector : world.getSectors(layer))
        {
            require(sector->getCellX0() >= Left && sector->getCellX1() <= Right
                && sector->getCellY0() >= Base && sector->getCellY1() < Base + Floors, "Sector outside building");
            if (auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(sector))
            {
                ++lifts;
                wide += sector->getCellsWide() == 2;
                baseLifts += sector->getCellY0() == Base;
                require(sector->getLevelsHigh() >= 6 && lift->getNumStops() * 2 >= sector->getLevelsHigh(), "Lift stop coverage");
            }
            World::CreateStairwellOptions well{};
            if (world.getStairwellOptions(sector->getIndex(), well))
            { ++wells; wellHeights.insert(well.levelsHigh); require(well.levelsHigh >= 2 && well.levelsHigh <= 4, "Stairwell height"); }
            World::CreateLadderOptions ladder{};
            if (world.getLadderOptions(sector->getIndex(), ladder))
            {
                ++laddersCount;
                require(ladder.levelsHigh == 2, "Ladder height");
                require(clearance(world, sector->getCellX0(), sector->getCellY0(), sector->getIndex()) >= 4,
                    "Ladder too close to other transit");
            }
            World::CreateStaircaseOptions flight;
            if (world.getStaircaseOptions(sector->getIndex(), flight))
            { ++flights; moving += flight.speed != 0; flightLevels.insert(sector->getCellY0()); }
            for (unsigned i = 0; i < sector->getNumObjects(); ++i)
            {
                World::CreateBulkheadDoorOptions options;
                if (world.getSectorBulkheadDoorOptions(sector->getIndex(), i, options)) ++bulkheadHalves;
            }
            for (auto agent : sector->getAgents())
            {
                require(bool(std::dynamic_pointer_cast<const core::Location>(sector)), "Agent placed in a Transit");
                auto position = agent->getGlobalPosition();
                auto y = unsigned(std::lround(position.y));
                require(y == sector->getCellY0() && y >= Base && y < Base + Floors, "Agent without ground floor");
                positions[y - Base].push_back(position.x);
                auto id = world.getAgentId(agent);
                require(world.getAgentTags(id).size() == 1 && world.getAgentBehaviourAssignment(id).has_value(), "Missing tag/behaviour");
                auto name = world.getAgentTagRegistry()->getAgentTagName(*world.getAgentTags(id).begin());
                require(name == "male" || name == "female", "Wrong Agent tag");
            }
        }
        for (auto& row : positions)
        {
            require(row.size() == 10, "Need ten Agents per level");
            std::sort(row.begin(), row.end());
            for (unsigned i = 1; i < row.size(); ++i) require(row[i] - row[i - 1] >= 0.99f, "Agents too close");
        }
        require(lifts == 8 && wide == 1 && baseLifts == 2 && wells == 3 && laddersCount == 2
            && flights == 6 && moving == 3 && bulkheadHalves == 4, "Transport counts changed");
        require(wellHeights == std::set<unsigned>{2, 3, 4}, "Stairwell height mix changed");
        require(flightLevels.size() == 6 && *flightLevels.begin() <= Base + 3
            && *flightLevels.rbegin() >= Base + 29, "Staircases not spread over building height");
        unsigned previousFlight = *flightLevels.begin();
        for (auto floor : flightLevels)
        { require(floor - previousFlight <= 8, "Staircases too concentrated"); previousFlight = floor; }
        for (auto point : expected.bulkheadPositions)
            require(bulkheadClear(world, point.x, point.y)
                && hasExit(locationAt(world, 0, point.x - 1, point.y))
                && hasExit(locationAt(world, 0, point.x, point.y)), "Bulkhead clearance or exits changed");
        for (unsigned y = Base; y < Base + Floors; ++y)
        {
            std::set<unsigned> corridorsOnLevel;
            for (unsigned x = Left; x <= Right; ++x)
                corridorsOnLevel.insert(locationAt(world, 0, x, y)->getIndex());
            require(corridorsOnLevel.size() >= 2 && corridorsOnLevel.size() <= 3, "Corridor coverage changed");
        }
        unsigned doorCount = 0, buttons = 0;
        for (auto room : rooms(world))
        {
            require(room->getCellsWide() >= 2 && room->getCellsWide() <= 5 && room->getLevelsHigh() <= 3, "Room size");
            ++roomHeights[room->getLevelsHigh()];
            require(names.insert(room->getName()).second, "Duplicate Room name");
            unsigned markerCount = 0, count = 0;
            std::set<unsigned> connected, overlaps, doorCells;
            for (unsigned x = room->getCellX0(); x <= room->getCellX1(); ++x)
                overlaps.insert(locationAt(world, 0, x, room->getCellY0())->getIndex());
            for (auto door : expected.doors) if (door.room == room->getIndex())
            {
                World::CreateDoorOptions options;
                require(world.getSectorDoorOptions(0, door.y, door.x, 1, options)
                    && options.openStyle == core::Door::OpenStyle::OpenUp && door.y == room->getCellY0(), "Room Door changed");
                ++count; ++doorCount;
                connected.insert(door.corridor);
                doorCells.insert(door.x);
                if (options.controls[0] || options.controls[1])
                { require(options.controls[0] && options.controls[1], "One-sided Door Button"); ++buttons; }
            }
            require(count >= 1 && count <= 2 && connected == overlaps && connected.size() == count, "Room Door coverage");
            for (unsigned i = 0; i < room->getNumObjects(); ++i)
                if (auto object = std::dynamic_pointer_cast<const core::MarkerSectorObject>(room->getObject(i)))
                {
                    auto marker = object->getMarker(); ++markerCount;
                    require(marker->getCellY() == room->getCellY0() && marker->getName() == room->getName(), "Marker mismatch");
                    require(!doorCells.contains(marker->getCellX()) || doorCells.size() == room->getCellsWide(), "Avoidable Marker behind Door");
                }
            require(markerCount == 1, "Need one Marker per Room");
        }
        require(names.size() == expected.rooms && roomHeights == expected.roomHeights
            && doorCount == expected.doors.size() && buttons == expected.buttonDoors
            && buttons == unsigned(std::lround(doorCount * 0.25)), "Room counts changed");
        require(world.getMarkerIds().size() == expected.rooms && world.getAgentBehaviourAssignmentCount() == 320
            && world.getAgentTagAssignmentCount() == 320, "Assignment counts changed");
        verifyReachability(world);
    }

    // Each run owns a fresh staging directory. Existing outputs are backed up here
    // during publication; failed generation never touches the destination World.
    struct Staging
    {
        fs::path path;
        bool preserve = false;
        explicit Staging(fs::path const& parent)
        {
            auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            for (unsigned i = 0; i < 100; ++i)
            {
                path = parent / (".world-generator-" + std::to_string(nonce) + "-" + std::to_string(i));
                if (fs::create_directory(path)) return;
            }
            throw std::runtime_error("Cannot create staging directory");
        }
        ~Staging() { if (!preserve) { std::error_code ignored; fs::remove_all(path, ignored); } }
    };

    void publish(fs::path const& stagedWorld, fs::path const& stagedPackage, fs::path const& stagedTags,
        fs::path const& output, Staging& staging)
    {
        fs::path package = output.parent_path() / stagedPackage.filename();
        fs::path tags = output.parent_path() / "test.tags.yaml";
        fs::path oldWorld = staging.path / "previous.world", oldPackage = staging.path / "previous.behaviours";
        bool worldBacked = false, packageBacked = false, packageInstalled = false, tagsInstalled = false;
        try
        {
            if (fs::exists(output)) { fs::rename(output, oldWorld); worldBacked = true; }
            if (fs::exists(package)) { fs::rename(package, oldPackage); packageBacked = true; }
            if (!fs::exists(tags)) { fs::rename(stagedTags, tags); tagsInstalled = true; }
            fs::rename(stagedPackage, package); packageInstalled = true;
            fs::rename(stagedWorld, output); // Publish the referencing World last.
        }
        catch (...)
        {
            try
            {
                if (packageInstalled) fs::remove_all(package);
                if (tagsInstalled) fs::remove(tags);
                if (packageBacked) fs::rename(oldPackage, package);
                if (worldBacked) fs::rename(oldWorld, output);
            }
            catch (...)
            {
                staging.preserve = true;
                throw std::runtime_error("Publication rollback failed; recovery files retained at " + staging.path.string());
            }
            throw;
        }
    }

    Options parse(int argc, char** argv)
    {
        Options options;
        std::random_device entropy;
        options.seed = (uint64_t(entropy()) << 32) ^ entropy()
            ^ uint64_t(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--force") { options.force = true; continue; }
            if (arg == "--help")
            {
                std::cout << "pf-generate-world [--output FILE.world] [--seed UINT64] [--force]\n"
                    << "                  [--tags FILE.tags.yaml] [--behaviour FILE.lua]\n"
                    << "Default: resources/test-worlds/new-world.world. Existing outputs require --force.\n";
                std::exit(0);
            }
            require(i + 1 < argc, "Missing value for " + arg);
            std::string value = argv[++i];
            if (arg == "--output") options.output = value;
            else if (arg == "--tags") options.tags = value;
            else if (arg == "--behaviour") options.behaviour = value;
            else if (arg == "--seed")
            {
                require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos, "Invalid seed");
                size_t consumed = 0;
                options.seed = std::stoull(value, &consumed);
                require(consumed == value.size(), "Invalid seed");
            }
            else throw std::runtime_error("Unknown argument: " + arg);
        }
        options.output = fs::absolute(options.output).lexically_normal();
        require(options.output.extension() == ".world", "Output must end in .world (binary)");
        return options;
    }
}

int main(int argc, char** argv)
{
    try
    {
        auto options = parse(argc, argv);
        auto packageName = options.output.stem().string() + ".behaviours";
        auto packagePath = options.output.parent_path() / packageName;
        auto tagPath = options.output.parent_path() / "test.tags.yaml";
        for (auto const& path : {options.output, packagePath})
        {
            require(!fs::is_symlink(fs::symlink_status(path)), "Refusing symlink output: " + path.string());
            require(options.force || !fs::exists(path), "Output exists (use --force): " + path.string());
        }
        require(!fs::exists(options.output) || fs::is_regular_file(options.output), "World output is not a regular file");
        require(!fs::exists(packagePath) || fs::is_directory(packagePath), "Package output is not a directory");
        require(!fs::is_symlink(fs::symlink_status(tagPath)), "Refusing symlink tag destination");
        auto tagText = readText(options.tags), script = readText(options.behaviour);
        if (fs::exists(tagPath)) require(readText(tagPath) == tagText, "Destination test.tags.yaml differs; refusing to overwrite it");
        fs::create_directories(options.output.parent_path());
        Staging staging(options.output.parent_path());
        writeText(staging.path / "test.tags.yaml", tagText);
        auto tags = core::AgentTagRegistry::loadFrom((staging.path / "test.tags.yaml").string());
        std::cout << "Seed: " << options.seed << '\n';
        Layout layout;
        std::string lastFailure;
        unsigned attempt = 0;
        for (; attempt < 200; ++attempt)
        {
            try { layout = generate(options.seed, attempt); break; }
            catch (std::exception const& error)
            {
                lastFailure = error.what();
                if (attempt < 3 || attempt % 25 == 24)
                    std::cerr << "Layout " << attempt + 1 << " rejected: " << lastFailure << '\n';
            }
        }
        require(bool(layout.world), "No valid layout after 200 attempts: " + lastFailure);
        Random population(options.seed ^ 0xd1b54a32d192ed03ULL);
        populate(layout, population, tags);
        auto stagedPackage = staging.path / packageName;
        fs::create_directory(stagedPackage);
        writeText(stagedPackage / "random-marker-wander.lua", script);
        writeText(stagedPackage / "behaviours.yaml", manifest(*layout.world));
        auto registry = core::AgentBehaviourRegistry::loadFrom((stagedPackage / "behaviours.yaml").string());
        assign(*layout.world, packageName, registry);
        validate(*layout.world, layout);
        auto stagedWorld = staging.path / options.output.filename();
        layout.world->saveTo(stagedWorld.string());
        {
            World reopened("Readback", 128, 64);
            auto input = core::BinarySerializer::fromFile(stagedWorld.string());
            input->deserialize();
            core::SerializationWorkData data;
            require(reopened.deserialize(*input, data), "Binary readback failed");
            reopened.pauseSimulation();
            reopened.resolveAgentTagRegistry(tags);
            reopened.resolveAgentBehaviourRegistry(registry);
            validate(reopened, layout);
            require(reopened.resumeSimulation(), "Could not start runtime smoke test");
            for (unsigned tick = 0; tick < 240; ++tick)
            {
                reopened.update(World::getFixedTimestep());
                for (auto const& diagnostic : reopened.getAgentBehaviourRuntimeDiagnostics())
                    require(diagnostic.failure == core::AgentBehaviourRuntimeFailure::None, diagnostic.diagnostic);
            }
        }
        publish(stagedWorld, stagedPackage, staging.path / "test.tags.yaml", options.output, staging);
        std::cout << "Created " << options.output << " (layout attempt " << attempt + 1 << ")\n"
            << "128x64, 2 Layers; building x=1-14, levels 3-34; 320 tagged wandering Agents.\n"
            << "8 Lifts, 3 Stairwells, 6 Staircases (3 Escalators), 2 Ladders, 2 Bulkhead Doors.\n"
            << layout.rooms << " Rooms (" << layout.roomHeights[1] << "/" << layout.roomHeights[2]
            << "/" << layout.roomHeights[3] << " of heights 1/2/3), " << layout.doors.size()
            << " room Doors, " << layout.buttonDoors << " button-operated.\n"
            << layout.doorwayMarkers << " Markers had no doorway-free cell. Binary readback, round-trip routing, and Lua smoke test passed.\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "Generation failed: " << error.what() << '\n';
        return 1;
    }
}
