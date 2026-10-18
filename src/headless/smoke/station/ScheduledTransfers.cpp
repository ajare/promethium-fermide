#include "Smoke.h"
#include "ScheduledObservationFixture.h"
#include "core/World.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationConnections.h"
#include "core/StationMetrics.h"
#include "core/StationPorts.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
void require(bool value, std::string const& message) { if (!value) throw std::runtime_error(message); }
using Mode = core::ResourceTransferMode;
core::ResourceTransferSettings batch(std::string id, std::string source, std::string destination,
    double quantity, double when = 0, int priority = 0)
{
    core::ResourceTransferSettings t{id, source, destination, "kg", 0, priority};
    t.mode = Mode::Scheduled; t.quantity = quantity; t.firstOccurrenceSeconds = when; return t;
}
core::StationSystemsSettings fixture()
{
    core::StationSystemsSettings s; s.enabled = true;
    s.stores = {{"a", "kg", 100, 10}, {"b", "kg", 100}, {"c", "kg", 100}, {"independent", "kg", 100, 5}};
    s.transfers = {batch("move", "a", "b", 4)}; return s;
}
void configure(core::World& w, core::StationSystemsSettings const& s)
{
    w.pauseSimulation(); w.finishBuild(); std::string why;
    require(w.configureStationSystems(s, true, &why), why);
}
void run(core::World& w, unsigned ticks)
{
    w.resumeSimulation(); require(w.advanceTicks(ticks), "World refused ticks");
    auto snapshot = w.getStationSystemsSnapshot();
    require(snapshot.healthy, "Station tick failed");
}
auto stock(core::World const& w, std::string const& id)
{
    auto snap = w.getStationSystemsSnapshot();
    return *std::ranges::find(snap.stores, id, &core::StationSystemsStoreSnapshot::id);
}
void scheduledTransfers(smoke::Context const&)
{
    auto s = fixture(); double const dt = core::World::getFixedTimestep();
    core::World w("One off", 2, 2); configure(w, s);
    require(w.getStationSystemsSnapshot().transfers[0].occurrencesFired == 0, "Time-zero event ran during Apply");
    run(w, 1); auto fired = w.getStationSystemsSnapshot();
    require(stock(w, "a").currentQualified == 6 && stock(w, "b").currentQualified == 4
        && fired.transfers[0].occurrencesFired == 1, "Time-zero event did not execute exactly once");
    run(w, 60);
    require(stock(w, "b").currentQualified == 4 && w.getStationSystemsSnapshot().transfers[0].lastCompleted == 4,
        "Scheduled event repeated or lost its last outcome");
    for (auto const& balance : w.getStationSystemsSnapshot().balances)
        require(balance.lost == 0 && balance.delivered == 0 && std::abs(balance.residual) < 1e-9, "Internal transfer crossed external boundary");
    w.pauseSimulation(); w.resetSimulation(); run(w, 1);
    require(w.getStationSystemsSnapshot() == fired, "Reset did not replay time-zero transfer deterministically");
    auto forecast = fired.forecast;
    for (auto const& entry : forecast.stores)
        require((entry.storeId == "a" || entry.storeId == "b") == !entry.unavailableReason.empty(),
            "Scheduled forecast exclusion hid independent projections or missed an endpoint");

    // Same committed ticks, pause, accelerated update batching, boundary tolerance.
    s.transfers[0].firstOccurrenceSeconds = 3 * dt;
    core::World a("A", 2, 2), b("B", 2, 2); configure(a, s); configure(b, s);
    run(a, 2); require(stock(a, "b").currentQualified == 0, "Batch moved early");
    a.pauseSimulation(); auto paused = a.getStationSystemsSnapshot(); a.update(10);
    require(a.getStationSystemsSnapshot() == paused, "Pause advanced scheduled time");
    run(a, 58); b.resumeSimulation(); b.setSimulationTimeScale(2); b.update(0.5f);
    require(a.getStationSystemsSnapshot() == b.getStationSystemsSnapshot(), "Batching/time-scale changed schedule results");

    // Qualified, unreserved source above reserve; held material occupies capacity.
    s = fixture(); s.stores[0].protectedReserve = 8; s.stores[0].startingUnqualified = 20;
    s.stores[1].capacity = 5; s.stores[1].startingUnqualified = 4;
    core::World partial("Partial", 2, 2); configure(partial, s); run(partial, 1);
    auto t = partial.getStationSystemsSnapshot().transfers[0];
    require(t.lastCompleted == 1 && t.lastUnmet == 3 && !t.limitingReason.empty(), "Held headroom/partial outcome incorrect");
    run(partial, 30); require(stock(partial, "b").currentQualified == 1, "Unmet batch retried");
    s.stores[1].startingUnqualified = 5;
    core::World zero("Zero", 2, 2); configure(zero, s); run(zero, 2);
    require(zero.getStationSystemsSnapshot().transfers[0].occurrencesFired == 1
        && zero.getStationSystemsSnapshot().transfers[0].lastCompleted == 0, "Blocked event was not counted once");

    // Coincident event kind, priority, declaration order; no settling/fixpoint pass.
    s = fixture(); s.stores[0].startingQualified = 0;
    s.deliveries = {{"delivery", "a", "kg", 8, 0}};
    s.transfers = {batch("ab", "a", "b", 8), batch("bc", "b", "c", 8)};
    s.wasteEjections = {{"eject", "c", "kg", 8, 0}};
    core::World chain("Chain", 2, 2); configure(chain, s); run(chain, 1);
    require(chain.getStationSystemsSnapshot().wasteEjections[0].lastEjected == 8, "Delivery-transfer-ejection order incorrect");
    std::reverse(s.transfers.begin(), s.transfers.end());
    core::World reverse("Reverse", 2, 2); configure(reverse, s); run(reverse, 2);
    require(stock(reverse, "b").currentQualified == 8 && stock(reverse, "c").currentQualified == 0,
        "Reverse chain repeated settling");
    s.transfers = {batch("ab", "a", "b", 8, 0, 2)};
    core::World priority("Priority", 2, 2); configure(priority, s); run(priority, 1);
    require(stock(priority, "a").currentQualified == 8, "Event kind overrode higher priority");
    s = fixture(); s.transfers = {batch("first", "a", "b", 8), batch("second", "a", "c", 8)};
    core::World competition("Competition", 2, 2); configure(competition, s); run(competition, 1);
    require(stock(competition, "b").currentQualified == 8 && stock(competition, "c").currentQualified == 2,
        "Competing scheduled transfers ignored declaration order");

    // Scheduled edges retain design metadata but ignore physical caps/losses.
    s = fixture(); auto graph = core::station_connections::build(s);
    for (auto const& edge : graph.links)
    {
        require(core::station_connections::automaticConnectionType(graph, edge) == core::StationPhysicalConnection::NonPhysical,
            "Scheduled link acquired physical enforcement");
        s.connections[edge.designKey] = {core::StationPhysicalConnection::GasPipe, 0.0001, 0.0};
    }
    require(core::stationExchangeObservations(s).empty(), "Scheduled batch was labelled as a continuous exchange rate");
    core::World lossless("Lossless", 2, 2); configure(lossless, s); run(lossless, 1);
    require(lossless.getStationSystemsSnapshot().forecast.available, "Informational Scheduled design properties disabled independent forecasts");
    require(stock(lossless, "b").currentQualified == 4, "Physical caps or losses clipped a scheduled quantity");
    s.transfers[0].mode = Mode::Continuous; s.transfers[0].ratePerSecond = 60;
    core::World continuous("Continuous", 2, 2); configure(continuous, s); run(continuous, 1);
    require(stock(continuous, "b").currentQualified == 0, "Continuous physical enforcement regressed");

    // #691: a round trip restores BOTH connection caps and efficiencies,
    // without changing identity or executing the inactive schedule/rate.
    auto continuousSettings = fixture();
    continuousSettings.transfers[0].mode = Mode::Continuous;
    continuousSettings.transfers[0].ratePerSecond = 60;
    for (auto const& edge : core::station_connections::build(continuousSettings).links)
        continuousSettings.connections[edge.designKey] = {core::StationPhysicalConnection::LiquidPipe, 6, 0.5};
    auto switched = continuousSettings;
    switched.transfers[0].mode = Mode::Scheduled;
    require(switched.connections == continuousSettings.connections, "Mode switch changed connection identity");
    core::World roundTrip("Switch", 2, 2); configure(roundTrip, switched); run(roundTrip, 2);
    require(stock(roundTrip, "b").currentQualified == 4
        && roundTrip.getStationSystemsSnapshot().transfers[0].occurrencesFired == 1,
        "Switched Scheduled mode enforced physical properties or also ran the rate");
    switched.transfers[0].mode = Mode::Continuous;
    require(switched == continuousSettings, "Round trip lost inactive mode settings or properties");
    configure(roundTrip, switched); run(roundTrip, 2);
    require(std::abs(stock(roundTrip, "a").currentQualified - (10 - 12 * dt)) < 1e-9
        && std::abs(stock(roundTrip, "b").currentQualified - 3 * dt) < 1e-9
        && roundTrip.getStationSystemsSnapshot().transfers[0].occurrencesFired == 0,
        "Continuous mode did not restore each cap and efficiency, or also fired its schedule");

    // Inactive finite ranges are irrelevant, but all supplied numbers must
    // remain well formed, including during standalone draft loading.
    for (auto mode : {Mode::Continuous, Mode::Scheduled})
    {
        auto inactive = continuousSettings; inactive.transfers[0].mode = mode;
        if (mode == Mode::Continuous)
        {
            inactive.transfers[0].quantity = -3;
            inactive.transfers[0].firstOccurrenceSeconds = -2;
            inactive.transfers[0].recurrenceSeconds = -1;
        }
        else inactive.transfers[0].ratePerSecond = -60;
        configure(roundTrip, inactive);
        core::StationConfigurationDocument doc, parsed; doc.settings = inactive;
        std::string why;
        require(core::parseStationConfiguration(core::serializeStationConfiguration(doc), parsed, &why)
            && parsed.settings == inactive, "Inactive finite settings did not survive loading: " + why);
        auto invalidSwitch = inactive;
        invalidSwitch.transfers[0].mode = mode == Mode::Continuous ? Mode::Scheduled : Mode::Continuous;
        require(!roundTrip.configureStationSystems(invalidSwitch, true, &why)
            && roundTrip.getStationSystemsSettings() == inactive, "Switch bypassed active-mode range validation");
        for (auto field : {"ratePerSecond", "quantity", "firstOccurrenceSeconds", "recurrenceSeconds"})
        {
            auto malformed = core::serializeStationConfiguration(doc);
            auto at = malformed.find(std::string(field) + ": ");
            require(at != std::string::npos, "Inactive field missing from document");
            at += std::string(field).size() + 2;
            malformed.replace(at, malformed.find('\n', at) - at, "not-a-number");
            require(!core::parseStationConfiguration(malformed, parsed, &why) && parsed.settings == inactive,
                "Malformed inactive number was accepted or mutated the document");
        }
        for (int field = 0; field < 4; ++field)
        {
            auto bad = inactive; auto& t = bad.transfers[0];
            double* fields[] = {&t.ratePerSecond, &t.quantity, &t.firstOccurrenceSeconds, &t.recurrenceSeconds};
            *fields[field] = std::numeric_limits<double>::infinity();
            require(!roundTrip.configureStationSystems(bad, true, &why)
                && roundTrip.getStationSystemsSettings() == inactive, "Non-finite inactive field was accepted");
            doc.settings = bad;
            require(!core::parseStationConfiguration(core::serializeStationConfiguration(doc), parsed, &why)
                && parsed.settings == inactive, "Invalid inactive document replaced valid settings");
        }
    }

    // Rename remaps design keys; ordinary disconnection/deletion still erases
    // them, even while informational. Restoring the authored snapshot restores
    // exactly the original settings (the draft history uses this same state).
    switched = continuousSettings; switched.transfers[0].mode = Mode::Scheduled;
    core::StationNodeRename rename;
    require(core::planStationNodeRename(switched, "Transfer", "move", "renamed", rename)
        && core::renameStationNode(switched, rename), "Scheduled transfer rename refused");
    for (auto const& edge : core::station_connections::build(switched).links)
        require(edge.designKey.ownerId == "renamed" && switched.connections.contains(edge.designKey),
            "Scheduled rename lost design properties");
    auto saved = switched;
    require(core::clearStationLink(switched, "Transfer:renamed:Source")
        && switched.connections.size() == 1, "Scheduled disconnect did not remove exactly its properties");
    switched = saved;
    core::StationNodeRemoval removal;
    require(core::planStationNodeRemoval(switched, "Transfer", "renamed", removal)
        && core::removeStationNode(switched, removal) && switched.connections.empty(),
        "Scheduled deletion retained stale design properties");

    // Constituent accounting follows the material with no external term.
    s = fixture(); core::ResourceCompositionSettings mix;
    mix.moisture = 0.2; mix.nutrientNitrogen = 0.1; mix.phosphorus = 0.02;
    mix.potassium = 0.01; mix.otherMinerals = 0.03; mix.energyKJPerKgDry = 1000;
    s.stores[0].composition = mix; s.stores[1].composition = mix;
    core::World compound("Constituents", 2, 2); configure(compound, s); run(compound, 2);
    for (auto const& balance : compound.getStationSystemsSnapshot().constituentBalances)
        require(std::abs(balance.residual) < 1e-9, "Scheduled compound transfer violated constituent conservation");
    s.transfers.push_back({"consume", "b", "c", "kg", 60, 0});
    s.stores[2].composition = mix;
    core::World phases("Continuous sees post-event", 2, 2); configure(phases, s); run(phases, 1);
    require(std::abs(stock(phases, "c").currentQualified - 60 * dt) < 1e-9,
        "Continuous processing did not see the scheduled event inventory");

    // Every invalid Apply is a true no-op. Inactive finite fields do not execute.
    auto before = lossless.getStationSystemsSettings(); auto live = lossless.getStationSystemsSnapshot();
    lossless.pauseSimulation();
    for (int test = 0; test < 8; ++test)
    {
        auto bad = before; auto& transfer = bad.transfers[0];
        if (test == 0) transfer.quantity = 0;
        if (test == 1) transfer.quantity = std::numeric_limits<double>::quiet_NaN();
        if (test == 2) transfer.firstOccurrenceSeconds = -1;
        if (test == 3) transfer.firstOccurrenceSeconds = std::numeric_limits<double>::infinity();
        if (test == 4) transfer.recurrenceSeconds = dt * 0.5;
        if (test == 5) transfer.sourceId = transfer.destinationId;
        if (test == 6) bad.stores[1].hazardClass = "hazardous";
        if (test == 7) transfer.ratePerSecond = std::numeric_limits<double>::infinity();
        std::string why;
        require(!lossless.configureStationSystems(bad, true, &why) && !why.empty()
            && lossless.getStationSystemsSettings() == before && lossless.getStationSystemsSnapshot() == live,
            "Invalid Scheduled Apply mutated World");
    }

    // Existing maintenance reservations and future consumed-waste claims are senior.
    s = smoke::station::scheduledObservationFixture(); s.deliveries.clear(); s.wasteEjections.clear();
    s.maintenanceJobs[0].durationSeconds = 2; s.maintenanceJobs[0].recurrenceSeconds = 0;
    s.stores.push_back({"other", "kg", 100, 10});
    for (auto& store : s.stores) if (store.id == "spent") store.capacity = 1;
    s.transfers = {batch("parts", "parts", "other", 10, 2 * dt),
        batch("tools", "tools", "other", 10, 2 * dt), batch("waste", "other", "spent", 10, 2 * dt)};
    core::World maintenance("Reservations", 2, 2); configure(maintenance, s); run(maintenance, 3);
    auto reservations = maintenance.getStationSystemsSnapshot();
    require(reservations.transfers[0].lastCompleted == 1.5 && reservations.transfers[1].lastCompleted == 0
        && reservations.transfers[2].lastCompleted == 0, "Scheduled transfer stole maintenance stock or headroom");

    // A later ledger failure rolls back inventories AND occurrence accounting.
    s = fixture(); core::SolarArraySettings sun; sun.id = "sun"; sun.capacityKW = 1e308;
    s.solarArrays = {sun}; s.solarArrays.push_back(sun); s.solarArrays.back().id = "sun2";
    core::World failed("Atomic", 2, 2); configure(failed, s); failed.resumeSimulation(); failed.advanceTick();
    auto failure = failed.getStationSystemsSnapshot();
    require(!failure.healthy && failure.transfers[0].occurrencesFired == 0 && stock(failed, "a").currentQualified == 10,
        "Failed tick published a partial scheduled transfer");
}
void scheduledTransferDocuments(smoke::Context const&)
{
    auto s = fixture();
    s.transfers[0].ratePerSecond = 37; // retain the inactive Continuous setting
    s.transfers[0].recurrenceSeconds = core::World::getFixedTimestep();
    for (auto const& edge : core::station_connections::build(s).links)
        s.connections[edge.designKey] = {core::StationPhysicalConnection::GasPipe, 2.5, 0.4};
    // Preserve keys through rename as well as mode switching before saving.
    core::StationNodeRename rename;
    require(core::planStationNodeRename(s, "Transfer", "move", "saved move", rename)
        && core::renameStationNode(s, rename), "Persistence fixture rename failed");
    for (auto mode : {Mode::Continuous, Mode::Scheduled})
    {
      s.transfers[0].mode = mode;
      core::World w("Persistence", 2, 2); configure(w, s); run(w, 2);
      for (bool binary : {false, true})
      {
        core::SerializationWorkData data;
        auto roundTrip = [&]<class Serializer>() {
            auto writer = Serializer::toString(); w.serialize(*writer, data); writer->serialize();
            auto reader = Serializer::fromString(writer->getSerializedString()); reader->deserialize();
            auto loaded = std::make_unique<core::World>("Loaded", 2, 2); loaded->deserialize(*reader, data); return loaded;
        };
        auto owned = binary ? roundTrip.template operator()<core::BinarySerializer>() : roundTrip.template operator()<core::YamlSerializer>();
        auto& loaded = *owned;
        require(loaded.getStationSystemsSettings() == s && loaded.getStationSystemsSnapshot().transfers[0].occurrencesFired == 0,
            "World round trip lost authored fields or serialized runtime counters");
        run(loaded, 2); require(loaded.getStationSystemsSnapshot() == w.getStationSystemsSnapshot(), "Loaded schedule did not replay");
      }
      core::StationConfigurationDocument doc, parsed; doc.settings = s; std::string why;
      require(core::parseStationConfiguration(core::serializeStationConfiguration(doc), parsed, &why)
          && parsed.settings == s, "Mode round trip lost inactive settings or properties: " + why);
    }
    core::StationConfigurationDocument document, loaded; document.settings = s; std::string why;
    auto yaml = core::serializeStationConfiguration(document);
    require(core::parseStationConfiguration(yaml, loaded, &why) && loaded.settings == s, "Standalone schedule round trip failed: " + why);
    auto withoutRate = yaml;
    auto rateAt = withoutRate.find("ratePerSecond: 37");
    require(rateAt != std::string::npos, "Scheduled fixture rate field missing");
    auto lineStart = withoutRate.rfind('\n', rateAt) + 1;
    withoutRate.erase(lineStart, withoutRate.find('\n', rateAt) + 1 - lineStart);
    auto defaultRate = s; defaultRate.transfers[0].ratePerSecond = 0;
    require(core::parseStationConfiguration(withoutRate, loaded, &why) && loaded.settings == defaultRate,
        "Scheduled load required an inactive continuous rate");
    loaded.settings = s;
    auto stale = yaml; auto at = stale.find("schemaVersion: 99"); stale.replace(at, 17, "schemaVersion: 98");
    require(!core::parseStationConfiguration(stale, loaded, &why) && loaded.settings == s, "Old schema discarded Scheduled semantics");
    document.settings.transfers[0].sourceId.clear();
    require(core::parseStationConfiguration(core::serializeStationConfiguration(document), loaded, &why)
        && loaded.settings == document.settings, "Incomplete Scheduled draft was not inspectable");
    document.settings.transfers[0].recurrenceSeconds = core::World::getFixedTimestep() * 0.5;
    auto previous = loaded;
    require(!core::parseStationConfiguration(core::serializeStationConfiguration(document), loaded, &why)
        && loaded.settings == previous.settings, "Incomplete draft accepted sub-tick recurrence or mutated on refusal");
    document.settings = fixture(); document.settings.transfers[0].mode = Mode::Continuous;
    document.settings.transfers[0].ratePerSecond = 1;
    document.settings.transfers[0].quantity = 1;
    auto legacy = core::serializeStationConfiguration(document); at = legacy.find("schemaVersion: 99");
    legacy.replace(at, 17, "schemaVersion: 98");
    require(core::parseStationConfiguration(legacy, loaded, &why) && loaded.settings == document.settings,
        "Legacy transfer did not default to Continuous");
}
void recurringTransfers(smoke::Context const&)
{
    double const dt = core::World::getFixedTimestep();
    auto s = fixture();
    s.transfers[0].recurrenceSeconds = dt;
    core::World minimum("Minimum recurrence", 2, 2); configure(minimum, s);
    run(minimum, 1);
    auto snap = minimum.getStationSystemsSnapshot();
    require(snap.transfers[0].occurrencesFired == 2 && stock(minimum, "b").currentQualified == 8
        && snap.transfers[0].hasNextOccurrence && snap.transfers[0].nextOccurrenceSeconds == 2 * dt,
        "Time-zero and first boundary occurrences were not counted independently");
    run(minimum, 1);
    require(minimum.getStationSystemsSnapshot().transfers[0].lastCompleted == 2, "Partial occurrence incorrect");
    run(minimum, 1);
    require(minimum.getStationSystemsSnapshot().transfers[0].lastRequested == 4
        && minimum.getStationSystemsSnapshot().transfers[0].lastCompleted == 0, "Shortage was retried or accumulated");

    // A delivery replenishes a depleted source at a later coincident occurrence.
    s = fixture(); s.stores[0].startingQualified = 3;
    s.transfers = {batch("first", "a", "b", 4, 2 * dt), batch("second", "a", "c", 4, 2 * dt)};
    for (auto& t : s.transfers) t.recurrenceSeconds = 3 * dt;
    s.deliveries = {{"refill", "a", "kg", 10, 5 * dt}};
    core::World repeated("Repeated competition", 2, 2); configure(repeated, s);
    run(repeated, 2);
    require(stock(repeated, "b").currentQualified == 3 && stock(repeated, "c").currentQualified == 0,
        "First competition ignored declaration order");
    run(repeated, 3); snap = repeated.getStationSystemsSnapshot();
    require(stock(repeated, "b").currentQualified == 7 && stock(repeated, "c").currentQualified == 4,
        "Later requests accumulated unmet stock or delivery tie ordering changed");
    for (auto const& t : snap.transfers)
        require(t.lastRequested == 4 && t.lastCompleted == 4 && t.occurrencesFired == 2
            && t.hasNextOccurrence && t.nextOccurrenceSeconds == 8 * dt, "Recurring accounting incorrect");
    repeated.pauseSimulation(); repeated.resetSimulation(); run(repeated, 5);
    require(repeated.getStationSystemsSnapshot() == snap, "Recurring Reset replay changed outcomes");

    // Non-tick-aligned intervals use absolute occurrence arithmetic, never countdowns.
    s = fixture(); s.transfers[0].firstOccurrenceSeconds = 3 * dt + 1e-10;
    s.transfers[0].recurrenceSeconds = 2.5 * dt;
    core::World a("Ticks", 2, 2), b("Batched", 2, 2); configure(a, s); configure(b, s);
    run(a, 2); require(stock(a, "b").currentQualified == 0, "Recurring event fired early");
    run(a, 1); require(a.getStationSystemsSnapshot().transfers[0].occurrencesFired == 1, "Boundary tolerance missed occurrence");
    a.pauseSimulation(); auto paused = a.getStationSystemsSnapshot(); a.update(10);
    require(a.getStationSystemsSnapshot() == paused, "Pause advanced recurring time");
    run(a, 57); b.resumeSimulation(); b.setSimulationTimeScale(2); b.update(0.5f);
    require(a.getStationSystemsSnapshot() == b.getStationSystemsSnapshot(), "Recurring batching/time-scale mismatch");
    require(a.getStationSystemsSnapshot().transfers[0].occurrencesFired == 23, "Recurring interval drifted");

    // Both Apply and standalone parsing refuse invalid intervals atomically.
    a.pauseSimulation(); auto settings = a.getStationSystemsSettings(); auto live = a.getStationSystemsSnapshot();
    core::StationConfigurationDocument doc, parsed; doc.settings = settings; parsed = doc;
    for (double interval : {-1.0, dt * 0.5, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        auto bad = settings; bad.transfers[0].recurrenceSeconds = interval; std::string why;
        require(!a.configureStationSystems(bad, true, &why) && a.getStationSystemsSettings() == settings
            && a.getStationSystemsSnapshot() == live, "Invalid recurrence Apply was not atomic");
        doc.settings = bad;
        require(!core::parseStationConfiguration(core::serializeStationConfiguration(doc), parsed, &why)
            && parsed.settings == settings, "Invalid recurrence load was not atomic");
    }
}
void scheduledPantry(smoke::Context const&)
{
    auto s = core::bundledStationReference().settings;
    s.transfers.clear(); s.deliveries.clear(); s.cropPopulations.clear(); s.algaeCultures.clear();
    auto& crew = s.crews.front();
    auto pantry = std::ranges::find(s.stores, crew.foodStoreId, &core::ResourceStoreSettings::id);
    auto bulk = *pantry; bulk.id = "bulk-food"; bulk.startingQualified = 10; bulk.protectedReserve = 0;
    pantry->startingQualified = 0; pantry->protectedReserve = 0;
    auto const pantryId = pantry->id; s.stores.push_back(bulk);
    auto t = batch("replenish", bulk.id, pantryId, 0.0001, 3.0 * static_cast<double>(core::World::getFixedTimestep())); t.unit = bulk.unit;
    t.recurrenceSeconds = 400 * static_cast<double>(core::World::getFixedTimestep());
    s.transfers = {t};
    core::World w("Pantry", 2, 2); configure(w, s); run(w, 2);
    require(stock(w, pantryId).currentQualified == 0, "Pantry replenished before due time");
    run(w, 1);
    auto snap = w.getStationSystemsSnapshot();
    require(snap.transfers[0].lastCompleted == t.quantity && stock(w, pantryId).currentQualified < t.quantity,
        "Crew did not consume the scheduled pantry batch continuously: moved=" + std::to_string(snap.transfers[0].lastCompleted)
            + " pantry=" + std::to_string(stock(w, pantryId).currentQualified) + " scale=" + std::to_string(snap.crews.front().metabolicScale));
    run(w, 300); require(stock(w, pantryId).currentQualified < 1e-10 && stock(w, bulk.id).currentQualified == 10 - t.quantity,
        "Depleted pantry retried batch or created food");
    require(w.getStationSystemsSnapshot().crews.front().metabolicScale == 0, "Empty pantry did not remain ordinary Crew shortage");
    run(w, 100);
    snap = w.getStationSystemsSnapshot();
    require(snap.transfers[0].occurrencesFired == 2 && snap.transfers[0].lastRequested == t.quantity
        && snap.crews.front().metabolicScale > 0 && stock(w, pantryId).currentQualified > 0,
        "Later recurrence did not recover the depleted pantry and Crew");
}
}
void registerScheduledTransferChecks(std::vector<smoke::Check>& checks)
{
    checks.push_back({"scheduledTransfers", scheduledTransfers});
    checks.push_back({"recurringTransfers", recurringTransfers});
    checks.push_back({"scheduledTransferDocuments", scheduledTransferDocuments});
    checks.push_back({"scheduledPantry", scheduledPantry});
}
