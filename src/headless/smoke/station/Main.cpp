#include "Smoke.h"
#include "Documents.h"
#include "Connections.h"
#include "BatteryObservations.h"
#include "ObservationIntegration.h"
#include "StationThermalChecks.h"
#include "StationGenerationChecks.h"
#include "StationAtmosphereChecks.h"
#include "StationWaterChecks.h"
#include "StationRecoveryChecks.h"
#include "StationBiologyChecks.h"
#include "StationRig.h"
#include "StationCoreChecks.h"
#include "StationPortChecks.h"
#include "StationSeamChecks.h"
#include "StationPowerChecks.h"
#include "StationCompoundPowerChecks.h"
#include "StationBatteryChecks.h"
#include "StationLogisticsChecks.h"
#include "StationOperationsChecks.h"
#include "StationResilienceChecks.h"
void registerScheduledTransferChecks(std::vector<smoke::Check>& checks);
#include "MaterialObservations.h"
#include "Coupled.h"
#include "Reference.h"
#include "Scenarios.h"
#include "Stability.h"
#include "core/World.h"
#include "core/StationPorts.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationSystems.h"
#include "core/StationConnections.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace
{
	using namespace smoke::station;


}

	// #418: bounded trends and incident history. Trend series are sampled
	// from the committed ledger at the existing observation cadence - no
	// second clock, no per-tick history - with a fixed per-series sample
	// budget (pairwise-merge downsampling on overflow) and an authored
	// retention cap in simulation seconds. Events carry a stable
	// presentation category so later features' incident types render
	// through the same contract. Reading trends never mutates state.
	void trends(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-9)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const constantProfile = [](double level)
		{ core::ProfileSettings p; p.constantLevel = level; return p; };
		auto const replaceAll = [](std::string text, std::string const& from, std::string const& to)
		{
			for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
				text.replace(pos, from.size(), to);
			return text;
		};
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findSeries = [](std::vector<core::StationTrendSeries> const& trends,
			std::string const& subsystem, std::string const& id) -> core::StationTrendSeries const*
		{
			for (auto const& series : trends)
				if (series.subsystem == subsystem && series.id == id) return &series;
			return nullptr;
		};
		// Steady-state rig: a 10 kW sun, a 6 kW constant load fully
		// supplied, and a 1 kg/s transfer with room at both ends. Every
		// sampled value is a known constant, so merge or retention
		// arithmetic errors show up as value drift, not just count drift.
		auto const rigSettings = [&]()
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			core::ResourceStoreSettings source;
			source.id = "source"; source.unit = "kg";
			source.capacity = 10000.0; source.startingQualified = 8000.0;
			core::ResourceStoreSettings sink;
			sink.id = "sink"; sink.unit = "kg";
			sink.capacity = 10000.0; sink.startingQualified = 0.0;
			settings.stores = { source, sink };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "source"; pump.destinationId = "sink";
			pump.unit = "kg"; pump.ratePerSecond = 1.0; pump.priority = 5;
			settings.transfers = { pump };
			core::SolarArraySettings sun;
			sun.id = "sun"; sun.capacityKW = 10.0; sun.illumination = constantProfile(1.0);
			settings.solarArrays = { sun };
			core::EquipmentLoadSettings load;
			load.id = "shop"; load.electricalDemandKW = 6.0;
			load.operatingProfile = constantProfile(1.0);
			load.heatOutputKW = 0.0; load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			load.criticalFloorKW = 0.0; load.priority = 10; load.maxCurtailment = 1.0;
			settings.equipmentLoads = { load };
			return settings;
		};
		std::string diagnostic;
		// Extreme limits and invalid spans exercise the public shared recorder.
		{
			core::StationRecordingSlot a, b;
			a.metric.id = "a"; b.metric.id = "b";
			core::StationRecordingLimits limits; limits.samples = 4;
			core::StationHistory history({a, b}, 1, 100, limits);
			std::vector<core::StationMetricSample> values(2);
			for (auto& v : values) { v.actual = 0; v.validity = core::StationMetricValidity::Valid; }
			for (int t = 1; t <= 8; ++t)
			{
				values[1].validity = t == 2 ? core::StationMetricValidity::Unavailable : core::StationMetricValidity::Valid;
				history.observe(t, values);
			}
			auto const recorded = history.metrics();
			require(history.status().coarseningLevel == 1 && history.status().truncated,
				"shared history did not coarsen deterministically");
			require(recorded[0].samples.size() == recorded[1].samples.size()
				&& recorded[0].samples[0].actual == 0 && !recorded[1].samples[0].actual
				&& recorded[1].samples[0].coverage == 0.5,
				"coarsening confused a known zero with an invalid interval");
			for (size_t i = 0; i < recorded[0].samples.size(); ++i)
				require(recorded[0].samples[i].elapsedSeconds == recorded[1].samples[i].elapsedSeconds,
					"shared history lost timeline alignment");
			limits.memoryBytes = 1;
			core::StationHistory limited({a, b}, 1, 100, limits);
			require(limited.status().limited && limited.metrics().empty(), "memory limit silently omitted a metric");
			limits = {}; limits.metrics = 1;
			require(core::StationHistory({a, b}, 1, 100, limits).status().limited, "cardinality limit ignored");
			limits = {}; limits.metadataBytes = 1;
			require(core::StationHistory({a, b}, 1, 100, limits).status().limited, "metadata limit ignored");
			limits = {}; limits.samples = 4;
			core::StationHistory retained({a, b}, 1, 2, limits);
			for (int t = 1; t <= 20; ++t) retained.observe(t, values);
			require(retained.status().retainedUntilSeconds - retained.status().retainedFromSeconds <= 2
				&& retained.status().recordingBytes <= limits.memoryBytes, "retention or recording bound exceeded");
		}
		// #697: public revision/query seam is read-only and independent of
		// population size. Counters measure work, not machine-dependent timing.
		{
			core::World world("Cached applied run", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			for (size_t i = 1; i < core::MaxStationEquipmentLoads; ++i)
			{
				auto load = settings.equipmentLoads.front(); load.id = "other-" + std::to_string(i);
				settings.equipmentLoads.push_back(load);
			}
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.markSaved();
			auto revision = world.getStationSystemsObservationRevision();
			auto const* catalogue = &world.getStationSystemsObservationCatalogue();
			world.resumeSimulation(); runOrDiagnose(world, 60, "cache run"); world.pauseSimulation();
			auto const recordedRevision = world.getStationSystemsObservationRevision();
			require(recordedRevision.run == revision.run && recordedRevision.history > revision.history,
				"observation did not advance only the history revision");
			auto const beforeQueryDocument = yaml(world);
			auto const beforeQuerySnapshot = world.getStationSystemsSnapshot();
			auto const expected = world.queryStationSystemsChart("power");
			auto const work = world.getStationSystemsQueryWork();
			auto const* buffer = world.queryStationSystemsChart("power").series[0].values.data();
			for (int i = 0; i < 100; ++i)
			{
				require(&world.getStationSystemsObservationCatalogue() == catalogue
					&& world.queryStationSystemsChart("power").series[0].values.data() == buffer,
					"unchanged query replaced catalogue or prepared buffer");
			}
			require(work.queries == 1 && work.metricsExported == 2
				&& world.getStationSystemsQueryWork().queries == work.queries
				&& world.getStationSystemsQueryWork().metricsExported == work.metricsExported
				&& world.getStationSystemsObservationRevision() == recordedRevision && !world.isModified()
				&& yaml(world) == beforeQueryDocument && world.getStationSystemsSnapshot() == beforeQuerySnapshot,
				"cached query rebuilt unrelated metrics or mutated the paused document");
			auto changed = settings; changed.equipmentLoads.clear();
			require(!world.configureStationSystems(changed, false)
				&& world.getStationSystemsObservationRevision() == recordedRevision,
				"unconfirmed application replaced the run");
			auto invalid = changed; invalid.observationIntervalSeconds = -1;
			require(!world.configureStationSystems(invalid, true)
				&& world.getStationSystemsObservationRevision() == recordedRevision,
				"invalid application replaced the run");
			require(world.configureStationSystems(settings, false)
				&& world.getStationSystemsObservationRevision() == recordedRevision,
				"unchanged application reset history");
			(void)world.queryStationSystemsChart("unavailable");
			auto const reselected = world.queryStationSystemsChart("power");
			require(reselected.series[0].values == expected.series[0].values
				&& reselected.series[0].times == expected.series[0].times
				&& world.getSimulationTick() == 60 && world.getStationSystemsObservationRevision() == recordedRevision,
				"property reselection did not reuse retained history without replay");
			world.resetSimulation();
			require(world.getStationSystemsObservationRevision().run != revision.run
				&& world.queryStationSystemsChart("power").series[0].values.empty(),
				"Reset reused an old-run query");
			world.resumeSimulation(); runOrDiagnose(world, 60, "cached replay"); world.pauseSimulation();
			auto const replay = world.queryStationSystemsChart("power");
			require(replay.series[0].values == expected.series[0].values
				&& replay.series[1].values == expected.series[1].values
				&& replay.series[0].times == expected.series[0].times, "cached replay differs");
			revision = world.getStationSystemsObservationRevision();
			require(world.configureStationSystems(changed, true)
				&& world.getStationSystemsObservationRevision().run != revision.run
				&& world.queryStationSystemsChart("power").series[0].values.empty(),
				"successful application retained old query history");
			// Replace the sole cache entry repeatedly, including an unknown
			// property; this cannot accumulate a per-selection history map.
			auto const* cacheEntry = &world.queryStationSystemsChart("power");
			for (int i = 0; i < 50; ++i)
			{
				auto const& missing = world.queryStationSystemsChart("unavailable-" + std::to_string(i));
				require(missing.series.empty() && &missing == cacheEntry, "unknown property fabricated data or grew the cache");
				auto const& power = world.queryStationSystemsChart("power");
				require(power.series.size() == 2 && &power == cacheEntry, "Power selection did not reuse the bounded cache entry");
			}
			require(world.getStationSystemsQueryWork().metricsExported == 102,
				"selection built unrelated metrics");
		}
		// Scenario A: sampling cadence. With a 60 s observation interval
		// and a 60 s run, each series holds exactly one sample at the
		// observation boundary - never one per tick - carrying units,
		// simulation timestamps, requested versus actual and identity.
		{
			core::World world("TrendCadence", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 60.0f;
			settings.trendRetentionSeconds = 3600.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 3600, "trend cadence"); // t = 60 s: one boundary
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4,
				"expected exactly the power, power-generation, equipment-load and transfer trend series");
			for (auto const& series : trends)
			{
				require(series.samples.size() == 1,
					"trend series sampled more than once within one observation window");
				require(series.samples[0].worldTick == 3600,
					"trend sample is not at the observation-boundary tick");
				require(series.downsampleLevel == 0 && !series.truncated,
					"a fresh series reports downsampling or truncation");
				require(approx(series.sampleIntervalSeconds, 60.0, 1e-4),
					"series stride does not match the authored observation cadence");
			}
			auto const* power = findSeries(trends, "power", "");
			require(power != nullptr && power->unit == "kW",
				"station-wide power trend series missing or mislabelled");
			require(approx(power->samples[0].requested, 6.0)
				&& approx(power->samples[0].actual, 6.0),
				"the power trend did not record requested versus actual supply");
			auto const* generation = findSeries(trends, "power-generation", "");
			require(generation && generation->unit == "kW"
				&& approx(generation->samples[0].actual, 10.0)
				&& approx(generation->samples[0].requested, 10.0),
				"generation history must retain 10 kW generation, not 6 kW supplied service");
			auto const metrics = world.getStationSystemsMetricHistory();
			auto const chart = core::queryStationChart(core::stationCharts().front(), metrics);
			require(chart.series.size() == 2 && !chart.incomplete
				&& chart.series[0].times.size() == 1 && approx(chart.series[0].times[0], 60.0, 1e-5)
				&& chart.series[0].values.size() == 1 && approx(chart.series[0].values[0], 6.0)
				&& chart.series[1].values.size() == 1 && approx(chart.series[1].values[0], 10.0),
				"declarative Power query lost surplus generation, demand or simulation time");
			require(metrics.size() == 4 + 2 * settings.stores.size() + core::stationExchangeObservations(settings).size()
				&& metrics[0].descriptor.requestedSemantics
				&& !metrics[1].descriptor.requestedSemantics && !metrics[1].samples[0].requested,
				"catalogue manufactured requested generation semantics");
			auto const* transfer = findSeries(trends, "transfer", "pump");
			require(transfer != nullptr && transfer->unit == "kg",
				"transfer trend identity or unit missing");
			// The snapshot's transfer requested/completed are per-tick
			// amounts (rate x fixed step); the trend mirrors that exactly.
			auto const perTick = 1.0 * static_cast<double>(core::World::getFixedTimestep());
			require(approx(transfer->samples[0].requested, perTick)
				&& approx(transfer->samples[0].actual, perTick),
				"the transfer trend did not record requested versus completed flow");
			auto const* load = findSeries(trends, "equipment-load", "shop");
			require(load != nullptr && load->unit == "kW" && load->id == "shop",
				"equipment load trend identity or unit missing");
		}
		// Scenario B: a long run under the fixed sample budget. 1300
		// observations per series must coarsen (pairwise merge, stride
		// x2 per level, cadence-aware decimation) instead of growing
		// storage, keep the newest retained sample within one stride of
		// the run end, keep gaps at the retained stride, and - because
		// the rig is steady - keep every merged value exactly constant.
		{
			core::World world("TrendLongRun", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.trendRetentionSeconds = 86400.0f; // retention never binds here
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 78000, "trend long run"); // t = 1300 s, 1300 raw samples
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4, "long run lost a trend series");
			for (auto const& series : trends)
			{
				require(series.samples.size() <= 256,
					"trend series exceeded the 256-sample storage bound");
				require(series.downsampleLevel >= 1,
					"a 1300-sample run did not downsample under the storage bound");
				require(series.truncated,
					"a downsampled series does not report truncation");
				require(approx(series.sampleIntervalSeconds,
					std::pow(2.0, static_cast<double>(series.downsampleLevel))),
					"effective stride is not the base cadence times 2^level");
				require(78000 - series.samples.back().worldTick < static_cast<uint64_t>(
					60.0 * series.sampleIntervalSeconds + 0.5),
					"the series lost the newest sample: the newest retained sample is not within one stride of the run end");
				for (size_t index = 1; index < series.samples.size(); ++index)
				{
					auto const gap = series.samples[index].worldTick - series.samples[index - 1].worldTick;
					require(gap >= 60 && gap <= static_cast<uint64_t>(
						60.0 * series.sampleIntervalSeconds + 0.5),
						"downsampled series has a gap outside the retained stride");
				}
				for (auto const& sample : series.samples)
					require(approx(sample.requested, sample.actual),
						"a fully supplied series merged into unequal requested/actual values");
				}
			auto const* power = findSeries(trends, "power", "");
			require(power != nullptr && approx(power->samples.back().requested, 6.0)
				&& approx(power->samples.back().actual, 6.0),
				"the downsampled power trend drifted from the steady 6 kW");
		}
		// Scenario C: the authored retention caps the retained window in
		// simulation seconds. A 60 s retention over a 200 s run keeps at
		// most the last minute of samples and marks the series truncated.
		{
			core::World world("TrendRetention", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.trendRetentionSeconds = 60.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 12000, "trend retention"); // t = 200 s
			auto const trends = world.getStationSystemsTrends();
			for (auto const& series : trends)
			{
				require(series.downsampleLevel == 0,
					"the retention window forced a downsample it did not need");
				require(series.truncated,
					"retention dropping old samples did not mark the series truncated");
				require(series.samples.back().elapsedSeconds
					- series.samples.front().elapsedSeconds <= 60.0 + 1e-9,
					"the retained window exceeds the authored retention");
				require(series.samples.size() <= 62 && series.samples.size() >= 2,
					"retention kept a sample count outside the window it declares");
			}
		}
		// Scenario D: observation never mutates. Two identical rigs run
		// the same ticks; one has its trends, snapshot and event history
		// read repeatedly during the run, the other is never read until
		// the end. Both must finish identical - visibility/plotting and
		// UI update frequency cannot change simulated results.
		{
			core::World quiet("TrendQuiet", 2, 2);
			quiet.pauseSimulation(); quiet.finishBuild();
			require(quiet.configureStationSystems(rigSettings(), false, &diagnostic), diagnostic.c_str());
			core::World noisy("TrendNoisy", 2, 2);
			noisy.pauseSimulation(); noisy.finishBuild();
			require(noisy.configureStationSystems(rigSettings(), false, &diagnostic), diagnostic.c_str());
			quiet.resumeSimulation(); noisy.resumeSimulation();
			for (uint64_t leg = 0; leg < 8; ++leg)
			{
				runOrDiagnose(quiet, 50, "trend quiet twin");
				runOrDiagnose(noisy, 50, "trend noisy twin");
				(void)noisy.getStationSystemsTrends();
				(void)noisy.getStationSystemsSnapshot();
				(void)noisy.getStationSystemsEvents();
			}
			require(quiet.getStationSystemsSnapshot() == noisy.getStationSystemsSnapshot(),
				"reading trends/snapshots changed the simulated result");
			require(quiet.getStationSystemsTrends() == noisy.getStationSystemsTrends(),
				"reading trends changed the trend history");
			require(!quiet.getStationSystemsTrends().empty(),
				"the twin run produced no trends at all");
		}
		// Scenario E: Reset reconstructs. After a reset the trend history
		// is gone - nothing leaks across the boundary - and a fresh run
		// rebuilds the series from tick zero with no truncation.
		{
			core::World world("TrendReset", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(world.configureStationSystems(rigSettings(), false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 6000, "trend reset pre-run"); // t = 100 s
			require(!world.getStationSystemsTrends().empty(),
				"the pre-reset run produced no trends");
			world.resetSimulation();
			require(world.getStationSystemsTrends().empty(),
				"reset left trend history behind");
			require(world.getStationSystemsSnapshot().worldTick == 0,
				"reset did not return the World to tick zero");
			runOrDiagnose(world, 600, "trend post-reset run"); // t = 10 s
			auto const trends = world.getStationSystemsTrends();
			for (auto const& series : trends)
			{
				require(series.samples.size() == 10,
					"the post-reset series did not restart its sample window");
				require(series.downsampleLevel == 0 && !series.truncated,
					"the post-reset series carried downsampling or truncation across the reset");
				require(series.samples.front().worldTick == 60,
					"the post-reset series did not start from tick zero");
			}
		}
		// Scenario F: the authored retention persists with the World and
		// pre-95 documents carrying it are refused rather than silently
		// dropped; a pre-95 document without it loads the default.
		{
			auto const path = context.temporaryRoot() / "trends-roundtrip.world.yaml";
			core::World original("TrendSave", 2, 2);
			original.pauseSimulation(); original.finishBuild();
			auto settings = rigSettings();
			settings.trendRetentionSeconds = 7200.0f;
			require(original.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			original.resumeSimulation();
			runOrDiagnose(original, 300, "trend save run");
			original.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the trend document failed to reopen");
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"the trend retention did not survive the document round trip");
			require(reopened->getStationSystemsSettings().trendRetentionSeconds == 7200.0f,
				"the reopened retention is not the authored 7200 s");
			auto const text = yaml(original);
			require(text.find("trendRetentionSeconds: 7200") != std::string::npos,
				"the saved document does not carry the authored retention");
			// A pre-95 document carrying the field is refused, not silently
			// dropped, and the refusal names the schema it needs.
			auto stale = replaceAll(text, "version: 99", "version: 94");
			bool refused = false;
			std::string refusal;
			try
			{
				auto staleReader = core::YamlSerializer::fromString(stale);
				staleReader->deserialize();
				core::SerializationWorkData data;
				core::World staleWorld("Stale", 2, 2);
				staleWorld.deserialize(*staleReader, data);
			}
			catch (std::exception const& error) { refused = true; refusal = error.what(); }
			require(refused && refusal.find("trend retention requires World schema 95")
				!= std::string::npos,
				("a pre-95 document carrying the trend retention was not refused by name: " + refusal).c_str());
			// The same pre-95 document without the field loads with the
			// default retention - compatible with documents that predate
			// the feature.
			stale = replaceAll(stale, "trendRetentionSeconds: 7200", "");
			bool loaded = false;
			try
			{
				auto legacyReader = core::YamlSerializer::fromString(stale);
				legacyReader->deserialize();
				core::SerializationWorkData data;
				core::World legacyWorld("LegacyTrends", 2, 2);
				legacyWorld.deserialize(*legacyReader, data);
				loaded = legacyWorld.getStationSystemsSettings().trendRetentionSeconds == 3600.0f;
			}
			catch (std::exception const& error)
			{
				std::fprintf(stderr, "legacy trend document failed: %s\n", error.what());
			}
			require(loaded, "a pre-95 document without the retention field did not load the default");
		}
		// Scenario G: the stable presentation contract. Init, observation
		// and eclipse-transition events each carry their declared category,
		// so incidents render by subsystem without message parsing.
		{
			core::World world("TrendEvents", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			core::ProfileSettings flicker;
			flicker.periodSeconds = 20.0;
			flicker.segments = {{0.0, 1.0}, {10.0, 0.0}};
			settings.solarArrays.front().illumination = flicker;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1500, "trend event categories"); // t = 25 s: eclipse legs
			bool sawStation = false, sawObservation = false, sawSolar = false;
			for (auto const& event : world.getStationSystemsEvents())
			{
				if (event.message.rfind("Station systems initialized", 0) == 0)
				{
					require(event.category == "station",
						"the initialization event does not carry the station category");
					sawStation = true;
				}
				if (event.message.find("completed the World tick") != std::string::npos)
				{
					require(event.category == "observation",
						"the observation event does not carry the observation category");
					sawObservation = true;
				}
				if (event.message.rfind("Solar array", 0) == 0)
				{
					require(event.category == "solar",
						"a solar transition event does not carry the solar category");
					sawSolar = true;
				}
				if (event.message.rfind("Transfer '", 0) == 0)
					require(event.category == "transfer",
						"a transfer event does not carry the transfer category");
			}
			require(sawStation && sawObservation && sawSolar,
				"the bounded history lost one of the categorized event producers");
		}
		// Scenario H: the #603 regression. The ticket reproduced at a
		// station with no physical installations: 4500 s at the ordinary
		// 1 s observation cadence left the station-wide power series
		// declaring a ~545-year stride while new samples still arrived
		// every second, and the level-34 presentation shift was
		// undefined behavior. With cadence-aware decimation the series
		// is uniformly spaced at its declared stride all the way
		// through: every retained sample stands for exactly the declared
		// number of seconds, the level stays small because coarsening
		// itself slows the recording cadence, and the audit passes.
		{
			core::World world("TrendStrideTruth", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 86400.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 270000, "trend stride truth run"); // t = 4500 s
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4, "the 4500 s run lost a trend series");
			auto const recorded = world.getStationSystemsMetricHistory();
			auto const recording = world.getStationSystemsRecordingStatus();
			require(!recording.limited && recording.recordingBytes <= 32 * 1024 * 1024
				&& recording.coarseningLevel > 0, "World did not expose its bounded shared recorder");
			for (auto const& metric : recorded)
			{
				require(metric.samples.size() == recorded.front().samples.size(), "World metric timelines differ in length");
				for (size_t i = 0; i < metric.samples.size(); ++i)
					require(metric.samples[i].elapsedSeconds == recorded.front().samples[i].elapsedSeconds
						&& metric.samples[i].coverage == 1, "World metric timelines or coverage lost alignment");
			}
			for (auto const& series : trends)
			{
				require(series.samples.size() <= 256,
					"the coarsened series exceeded the sample bound");
				require(series.downsampleLevel >= 1,
					"a 4500-sample run did not coarsen at all");
				require(series.downsampleLevel <= 6,
					"cadence-aware decimation coarsened far past what the run length needs");
				require(approx(series.sampleIntervalSeconds,
					std::pow(2.0, static_cast<double>(series.downsampleLevel))),
					"the declared stride is not the base cadence times 2^level");
				auto const strideTicks = static_cast<uint64_t>(
					60.0 * series.sampleIntervalSeconds + 0.5);
				for (size_t index = 1; index < series.samples.size(); ++index)
					require(series.samples[index].worldTick - series.samples[index - 1].worldTick
						== strideTicks,
					"samples are not uniformly spaced at the declared stride");
				require(270000 - series.samples.back().worldTick < strideTicks,
					"the newest retained sample is more than one stride behind the run end");
				require(series.truncated, "a coarsened series does not report truncation");
			}
			auto const audit = core::auditStationStability(world.getStationSystemsSnapshot(),
				world.getStationSystemsEvents(), trends, world.getStationSystemsDiagnostics());
			require(audit.clean, "the coarsened trend history failed the stability audit");
			// The audit must refuse the old failure mode: a series whose
			// samples sit closer together than its declared stride
			// reports a fictitious stride and is a bound violation, and
			// so is a level past the published cap.
			core::StationTrendSeries lying;
			lying.subsystem = "power";
			lying.unit = "kW";
			lying.sampleIntervalSeconds = 32.0;
			lying.downsampleLevel = 5;
			lying.samples.push_back({60, 1.0, 1.0, 1.0});
			lying.samples.push_back({120, 2.0, 1.0, 1.0});
			auto const strideAudit = core::auditStationStability(world.getStationSystemsSnapshot(),
				{}, {lying}, {});
			require(!strideAudit.clean,
				"a series with a fictitious stride audited clean");
			require(std::any_of(strideAudit.boundViolations.begin(), strideAudit.boundViolations.end(),
				[](std::string const& text) { return text.find("below its declared stride") != std::string::npos; }),
				"the fictitious-stride violation was not reported by name");
			lying.downsampleLevel = core::StationTrendDownsampleLevelCap + 1;
			auto const capAudit = core::auditStationStability(world.getStationSystemsSnapshot(),
				{}, {lying}, {});
			require(!capAudit.clean,
				"a downsample level past the cap audited clean");
			require(std::any_of(capAudit.boundViolations.begin(), capAudit.boundViolations.end(),
				[](std::string const& text) { return text.find("exceeds the cap") != std::string::npos; }),
				"the past-cap level violation was not reported by name");
		}
		// Scenario I: retention expiry on a coarsened series. A 1000 s
		// retention over a 5000 s run binds after the second
		// coarsening: the window stays capped at the authored seconds,
		// the stride stays uniform inside it, and the series never
		// coarsens past the point where retention holds the count
		// below the storage bound.
		{
			core::World world("TrendRetentionCoarsened", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 1000.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 300000, "retention-bound coarsened run"); // t = 5000 s
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4, "the retention-bound run lost a trend series");
			for (auto const& series : trends)
			{
				require(series.downsampleLevel >= 1,
					"a 5000-sample run did not coarsen before retention bound it");
				require(series.downsampleLevel <= 2,
					"retention failed to hold the count below the storage bound, so coarsening ran on");
				require(series.samples.size() <= 256,
					"the retention-bound series exceeded the sample bound");
				auto const window = series.samples.back().elapsedSeconds
					- series.samples.front().elapsedSeconds;
				require(window <= 1000.0 + 1e-9,
					"the retained window exceeds the authored retention");
				require(window >= 900.0,
					"the retention window did not actually fill the authored seconds");
				auto const strideTicks = static_cast<uint64_t>(
					60.0 * series.sampleIntervalSeconds + 0.5);
				for (size_t index = 1; index < series.samples.size(); ++index)
					require(series.samples[index].worldTick - series.samples[index - 1].worldTick
						== strideTicks,
					"the retention-bound series lost uniform spacing");
				require(series.truncated, "a retention-trimmed series does not report truncation");
			}
		}
		// Scenario J: reset and replay. After a reset the coarsened
		// history is gone, and re-running the same ticks reproduces the
		// exact same series - the coarsening is a deterministic
		// function of the tick grid, not of accumulated drift.
		{
			core::World world("TrendReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 86400.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 33000, "replay pre-reset run"); // t = 550 s: coarsened once
			auto const firstRun = world.getStationSystemsTrends();
			require(firstRun.size() == 4, "the pre-reset replay run produced no trends");
			require(firstRun.front().downsampleLevel >= 1,
				"the pre-reset run did not coarsen, so the replay proves nothing");
			world.resetSimulation();
			require(world.getStationSystemsTrends().empty(),
				"reset left coarsened trend history behind");
			runOrDiagnose(world, 33000, "replay post-reset run");
			require(world.getStationSystemsTrends() == firstRun,
				"the replay after reset did not reproduce the trend history exactly");
		}
		// Scenario K: the primitive exercised far past anything the
		// World-driven bounds can reach - 52 successive coarsenings
		// with nonconstant input, through the level cap and into the
		// sliding-window regime. A shadow model of the documented merge
		// semantics tracks every accepted value; the series must match
		// it exactly below the cap, stay uniformly spaced at the
		// declared stride throughout, and keep every published number
		// finite past the cap.
		{
			std::vector<core::StationTrendSeries> series;
			double const base = 1.0;
			double const retention = 1e18; // never binds: pure coarsening
			uint64_t tick = 0;
			double value = 0.0;
			uint32_t level = 0;
			int overflows = 0;
			std::vector<double> expected;
			while (overflows < 52)
			{
				// Mirror the primitive's own order: coarsen first when
				// the buffer is full, then record at the new stride.
				if (expected.size() >= 256)
				{
					std::vector<double> merged;
					for (size_t index = 0; index + 1 < expected.size(); index += 2)
						merged.push_back((expected[index] + expected[index + 1]) / 2.0);
					if (expected.size() % 2 != 0) merged.push_back(expected.back());
					expected = std::move(merged);
					++level;
					++overflows;
				}
				auto const effective = std::min(level, core::StationTrendDownsampleLevelCap);
				auto const strideTicks = static_cast<uint64_t>(60) << effective;
				tick += strideTicks;
				value += 1.0;
				core::appendBoundedTrendSample(series, "power", "", "kW", base, retention,
					tick, static_cast<double>(tick) / 60.0, value, value);
				expected.push_back(value);
				auto const& s = series.front();
				require(s.downsampleLevel == effective,
					"the primitive level diverged from the documented coarsening");
				require(s.samples.size() <= 256,
					"the primitive exceeded the sample bound");
				require(approx(s.sampleIntervalSeconds,
					std::ldexp(base, static_cast<int>(effective))),
					"the primitive stride is not base x 2^level");
				require(std::isfinite(s.sampleIntervalSeconds),
					"the stride went non-finite during repeated coarsening");
				for (size_t index = 1; index < s.samples.size(); ++index)
					require(s.samples[index].worldTick - s.samples[index - 1].worldTick
						== strideTicks,
						"the primitive lost uniform spacing across a coarsening");
				if (level <= core::StationTrendDownsampleLevelCap)
				{
					require(s.samples.size() == expected.size(),
						"the primitive and the documented merge semantics kept different counts");
					for (size_t index = 0; index < expected.size(); ++index)
						require(approx(s.samples[index].requested, expected[index])
							&& approx(s.samples[index].actual, expected[index]),
							"a merged value disagrees with the documented pairwise mean");
				}
			}
			auto const& s = series.front();
			require(s.downsampleLevel == core::StationTrendDownsampleLevelCap,
				"the level ran past the cap instead of sliding the window");
			require(s.samples.size() <= 256 && approx(s.samples.back().requested, value),
				"the sliding window lost the newest sample or grew past the bound");
			require(s.truncated, "the capped sliding window does not report truncation");
			for (auto const& sample : s.samples)
				require(std::isfinite(sample.requested) && std::isfinite(sample.actual)
					&& std::isfinite(sample.elapsedSeconds),
					"a sample went non-finite past the level cap");
		}
	}

	// #419: per-resource endurance explanation. For every Resource store
	// and battery the snapshot distinguishes the physical total, the
	// usable/qualified inventory and the protected reserve, reports the
	// net rate observed over the declared observation interval, and turns
	// that into a basis (unknown / depleted / no-net-change / growing /
	// depleting) with a stability claim and bounded estimates - never an
	// unjustified infinite countdown. The estimate is the explicit static
	// current-rate assumption: the observed net rate continues, no future
	// profile or consumption change is forecast. Next-delivery context is
	// shown per store without claiming a survival countdown.
	void endurance(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-9)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const constantProfile = [](double level)
		{ core::ProfileSettings p; p.constantLevel = level; return p; };
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findEndurance = [](std::vector<core::StationEnduranceSnapshot> const& entries,
			std::string const& kind, std::string const& id) -> core::StationEnduranceSnapshot const*
		{
			for (auto const& entry : entries)
				if (entry.kind == kind && entry.id == id) return &entry;
			return nullptr;
		};
		std::string diagnostic;
		// Scenario A: steady depletion with a next delivery in context.
		// A 2 kg/s transfer drains a store whose protected reserve is
		// withheld; the estimate is drawable / observed rate, the first
		// window carries no stability claim, the second confirms stability,
		// and the soonest authored delivery is reported as context.
		{
			core::World world("EnduranceDepletion", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 600.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings resupply;
			resupply.id = "tanker"; resupply.storeId = "water"; resupply.unit = "kg";
			resupply.quantity = 100.0; resupply.firstOccurrenceSeconds = 300.0;
			resupply.recurrenceSeconds = 100.0;
			settings.deliveries = { resupply };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			// Before the first window completes nothing is known: no rate,
			// no estimate - and never an infinite countdown.
			runOrDiagnose(world, 300, "endurance pre-window"); // t = 5 s
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const* entry = findEndurance(snap.endurance, "store", "water");
				require(entry != nullptr, "no endurance entry for the depleted store");
				require(entry->unit == "kg", "endurance entry lost the store's declared unit");
				require(entry->basis == core::StationEnduranceBasis::Unknown,
					"endurance claimed a basis before any observation window completed");
				require(entry->stability == core::StationRateStability::Unknown,
					"endurance claimed stability with no observation");
				require(approx(entry->estimateSeconds, 0.0),
					"an unobserved store reported an endurance estimate");
				require(approx(entry->protectedReserve, 100.0)
					&& approx(entry->drawableUsable, 490.0, 1e-4),
					"the reserve/drawable split is not the qualified amount above the protected reserve");
				require(entry->hasNextDelivery && approx(entry->nextDeliverySeconds, 300.0)
					&& approx(entry->nextDeliveryQuantity, 100.0)
					&& entry->nextDeliveryArrivesQualified,
					"the next-delivery context is missing or wrong before the first window");
			}
			runOrDiagnose(world, 300, "endurance first window"); // t = 10 s: first boundary
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const* entry = findEndurance(snap.endurance, "store", "water");
				require(entry != nullptr, "endurance entry vanished after the first window");
				require(entry->basis == core::StationEnduranceBasis::Depleting,
					"a steady drain did not read as depleting");
				require(entry->stability == core::StationRateStability::Unknown,
					"a single observation window claimed stability");
				require(approx(entry->observedNetRatePerSecond, -2.0, 1e-5),
					"the observed net rate does not match the 2 kg/s drain");
				require(approx(entry->observationWindowSeconds, 10.0, 1e-6)
					&& approx(entry->observedUntilSeconds, 10.0, 1e-6),
					"the observation window is not the declared observation interval");
				// drawable 480 at t=10, rate 2 kg/s -> 240 s static estimate.
				require(approx(entry->drawableUsable, 480.0, 1e-4),
					"drawable inventory drifted from the committed ledger");
				require(approx(entry->estimateSeconds, 240.0, 1e-4),
					"the static current-rate estimate is not drawable / observed rate");
				require(std::isfinite(entry->estimateSeconds),
					"the endurance estimate is not finite");
				require(approx(entry->boundMinSeconds, entry->estimateSeconds, 1e-9)
					&& approx(entry->boundMaxSeconds, 0.0),
					"a single-window drain must claim its estimate as the lower bound with no upper bound");
			}
			runOrDiagnose(world, 600, "endurance second window"); // t = 20 s
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const* entry = findEndurance(snap.endurance, "store", "water");
				require(entry->stability == core::StationRateStability::Stable,
					"two identical windows did not establish a stable rate");
				require(approx(entry->drawableUsable, 460.0, 1e-4),
					"drawable inventory drifted on the second window");
				require(approx(entry->estimateSeconds, 230.0, 1e-4),
					"the stable estimate is not drawable / observed rate on the second window");
				require(approx(entry->boundMaxSeconds, entry->estimateSeconds, 1e-6),
					"a stable rate must bound above at the same value it bounds below");
				require(approx(entry->physicalTotal, 560.0, 1e-4)
					&& approx(entry->usableQualified, 560.0, 1e-4),
					"physical total / usable qualified do not match the committed store state");
				require(entry->hasNextDelivery && approx(entry->nextDeliverySeconds, 300.0),
					"the next-delivery context moved off the authored schedule");
				auto const* tank = findEndurance(snap.endurance, "store", "tank");
				require(tank != nullptr && tank->basis == core::StationEnduranceBasis::Growing
					&& tank->observedNetRatePerSecond > 0.0,
					"the receiving store did not read as growing");
				require(approx(tank->estimateSeconds, 0.0),
					"a growing store reported a depletion countdown");
			}
		}
		// Scenario B: zero demand. A store with no flows at all reads as
		// no-net-change after its windows complete - explicitly not a
		// countdown, never an infinite endurance.
		{
			core::World world("EnduranceZeroDemand", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings idle;
			idle.id = "idle"; idle.unit = "L";
			idle.capacity = 500.0; idle.startingQualified = 250.0;
			settings.stores = { idle };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 900, "endurance zero demand"); // t = 15 s, three windows
			auto const snap = world.getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "store", "idle");
			require(entry != nullptr, "no endurance entry for the idle store");
			require(entry->basis == core::StationEnduranceBasis::NoNetChange,
				"a store with no flows did not read as no-net-change");
			require(entry->stability == core::StationRateStability::Stable,
				"a zero rate across windows is not stable");
			require(approx(entry->observedNetRatePerSecond, 0.0),
				"the observed net rate of an untouched store is not zero");
			require(approx(entry->estimateSeconds, 0.0)
				&& approx(entry->boundMinSeconds, 0.0)
				&& approx(entry->boundMaxSeconds, 0.0),
				"no-net-change must not publish any countdown");
			require(!entry->hasNextDelivery,
				"a store with no authored deliveries reported next-delivery context");
		}
		// Scenario C: a sudden rate change. An equipment load whose
		// operating profile steps 4x at t=20 makes the drain rate triple
		// across the t=30 boundary: the rate is unstable, so the estimate
		// is a bound built from the fastest and slowest observed drains,
		// not a single precise countdown.
		{
			core::World world("EnduranceRateChange", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings feed;
			feed.id = "feed"; feed.unit = "kg";
			feed.capacity = 10000.0; feed.startingQualified = 5000.0;
			settings.stores = { feed };
			core::SolarArraySettings sun;
			sun.id = "sun"; sun.capacityKW = 10.0; sun.illumination = constantProfile(1.0);
			settings.solarArrays = { sun };
			core::EquipmentLoadSettings pump;
			pump.id = "pump"; pump.electricalDemandKW = 1.0;
			core::ProfileSettings step;
			step.periodSeconds = 100000.0;
			step.segments = {{0.0, 0.25}, {25.0, 1.0}};
			pump.operatingProfile = step;
			pump.heatOutputKW = 0.0; pump.thermalClass = "low";
			pump.coolingDisposition = core::StationCoolingDisposition::Exported;
			pump.criticalFloorKW = 0.0; pump.priority = 10; pump.maxCurtailment = 1.0;
			core::EquipmentMaterialExchangeSettings draw;
			draw.storeId = "feed"; draw.ratePerSecond = 4.0; draw.consumes = true;
			pump.materialExchanges = { draw };
			settings.equipmentLoads = { pump };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1800, "endurance rate change pre-step"); // t = 30 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "store", "feed");
			require(entry != nullptr, "no endurance entry for the equipment-fed store");
			require(entry->basis == core::StationEnduranceBasis::Depleting,
				"the post-step drain did not read as depleting");
			require(entry->stability == core::StationRateStability::Unstable,
				"a 4x profile step across the window boundary did not mark the rate unstable");
			// The window t=20..30 ran half its ticks at 0.25 x 4 = 1 kg/s
			// and half at the full 4 kg/s: the observed rate is the
			// average, about -2.5 kg/s (the boundary tick lands on the
			// later segment).
			require(approx(entry->observedNetRatePerSecond, -2.505, 1e-2),
				"the post-step window rate is not the profile-averaged draw");
			// The previous window ran at 1 kg/s. The bound is built from
			// both drains: fastest gives the conservative lower bound,
			// slowest the upper bound.
			auto const drawable = entry->drawableUsable;
			require(approx(entry->boundMinSeconds * std::abs(entry->observedNetRatePerSecond),
				drawable, 1e-6),
				"the unstable lower bound is not drawable over the fastest observed drain");
			require(approx(entry->boundMaxSeconds, drawable / 1.0, 1.0),
				"the unstable upper bound is not the slowest observed drain (1 kg/s)");
			require(approx(entry->estimateSeconds, entry->boundMinSeconds, 1e-9),
				"the unstable estimate is not the conservative lower bound");
			require(entry->boundMinSeconds < entry->boundMaxSeconds,
				"an unstable rate collapsed to a single unjustified point estimate");
		}
		// Scenario D: unavailable inventory. Material below the protected
		// reserve and material held unqualified are both not drawable: the
		// store reads depleted with a zero estimate, while the physical
		// total still shows what the station owns.
		{
			core::World world("EnduranceUnavailable", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings brine;
			brine.id = "brine"; brine.unit = "kg";
			brine.capacity = 100.0; brine.startingQualified = 50.0;
			brine.protectedReserve = 50.0; // everything qualified is withheld
			core::ResourceStoreSettings held;
			held.id = "held"; held.unit = "kg";
			held.capacity = 100.0; held.startingQualified = 0.0;
			held.startingUnqualified = 80.0; // owned but not usable
			settings.stores = { brine, held };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "endurance unavailable inventory"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* brineEntry = findEndurance(snap.endurance, "store", "brine");
			require(brineEntry != nullptr
				&& brineEntry->basis == core::StationEnduranceBasis::Depleted
				&& approx(brineEntry->drawableUsable, 0.0)
				&& approx(brineEntry->estimateSeconds, 0.0),
				"inventory fully inside the protected reserve did not read depleted");
			auto const* heldEntry = findEndurance(snap.endurance, "store", "held");
			require(heldEntry != nullptr
				&& heldEntry->basis == core::StationEnduranceBasis::Depleted
				&& approx(heldEntry->physicalTotal, 80.0, 1e-6)
				&& approx(heldEntry->usableQualified, 0.0)
				&& approx(heldEntry->drawableUsable, 0.0),
				"unqualified holdings were counted as usable endurance");
		}
		// Scenario E: battery energy/power distinctions. Endurance is
	// energy over the observed energy rate: the reserve floor is
	// excluded from the drawable energy, the discharge power rating is
	// reported as a constraint on how fast the endurance can be drawn
	// and is never itself the endurance rate.
		{
			core::World world("EnduranceBattery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::BatterySettings bank;
			bank.id = "bank"; bank.nameplateKWh = 10.0; bank.usableKWh = 8.0;
			bank.startingStoredKWh = 10.0;
			bank.maxChargeKW = 5.0; bank.maxDischargeKW = 3.6;
			settings.batteries = { bank };
			core::SolarArraySettings night;
			night.id = "night"; night.capacityKW = 10.0;
			night.illumination = constantProfile(0.0);
			settings.solarArrays = { night };
			core::EquipmentLoadSettings load;
			load.id = "life"; load.electricalDemandKW = 1.8;
			load.operatingProfile = constantProfile(1.0);
			load.heatOutputKW = 0.0; load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			load.criticalFloorKW = 0.0; load.priority = 10; load.maxCurtailment = 1.0;
			settings.equipmentLoads = { load };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "endurance battery window"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "battery", "bank");
			require(entry != nullptr, "no endurance entry for the battery");
			require(entry->unit == "kWh", "battery endurance must be reported in energy units");
			require(approx(entry->physicalTotal, 10.0) && approx(entry->usableQualified, 8.0)
				&& approx(entry->protectedReserve, 2.0),
				"the battery total/usable/reserve split is not nameplate/cyclable/nameplate-minus-usable");
			require(approx(entry->maxDrawPowerKW, 3.6),
				"the discharge power constraint is missing from the battery endurance entry");
			require(entry->basis == core::StationEnduranceBasis::Depleting,
				"a battery covering a deficit did not read as depleting");
			// 1.8 kW = 1.8 kJ/s = 0.0005 kWh/s of stored energy.
			require(approx(entry->observedNetRatePerSecond, -0.0005, 1e-7),
				"the observed battery rate is not the stored-energy rate of the deficit");
			auto const drawable = entry->drawableUsable;
			require(approx(drawable, 7.995, 1e-5),
				"drawable battery energy is not stored above the reserve floor");
			require(approx(entry->estimateSeconds, drawable / 0.0005, 1e-3),
				"the battery endurance is not drawable energy over the observed energy rate");
			// The energy/power distinction: dividing energy by the power
			// rating (kWh / kW) is hours, not the seconds-scale endurance
			// the observed rate produces; the estimate must never equal it.
			require(std::abs(entry->estimateSeconds - drawable / 3.6) > 1.0,
				"the battery endurance was computed from the power rating, not the observed energy rate");
			require(entry->estimateSeconds < drawable / 0.0005 + 1.0
				&& std::isfinite(entry->estimateSeconds),
				"the battery endurance estimate is not a finite seconds-scale number");
		}
		// Scenario F: Reset clears the observation history. After a reset
		// every entry is back to Unknown (or Depleted where that is knowable
		// from the authored state alone) - no rate or estimate leaks across
		// the boundary - and a fresh run re-observes from the authored start.
		{
			core::World world("EnduranceReset", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings store;
			store.id = "air"; store.unit = "kg";
			store.capacity = 1000.0; store.startingQualified = 400.0;
			store.protectedReserve = 50.0;
			settings.stores = { store };
			core::ResourceTransferSettings bleed;
			bleed.id = "bleed"; bleed.sourceId = "air";
			bleed.destinationId = "air"; bleed.unit = "kg";
			bleed.ratePerSecond = 1.0; bleed.priority = 1;
			// A self-transfer is refused by validation; use a second store.
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1000.0; ballast.startingQualified = 0.0;
			settings.stores = { store, ballast };
			bleed.destinationId = "ballast";
			settings.transfers = { bleed };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 900, "endurance reset pre-run"); // t = 15 s, three windows
			auto const before = world.getStationSystemsSnapshot();
			auto const* beforeEntry = findEndurance(before.endurance, "store", "air");
			require(beforeEntry != nullptr
				&& beforeEntry->basis == core::StationEnduranceBasis::Depleting
				&& beforeEntry->stability == core::StationRateStability::Stable,
				"the pre-reset run did not establish a stable depletion");
			world.resetSimulation();
			auto const after = world.getStationSystemsSnapshot();
			auto const* afterEntry = findEndurance(after.endurance, "store", "air");
			require(afterEntry != nullptr, "reset lost the endurance entry");
			require(afterEntry->basis == core::StationEnduranceBasis::Unknown,
				"reset leaked an endurance basis across the boundary");
			require(approx(afterEntry->observedNetRatePerSecond, 0.0)
				&& approx(afterEntry->estimateSeconds, 0.0)
				&& afterEntry->observationWindowSeconds == 0.0,
				"reset leaked observed rate or estimate state");
			runOrDiagnose(world, 300, "endurance post-reset window"); // t = 5 s
			auto const replay = world.getStationSystemsSnapshot();
			auto const* replayEntry = findEndurance(replay.endurance, "store", "air");
			require(replayEntry != nullptr
				&& replayEntry->basis == core::StationEnduranceBasis::Depleting
				&& approx(replayEntry->observedNetRatePerSecond, -1.0, 1e-5)
				&& approx(replayEntry->drawableUsable, 345.0, 1e-4),
				"the post-reset run did not re-observe from the authored start");
		}
		// Scenario G: observation determinism. Two identical rigs run the
		// same ticks; one has its snapshot read repeatedly during the run.
		// Endurance is tick-driven, so UI visibility and read frequency
		// cannot change the reported estimates.
		{
			core::World quiet("EnduranceQuiet", 2, 2);
			quiet.pauseSimulation(); quiet.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 4.0f;
			core::ResourceStoreSettings a;
			a.id = "a"; a.unit = "kg"; a.capacity = 500.0; a.startingQualified = 300.0;
			core::ResourceStoreSettings b;
			b.id = "b"; b.unit = "kg"; b.capacity = 500.0; b.startingQualified = 100.0;
			settings.stores = { a, b };
			core::ResourceTransferSettings t;
			t.id = "t"; t.sourceId = "a"; t.destinationId = "b";
			t.unit = "kg"; t.ratePerSecond = 0.5; t.priority = 1;
			settings.transfers = { t };
			require(quiet.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			core::World noisy("EnduranceNoisy", 2, 2);
			noisy.pauseSimulation(); noisy.finishBuild();
			require(noisy.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			quiet.resumeSimulation(); noisy.resumeSimulation();
			for (uint64_t leg = 0; leg < 10; ++leg)
			{
				runOrDiagnose(quiet, 90, "endurance quiet twin");
				runOrDiagnose(noisy, 90, "endurance noisy twin");
				(void)noisy.getStationSystemsSnapshot();
				(void)noisy.getStationSystemsEvents();
			}
			require(quiet.getStationSystemsSnapshot() == noisy.getStationSystemsSnapshot(),
				"reading snapshots changed the endurance result");
			require(!quiet.getStationSystemsSnapshot().endurance.empty(),
				"the twin run produced no endurance entries at all");
		}
		// Scenario H: persistence compatibility. Endurance is derived
		// runtime state: it never travels in the document. A reopened
		// World carries the authored settings but no observed rates or
		// estimates - it must re-observe from its own ticks.
		{
			core::World original("EnduranceSave", 2, 2);
			original.pauseSimulation(); original.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings store;
			store.id = "air"; store.unit = "kg";
			store.capacity = 1000.0; store.startingQualified = 400.0;
			store.protectedReserve = 50.0;
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1000.0; ballast.startingQualified = 0.0;
			settings.stores = { store, ballast };
			core::ResourceTransferSettings bleed;
			bleed.id = "bleed"; bleed.sourceId = "air"; bleed.destinationId = "ballast";
			bleed.unit = "kg"; bleed.ratePerSecond = 1.0; bleed.priority = 1;
			settings.transfers = { bleed };
			require(original.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			original.resumeSimulation();
			runOrDiagnose(original, 600, "endurance save run"); // t = 10 s, two windows
			auto const path = context.temporaryRoot() / "endurance-roundtrip.world.yaml";
			original.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the endurance document failed to reopen");
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"the station settings did not survive the endurance round trip");
			auto const snap = reopened->getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "store", "air");
			require(entry != nullptr
				&& entry->basis == core::StationEnduranceBasis::Unknown
				&& approx(entry->observedNetRatePerSecond, 0.0)
				&& approx(entry->estimateSeconds, 0.0),
				"observed endurance travelled in the document instead of being re-derived");
		}
	}

	// #420: the bounded scenario forecast. The static estimate is
	// extended from the observed current rate to the configured
	// demand/production profiles and the authored future delivery
	// schedule: projected inventory at resupply, shortfall onset with
	// the limiting service, capacity-capped accepted delivery
	// quantities, and scenario/uncertainty labels that never present
	// biology as guaranteed recovery. The forecast mutates no live
	// inventory, advances no World clock, consumes no deterministic
	// stream, and is bounded by the declared horizon and segment cap.
	void forecast(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findForecast = [](std::vector<core::StationForecastEntry> const& entries,
			std::string const& id) -> core::StationForecastEntry const*
		{
			for (auto const& entry : entries)
				if (entry.storeId == id) return &entry;
			return nullptr;
		};
		auto const findStore = [](std::vector<core::StationSystemsStoreSnapshot> const& stores,
			std::string const& id) -> core::StationSystemsStoreSnapshot const*
		{
			for (auto const& store : stores)
				if (store.id == id) return &store;
			return nullptr;
		};
		auto const chainStores = [](std::string const& id, double capacity,
			double starting) -> core::ResourceStoreSettings
		{
			core::ResourceStoreSettings settings;
			settings.id = id; settings.unit = "kg";
			settings.capacity = capacity; settings.startingQualified = starting;
			return settings;
		};
		auto const chainTransfer = [](std::string const& id, std::string const& src,
			std::string const& dst, double rate, int priority)
			-> core::ResourceTransferSettings
		{
			core::ResourceTransferSettings settings;
			settings.id = id; settings.sourceId = src; settings.destinationId = dst;
			settings.unit = "kg"; settings.ratePerSecond = rate;
			settings.priority = priority;
			return settings;
		};
		std::string diagnostic;
		// Scenario A: a known analytic profile with a timely one-off
		// resupply. A 2 kg/s drain on 180 drawable at t=10 is rescued at
		// t=50 by a 400 kg arrival (projected 100 drawable remaining at
		// the arrival), then runs out at t=300 with the transfer named as
		// the limiting service and the uncovered drain reported.
		{
			core::World world("ForecastTimely", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 100.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 400.0; tanker.firstOccurrenceSeconds = 50.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast timely resupply"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available,
				"the scenario forecast is not available for an enabled Station");
			require(approx(snap.forecast.asOfSeconds, 10.0),
				"the forecast is not as of the snapshot's simulation time");
			require(approx(snap.forecast.horizonSeconds, core::StationForecastHorizonSeconds),
				"the forecast horizon is not the declared bound");
			require(!snap.forecast.truncated, "a sparse schedule truncated the forecast");
			require(snap.forecast.scenarioLabel.find("declared-profile scenario") == 0,
				"the forecast carries no scenario label");
			require(snap.forecast.uncertaintyLabel.find("provisional biology") == 0,
				"the forecast carries no uncertainty label");
			auto const* entry = findForecast(snap.forecast.stores, "water");
			require(entry != nullptr, "no forecast entry for the drained store");
			require(entry->unit == "kg", "the forecast entry lost the store's declared unit");
			require(approx(entry->drawableAtStart, 180.0, 1e-4)
				&& approx(entry->physicalAtStart, 280.0, 1e-4),
				"the forecast start state does not match the committed inventory");
			require(approx(entry->netRateAtStart, -2.0),
				"the projected net rate does not match the authored drain");
			require(entry->basis == core::StationForecastBasis::Shortfall
				&& entry->hasShortfall,
				"a store that runs out inside the horizon did not forecast a shortfall");
			require(approx(entry->shortfallOnsetSeconds, 300.0, 1e-5),
				"the projected depletion onset is not drawable-after-arrival / rate");
			require(entry->limitingServiceId == "pump"
				&& entry->limitingServiceKind == "transfer",
				"the limiting service is not the binding transfer");
			require(approx(entry->projectedDrawableAtHorizon, 0.0),
				"a depleted store did not project zero drawable at the horizon");
			require(entry->arrivals.size() == 1, "the authored arrival is missing");
			if (entry->arrivals.size() == 1)
			{
				auto const& arrival = entry->arrivals[0];
				require(arrival.deliveryId == "tanker" && arrival.storeId == "water",
					"the projected arrival lost its identity");
				require(approx(arrival.arrivalSeconds, 50.0),
					"the projected arrival is not on the authored schedule");
				require(approx(arrival.projectedDrawableBefore, 100.0, 1e-5),
					"the projected inventory at resupply is wrong");
				require(approx(arrival.acceptedQuantity, 400.0)
					&& approx(arrival.rejectedQuantity, 0.0),
					"a delivery that fits was not fully accepted");
				require(arrival.arrivesQualified && arrival.rescuesStore,
					"a timely qualified arrival did not read as a rescue");
			}
			require(approx(entry->unmetOutflowTotal,
				2.0 * (10.0 + core::StationForecastHorizonSeconds - 300.0), 1e-3),
				"the uncovered drain after depletion is not reported");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the projected exchanges do not conserve for the drained store");
			auto const* tankEntry = findForecast(snap.forecast.stores, "tank");
			require(tankEntry != nullptr
				&& tankEntry->basis == core::StationForecastBasis::Sustained
				&& approx(tankEntry->netRateAtStart, 2.0)
				&& approx(tankEntry->projectedDrawableAtHorizon, 700.0, 1e-3),
				"the receiving store did not project as sustained with the delivered total");
			// The forecast mutated nothing: the live store snapshot is
			// unchanged by projection, and a second projection from the
			// same state is identical.
			auto const before = snap.stores;
			auto const again = world.getStationSystemsSnapshot();
			require(again.stores == before,
				"computing the forecast mutated the live store inventory");
			require(again.forecast == snap.forecast,
				"the forecast is not a deterministic function of the committed state");
			require(world.getSimulationTick() == snap.worldTick,
				"computing the forecast advanced the World");
			require(world.getStationSystemsDiagnostics().empty(),
				"the forecast produced a Station diagnostic");
			// Determinism across two identical worlds: the same scenario
			// projects identically, so no hidden stream was consumed.
			core::World twin("ForecastTimelyTwin", 2, 2);
			twin.pauseSimulation(); twin.finishBuild();
			require(twin.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			twin.resumeSimulation();
			runOrDiagnose(twin, 600, "forecast twin run");
			require(twin.getStationSystemsSnapshot().forecast == snap.forecast,
				"two identical scenario runs projected differently");
			// Persistence: the forecast is derived, never stored. A reset
			// re-derives it from the initial state, and a saved and
			// reopened World - which starts from that same initial state -
			// re-derives the identical projection.
			world.pauseSimulation();
			world.resetSimulation();
			auto const reset = world.getStationSystemsSnapshot().forecast;
			require(reset.asOfSeconds == 0.0
				&& approx(reset.stores.size(), 2.0)
				&& approx(findForecast(reset.stores, "water")->drawableAtStart, 200.0, 1e-4)
				&& approx(findForecast(reset.stores, "water")->shortfallOnsetSeconds, 300.0, 1e-5),
				"reset did not re-derive the forecast from the initial state");
			auto const path = context.temporaryRoot() / "forecast-roundtrip.world.yaml";
			world.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the forecast document failed to reopen");
			require(reopened->getStationSystemsSettings() == settings,
				"the station settings did not survive the forecast round trip");
			require(reopened->getStationSystemsSnapshot().forecast == reset,
				"the reopened forecast is not re-derived identically from the initial state");
			require(reopened->getStationSystemsSnapshot().forecast.available,
				"the reopened forecast is not available");
		}
		// Scenario B: delayed resupply. The arrival lands after the
		// projected depletion: the shortfall onset stands, the arrival is
		// not a rescue, and the drain blocked between depletion and
		// arrival is reported as uncovered demand.
		{
			core::World world("ForecastDelayed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 400.0; tanker.firstOccurrenceSeconds = 150.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast delayed resupply"); // t = 10 s
			auto const* entry = findForecast(world.getStationSystemsSnapshot().forecast.stores, "water");
			require(entry != nullptr && entry->hasShortfall,
				"a delayed resupply did not forecast a shortfall");
			require(approx(entry->shortfallOnsetSeconds, 100.0, 1e-5),
				"the depletion onset ignored the delayed arrival");
			require(entry->arrivals.size() == 1 && !entry->arrivals[0].rescuesStore,
				"an arrival after depletion read as a rescue");
			require(approx(entry->arrivals[0].projectedDrawableBefore, 0.0),
				"the projected inventory at a late arrival is not zero");
			require(approx(entry->arrivals[0].acceptedQuantity, 400.0),
				"a late arrival that fits was not accepted");
			// Blocked 100..150 plus blocked again after the second drain
			// ends at 350: the uncovered drain is the whole blocked span.
			require(approx(entry->unmetOutflowTotal,
				2.0 * (50.0 + (10.0 + core::StationForecastHorizonSeconds - 350.0)), 1e-3),
				"the blocked drain around the delayed arrival is not fully reported");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the delayed-resupply projection does not conserve");
		}
		// Scenario C: accepted delivery quantity is capped by the store's
		// projected capacity; what cannot fit is refused at the boundary
		// and reported, never overflowed.
		{
			core::World world("ForecastCapacity", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 400.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 900.0; tanker.firstOccurrenceSeconds = 50.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const* entry = findForecast(world.getStationSystemsSnapshot().forecast.stores, "water");
			require(entry != nullptr && entry->arrivals.size() == 1,
				"the capacity-capped arrival is missing");
			require(approx(entry->arrivals[0].acceptedQuantity, 200.0),
				"the accepted quantity is not the projected headroom");
			require(approx(entry->arrivals[0].rejectedQuantity, 700.0),
				"the refused quantity is not reported");
			require(approx(entry->shortfallOnsetSeconds, 200.0, 1e-5),
				"the depletion after the capped arrival is wrong");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the capped-arrival projection does not conserve");
		}
		// Scenario D: a configured crew with a stepped activity profile.
		// The food draw follows the profile, the food store runs out
		// inside the horizon with the crew named as the limiting service,
		// and - matching the live whole-metabolism rule - the urine and
		// organic-waste returns stop together with the draw, so the waste
		// stores freeze at the value the last fed moment produced.
		{
			core::World world("ForecastCrew", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto store = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				settings.composition = std::move(composition);
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.periodSeconds = 100.0;
			crew.activityProfile.segments = {{0.0, 0.5}, {50.0, 1.0}};
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("food", 100000.0, 101.0, foodComposition),
				store("water", 100000.0, 1000.0, waterComposition),
				store("urine", 100000.0, 1.0, urineComposition),
				store("organicWaste", 100000.0, 1.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			// The projection is read at t=0 from the initial state.
			auto const snap = world.getStationSystemsSnapshot();
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr, "no forecast entry for the crew food store");
			require(approx(food->netRateAtStart, -0.05),
				"the initial food draw is not crew x rate x half-activity");
			require(food->basis == core::StationForecastBasis::Shortfall,
				"the stepping crew draw did not forecast a food shortfall");
			// 13 full periods burn 97.5 kg, the next half-activity leg
			// burns 2.5, and the full-activity leg burns the last 1.0 kg
			// 10 s in: onset 1360 s.
			require(approx(food->shortfallOnsetSeconds, 1360.0, 1e-4),
				"the profiled food depletion onset is wrong");
			require(food->limitingServiceId == "crew"
				&& food->limitingServiceKind == "crew",
				"the limiting service is not the crew");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			require(urine != nullptr
				&& urine->basis == core::StationForecastBasis::Sustained
				&& approx(urine->projectedDrawableAtHorizon, 152.5, 1e-3),
				"the urine return did not freeze at the last fed moment");
			auto const* waste = findForecast(snap.forecast.stores, "organicWaste");
			require(waste != nullptr
				&& approx(waste->projectedDrawableAtHorizon, 21.2, 1e-3),
				"the organic-waste return did not freeze at the last fed moment");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(water != nullptr && water->basis == core::StationForecastBasis::Sustained
				&& approx(water->projectedDrawableAtHorizon, 798.0, 1e-3),
				"the crew water draw over the profiled window is wrong");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the crew-profile projection does not conserve");
			// The live run stays healthy: the forecast is pure observability
			// and the Lua audit is undisturbed.
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast crew live run");
			require(world.getStationSystemsSnapshot().healthy,
				"the crew forecast scenario made the Station unhealthy");
			require(world.getStationSystemsDiagnostics().empty(),
				"the crew forecast scenario produced a diagnostic");
		}
		// Scenario E: uncertainty labels. A store that a crop harvest and
		// a water treatment could replenish is flagged: the possible
		// recovery is visible but is NOT counted as guaranteed, so the
		// projection of the destination stores stays exactly what the
		// declared non-biology flows say.
		{
			core::World world("ForecastUncertainty", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings cropComposition;
			cropComposition.moisture = 0.0;
			cropComposition.nutrientNitrogen = 0.05;
			cropComposition.atmosphericNitrogen = 0.0;
			cropComposition.phosphorus = 0.02;
			cropComposition.potassium = 0.03;
			cropComposition.otherMinerals = 0.05;
			cropComposition.energyKJPerKgDry = 1700.0;
			auto plain = [](std::string const& id, double capacity, double starting)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 40.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingCarbonDioxideKg = 5.0; habitat.startingWaterVapourKg = 1.0;
			habitat.startingTemperatureC = 22.0;
			habitat.thermalCapacityKJPerK = 20000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CropPopulationSettings canopy;
			canopy.id = "canopy"; canopy.atmosphereId = "habitat";
			canopy.waterStoreId = "irrigation";
			canopy.nitrogenStoreId = "fertN"; canopy.phosphorusStoreId = "fertP";
			canopy.potassiumStoreId = "fertK"; canopy.mineralsStoreId = "fertMin";
			canopy.composition = cropComposition;
			canopy.growingAreaM2 = 10.0; canopy.maxBiomassDensityKgPerM2 = 2.0;
			canopy.initialBiomassKg = 1.0;
			canopy.lampRatedKW = 3.6; canopy.illuminationProfile.constantLevel = 1.0;
			canopy.circulationPowerKW = 0.1;
			canopy.growthYieldKgPerKWh = 2.0; canopy.respirationPerHour = 0.5;
			canopy.transpirationKgPerKgGrowth = 5.0;
			canopy.minGrowingTemperatureC = 10.0; canopy.maxGrowingTemperatureC = 35.0;
			canopy.foodStoreId = "food"; canopy.wasteStoreId = "residue";
			canopy.edibleFraction = 0.5; canopy.harvestTriggerKg = 5.0;
			canopy.harvestFraction = 1.0;
			core::WaterTreatmentSettings treatment;
			treatment.id = "polish"; treatment.hazardClass = "normal";
			treatment.sourceStoreId = "raw"; treatment.treatedStoreId = "treated";
			treatment.concentrateStoreId = "concentrate";
			treatment.consumableStoreId = "consumable";
			treatment.throughputKgPerS = 0.005; treatment.recoveryRatio = 0.8;
			treatment.specificEnergyKWhPerKg = 0.5; treatment.consumablePerKg = 0.01;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				plain("irrigation", 200.0, 150.0), plain("fertN", 10.0, 5.0),
				plain("fertP", 10.0, 5.0), plain("fertK", 10.0, 5.0),
				plain("fertMin", 10.0, 5.0),
				plain("food", 1000.0, 100.0), plain("residue", 1000.0, 1.0),
				plain("raw", 1000.0, 100.0), plain("treated", 1000.0, 1.0),
				plain("concentrate", 1000.0, 0.0), plain("consumable", 100.0, 10.0) };
			settings.atmospheres = { habitat };
			settings.cropPopulations = { canopy };
			settings.waterTreatments = { treatment };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr && food->biologyRecoveryPossible,
				"a crop-destined store is not flagged for uncertain biology recovery");
			require(food->basis == core::StationForecastBasis::Sustained
				&& approx(food->netRateAtStart, 0.0)
				&& food->arrivals.empty(),
				"the uncertain harvest was counted into the guaranteed projection");
			auto const* residue = findForecast(snap.forecast.stores, "residue");
			require(residue != nullptr && residue->biologyRecoveryPossible,
				"the crop waste destination is not flagged for uncertain biology");
			auto const* treated = findForecast(snap.forecast.stores, "treated");
			require(treated != nullptr && treated->processProductionPossible,
				"a treatment-destined store is not flagged for unprojected process production");
			require(!treated->biologyRecoveryPossible
				&& treated->basis == core::StationForecastBasis::Sustained,
				"the treatment destination carries the wrong uncertainty mix");
			// #594: the consuming side of the same scenario - the crop's
			// uptake stores and the treatment's source and consumable are
			// drained by pathways the forecast does not project, and must
			// carry the matching consumption flags.
			for (auto const* id : {"irrigation", "fertN", "fertP", "fertK", "fertMin"})
			{
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->biologyConsumptionPossible
					&& !entry->processConsumptionPossible,
					"a crop uptake store is not flagged for unprojected biology consumption");
			}
			for (auto const* id : {"raw", "consumable"})
			{
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->processConsumptionPossible
					&& !entry->biologyConsumptionPossible,
					"a treatment feed store is not flagged for unprojected process consumption");
			}
			require(food != nullptr && !food->biologyConsumptionPossible
				&& !food->processConsumptionPossible,
				"the crop food destination carries a consumption flag it does not earn");
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast uncertainty live run");
			require(world.getStationSystemsSnapshot().healthy,
				"the uncertainty forecast scenario made the Station unhealthy");
		}
		// Scenario F: the computation is bounded. A one-second
		// recurrence across the whole horizon cannot make the forecast
		// run forever: collection stops at the declared segment cap and
		// the truncation is reported. #604: a truncated store with no
		// shortfall inside its complete range reports Incomplete, never
		// Sustained, and the snapshot exposes the complete-through bound
		// so the unavailable range beyond it is explicit.
		{
			core::World world("ForecastBounded", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1.0e12; ballast.startingQualified = 1.0;
			settings.stores = { ballast };
			core::ResourceDeliverySettings drip;
			drip.id = "drip"; drip.storeId = "ballast"; drip.unit = "kg";
			drip.quantity = 1.0; drip.firstOccurrenceSeconds = 1.0;
			drip.recurrenceSeconds = 1.0;
			settings.deliveries = { drip };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.truncated,
				"a schedule denser than the segment cap did not truncate the forecast");
			require(snap.forecast.segmentsComputed <= core::StationForecastMaxSegments,
				"the forecast computed more segments than the declared cap");
			require(snap.forecast.completeThroughSeconds > snap.forecast.asOfSeconds
				&& snap.forecast.completeThroughSeconds
					< snap.forecast.asOfSeconds + core::StationForecastHorizonSeconds,
				"a truncated forecast does not report a partial complete-through bound");
			auto const* entry = findForecast(snap.forecast.stores, "ballast");
			require(entry != nullptr
				&& entry->basis == core::StationForecastBasis::Incomplete,
				"a truncated store with no shortfall inside its complete range "
				"did not report Incomplete instead of an unjustified Sustained");
			require(!entry->arrivals.empty(),
				"the truncated projection dropped every already-collected arrival");
			for (auto const& storeEntry : snap.forecast.stores)
				for (auto const& arrival : storeEntry.arrivals)
					require(arrival.arrivalSeconds
						<= snap.forecast.completeThroughSeconds + 1e-6,
						"an arrival outside the complete-through range was reported");
		}
		// Scenario #604-A: the ticket reproduction. A dense unrelated
		// drip (0.1 s recurrence into "noisy") is declared before a
		// one-off 10 kg rescue of the "feed" store, which starts with
		// 2 drawable and drains 1 kg/s to the sink. Without the rescue
		// feed runs out at t=2; with the rescue landing at t=1 it runs
		// out at t=12. The drip still overruns the event budget - the
		// forecast truncates - but the chronological frontier merge
		// keeps the rescue inside the complete-through range, so the
		// depletion onset reads 12 in both declaration orders and
		// matches the sparse control that has no drip at all.
		{
			core::World dense("ForecastDenseRescue", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = { chainStores("noisy", 1.0e9, 1.0),
				chainStores("feed", 100.0, 2.0),
				chainStores("sink", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("drain", "feed", "sink", 1.0, 0) };
			core::ResourceDeliverySettings noise;
			noise.id = "noise"; noise.storeId = "noisy"; noise.unit = "kg";
			noise.quantity = 1.0; noise.firstOccurrenceSeconds = 0.1;
			noise.recurrenceSeconds = 0.1;
			core::ResourceDeliverySettings rescue;
			rescue.id = "rescue"; rescue.storeId = "feed"; rescue.unit = "kg";
			rescue.quantity = 10.0; rescue.firstOccurrenceSeconds = 1.0;
			settings.deliveries = { noise, rescue };
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense drip schedule did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& feed->basis == core::StationForecastBasis::Shortfall,
				"the rescued feed store did not forecast a shortfall");
			require(approx(feed->shortfallOnsetSeconds, 12.0, 1e-3),
				("the dense unrelated schedule dropped the early rescue: onset is "
				+ std::to_string(feed->shortfallOnsetSeconds) + ", expected 12").c_str());
			require(feed->arrivals.size() == 1
				&& feed->arrivals[0].deliveryId == "rescue"
				&& approx(feed->arrivals[0].arrivalSeconds, 1.0)
				&& approx(feed->arrivals[0].acceptedQuantity, 10.0)
				&& feed->arrivals[0].rescuesStore,
				"the rescue arrival is missing or misapplied behind the dense schedule");
			require(denseSnap.completeThroughSeconds > 12.0
				&& denseSnap.completeThroughSeconds
					< denseSnap.asOfSeconds + core::StationForecastHorizonSeconds,
				"the truncated forecast does not cover the rescue but stop short of the horizon");
			for (auto const& storeEntry : denseSnap.stores)
				for (auto const& arrival : storeEntry.arrivals)
					require(arrival.arrivalSeconds
						<= denseSnap.completeThroughSeconds + 1e-6,
						"an arrival outside the complete-through range was reported");
			// Declaration permutation: the rescue declared first must
			// project identically - the frontier merge never lets the
			// drip's budget position depend on declaration order.
			settings.deliveries = { rescue, noise };
			core::World permuted("ForecastDenseRescuePermuted", 2, 2);
			permuted.pauseSimulation(); permuted.finishBuild();
			require(permuted.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const permutedSnap = permuted.getStationSystemsSnapshot().forecast;
			require(permutedSnap.truncated, "the permuted forecast stopped truncating");
			auto const* permutedFeed = findForecast(permutedSnap.stores, "feed");
			require(permutedFeed != nullptr
				&& approx(permutedFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"swapping the declaration order changed the projected depletion onset");
			// Sparse control: the same rig without the dense drip must
			// project the same depletion onset over the full horizon.
			settings.deliveries = { rescue };
			core::World sparse("ForecastDenseRescueControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			require(!sparseSnap.truncated,
				"the sparse control forecast truncated without a dense schedule");
			require(approx(sparseSnap.completeThroughSeconds,
				sparseSnap.asOfSeconds + sparseSnap.horizonSeconds),
				"a complete forecast does not report complete-through at the horizon");
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"the dense-schedule projection disagrees with the sparse control");
		}
		// Scenario #604-B: a dense ejection schedule declared first
		// must not drop a later one-off ejection. The feed store starts
		// with 10 drawable and drains 1 kg/s; a one-off 3 kg ejection
		// at t=1 leaves 6, so depletion lands at t=7. A dense unrelated
		// bilge ejection every 0.1 s overruns the event budget; the
		// merged frontier must still include the one-off whatever the
		// declaration order, matching the sparse control.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = { chainStores("bilge", 1.0e9, 1.0e8),
				chainStores("feed", 100.0, 10.0),
				chainStores("sink", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("drain", "feed", "sink", 1.0, 0) };
			core::WasteEjectionSettings bilgeEject;
			bilgeEject.id = "bilgeEject"; bilgeEject.storeId = "bilge";
			bilgeEject.unit = "kg"; bilgeEject.quantity = 1.0;
			bilgeEject.firstOccurrenceSeconds = 0.1; bilgeEject.recurrenceSeconds = 0.1;
			core::WasteEjectionSettings feedEject;
			feedEject.id = "feedEject"; feedEject.storeId = "feed";
			feedEject.unit = "kg"; feedEject.quantity = 3.0;
			feedEject.firstOccurrenceSeconds = 1.0;
			settings.wasteEjections = { bilgeEject, feedEject };
			core::World dense("ForecastDenseEject", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense ejection schedule did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 7.0, 1e-3),
				("the dense ejection schedule dropped the one-off ejection: onset is "
				+ std::to_string(feed->shortfallOnsetSeconds) + ", expected 7").c_str());
			settings.wasteEjections = { feedEject, bilgeEject };
			core::World permuted("ForecastDenseEjectPermuted", 2, 2);
			permuted.pauseSimulation(); permuted.finishBuild();
			require(permuted.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const permutedSnap = permuted.getStationSystemsSnapshot().forecast;
			auto const* permutedFeed = findForecast(permutedSnap.stores, "feed");
			require(permutedSnap.truncated && permutedFeed != nullptr
				&& approx(permutedFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"swapping the ejection declaration order changed the onset");
			settings.wasteEjections = { feedEject };
			core::World sparse("ForecastDenseEjectControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(!sparseSnap.truncated && controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"the dense-ejection projection disagrees with the sparse control");
		}
		// Scenario #604-C: a dense demand profile must not drop another
		// profile's breakpoints. A crew with a maximally dense activity
		// profile (8 segments per second) is declared before an
		// equipment load whose operating profile (period 100 s) steps on
		// at t=50.05 and drains a feed store holding 62 drawable at
		// 1 kg/s. The crew never touches the feed store; only the load's
		// step at 50.05 matters, and the crew's 0.125 s breakpoints
		// cannot stand in for it. The load draws 49.95 kg during
		// [50.05, 100), idles through [100, 150.05) and takes the last
		// 12.05 kg from 150.05, depleting the store at t=162.1. The
		// crew alone overruns the breakpoint budget so the forecast
		// truncates, but the merged frontier must still have collected
		// the load's 50.05 step, putting depletion at t=162.1 exactly
		// as in the sparse control with no crew. The old
		// declaration-order collection dropped every load breakpoint
		// once the crew filled the budget, starting the drain no earlier
		// than the next 0.125 s crew breakpoint and reporting the
		// depletion late.
		{
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto compound = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg";
				store.capacity = capacity; store.startingQualified = starting;
				store.composition = std::move(composition);
				return store;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "denseCrew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.periodSeconds = 1.0;
			crew.activityProfile.segments = {{0.0, 0.5}, {0.125, 0.6},
				{0.25, 0.7}, {0.375, 0.8}, {0.5, 0.9}, {0.625, 1.0},
				{0.75, 0.9}, {0.875, 0.8}};
			crew.foodStoreId = "rations"; crew.waterStoreId = "potable";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::EquipmentLoadSettings drill;
			drill.id = "drill"; drill.electricalDemandKW = 0.0;
			drill.operatingProfile.periodSeconds = 100.0;
			drill.operatingProfile.segments = {{0.0, 0.0}, {50.05, 1.0}};
			drill.heatOutputKW = 0.0; drill.thermalClass = "low";
			drill.coolingDisposition = core::StationCoolingDisposition::Exported;
			drill.criticalFloorKW = 0.0; drill.priority = 0; drill.maxCurtailment = 1.0;
			core::EquipmentMaterialExchangeSettings draw;
			draw.storeId = "feed"; draw.ratePerSecond = 1.0; draw.consumes = true;
			drill.materialExchanges = { draw };
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				compound("rations", 100000.0, 100000.0, foodComposition),
				compound("potable", 100000.0, 100000.0, waterComposition),
				compound("urine", 100000.0, 1.0, urineComposition),
				compound("organicWaste", 100000.0, 1.0, wasteComposition),
				chainStores("feed", 1000.0, 62.0) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			settings.equipmentLoads = { drill };
			core::World dense("ForecastDenseProfile", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense crew profile did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 162.1, 1e-3),
				("the dense crew profile dropped the load's step breakpoint: onset is "
					+ std::to_string(feed->shortfallOnsetSeconds)
					+ ", expected 162.1").c_str());
			settings.crews.clear();
			settings.atmospheres.clear();
			core::World sparse("ForecastDenseProfileControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(!sparseSnap.truncated && controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-6),
				"the dense-profile projection disagrees with the sparse control");
		}
		// Scenario G: a store that starts depleted reads as a shortfall
		// from the forecast instant, never as sustained supply.
		{
			core::World world("ForecastDepletedStart", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings dry;
			dry.id = "dry"; dry.unit = "kg";
			dry.capacity = 100.0; dry.startingQualified = 0.0;
			core::ResourceStoreSettings held;
			held.id = "held"; held.unit = "kg";
			held.capacity = 100.0; held.startingQualified = 50.0;
			held.protectedReserve = 50.0;
			settings.stores = { dry, held };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* dryEntry = findForecast(snap.forecast.stores, "dry");
			auto const* heldEntry = findForecast(snap.forecast.stores, "held");
			require(dryEntry != nullptr && dryEntry->hasShortfall
				&& approx(dryEntry->shortfallOnsetSeconds, snap.forecast.asOfSeconds),
				"a store with nothing drawable did not forecast an immediate shortfall");
			require(heldEntry != nullptr && heldEntry->hasShortfall
				&& approx(heldEntry->drawableAtStart, 0.0),
				"a store fully inside its protected reserve did not forecast a shortfall");
		}
		// Scenario H: destination backpressure. A transfer into a store
		// that fills inside the horizon stops at the fill-up: the source
		// keeps the material it never got to send, and neither store is
		// a shortage - the capacity limit is the account.
		{
			core::World world("ForecastBackpressure", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings src;
			src.id = "src"; src.unit = "kg";
			src.capacity = 1000.0; src.startingQualified = 200.0;
			core::ResourceStoreSettings dst;
			dst.id = "dst"; dst.unit = "kg";
			dst.capacity = 100.0; dst.startingQualified = 90.0;
			settings.stores = { src, dst };
			core::ResourceTransferSettings feed;
			feed.id = "feed"; feed.sourceId = "src"; feed.destinationId = "dst";
			feed.unit = "kg"; feed.ratePerSecond = 1.0; feed.priority = 1;
			settings.transfers = { feed };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* srcEntry = findForecast(snap.forecast.stores, "src");
			auto const* dstEntry = findForecast(snap.forecast.stores, "dst");
			require(srcEntry != nullptr && srcEntry->basis == core::StationForecastBasis::Sustained
				&& approx(srcEntry->projectedDrawableAtHorizon, 190.0, 1e-3),
				"the source did not keep the material the full destination back-pressured");
			require(approx(srcEntry->unmetOutflowTotal, 0.0),
				"destination backpressure was misreported as a source shortage");
			require(dstEntry != nullptr && dstEntry->basis == core::StationForecastBasis::Sustained
				&& approx(dstEntry->projectedDrawableAtHorizon, 100.0, 1e-3),
				"the backpressured destination did not project exactly full");
			require(std::abs(srcEntry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6
				&& std::abs(dstEntry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the backpressure projection does not conserve");
		}
		// Scenario I: a full waste destination back-pressures the whole
		// crew metabolism. Once the urine store fills the crew draws no
		// food or water and returns nothing - the live whole-metabolism
		// rule (#401) - and the held-back food draw is reported as
		// uncovered demand.
		{
			core::World world("ForecastWasteFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto store = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				settings.composition = std::move(composition);
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("food", 100000.0, 100.0, foodComposition),
				store("water", 100000.0, 1000.0, waterComposition),
				store("urine", 3.0, 0.0, urineComposition),
				store("organicWaste", 100000.0, 1.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			// Urine fills 3 kg at 0.15 kg/s after 20 s; the crew then
			// stops whole: food stays at 100 - 0.1 x 20 = 98 and the
			// held-back draw is uncovered demand.
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr && food->basis == core::StationForecastBasis::Sustained
				&& approx(food->projectedDrawableAtHorizon, 98.0, 1e-2),
				"the full waste destination did not stop the crew food draw");
			require(approx(food->unmetOutflowTotal,
				0.1 * (core::StationForecastHorizonSeconds - 20.0), 1e-1),
				"the held-back crew draw is not reported as uncovered demand");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			require(urine != nullptr
				&& approx(urine->projectedDrawableAtHorizon, 3.0, 1e-2),
				"the urine store did not project exactly to its capacity");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(water != nullptr
				&& approx(water->projectedDrawableAtHorizon, 996.0, 1e-2),
				"the crew water draw did not stop with the metabolism");
		}
		// Scenario J: a transfer chain through a dry intermediate store
		// (#593). A -> B -> C at equal 1 kg/s rates with B starting
		// empty must flow straight through. Gating each transfer on the
		// inventory at the segment start made the solve alternate fill
		// and drain half-duty phases: it truncated the projection at the
		// segment cap, delivered only half the through-flow to C, and
		// invented an unmet transfer demand at B that the live chain
		// never has. The forecast must instead carry the full horizon of
		// through-flow, report no shortage and no uncovered outflow at
		// the dry intermediate, and agree with the live trajectory.
		{
			core::World world("ForecastChainDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 0.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available
				&& !snap.forecast.truncated,
				"a sparse constant-rate transfer chain truncated the forecast");
			require(snap.forecast.segmentsComputed <= 2u,
				"a steady through-flow chain needed more than a couple of segments");
			auto const* a = findForecast(snap.forecast.stores, "A");
			auto const* b = findForecast(snap.forecast.stores, "B");
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(a != nullptr && b != nullptr && c != nullptr,
				"a transfer-chain store is missing from the forecast");
			require(a->netRateAtStart == -1.0
				&& !a->hasShortfall
				&& approx(a->projectedDrawableAtHorizon, 13600.0, 1e-3),
				"the chain source did not project a full-horizon drain");
			require(!b->hasShortfall
				&& approx(b->netRateAtStart, 0.0)
				&& approx(b->unmetOutflowTotal, 0.0),
				"the dry intermediate invented an unmet transfer demand");
			require(c->netRateAtStart == 1.0
				&& approx(c->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the chain end did not receive the full horizon of through-flow");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the dry-intermediate chain projection does not conserve");
			// Agreement with the live trajectory: 1200 fixed ticks (20 s)
			// of continuous through-flow, then the forecast from that
			// state starts exactly where the live stores are and continues
			// the same line.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain dry live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "A")->currentQualified, 99980.0, 1e-3)
				&& approx(findStore(live.stores, "B")->currentQualified, 0.0, 1e-3)
				&& approx(findStore(live.stores, "C")->currentQualified, 20.0, 1e-3),
				"the live dry-intermediate chain did not flow continuously");
			auto const f2 = live.forecast;
			require(!f2.truncated && f2.segmentsComputed <= 2u,
				"the through-flow chain truncated after the live run");
			require(approx(findForecast(f2.stores, "A")->drawableAtStart, 99980.0, 1e-3)
				&& approx(findForecast(f2.stores, "C")->drawableAtStart, 20.0, 1e-3),
				"the re-forecast start state does not match the live stores");
			require(approx(findForecast(f2.stores, "A")->projectedDrawableAtHorizon,
				13580.0, 1e-3)
				&& approx(findForecast(f2.stores, "C")->projectedDrawableAtHorizon,
					86420.0, 1e-3),
				"the re-forecast did not continue the live through-flow trajectory");
			require(!findForecast(f2.stores, "B")->hasShortfall
				&& approx(findForecast(f2.stores, "B")->unmetOutflowTotal, 0.0),
				"the dry intermediate still invents unmet demand after the live run");
			for (auto const& entry : f2.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the post-run dry-intermediate chain projection does not conserve");
		}
		// Scenario K: the same chain with the intermediate store starting
		// full. The incoming leg must not stop while the outgoing leg
		// drains it: the through-flow is symmetric with the dry case and
		// C again receives the full horizon.
		{
			core::World world("ForecastChainFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 10.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"a full-intermediate through-flow chain truncated the forecast");
			auto const* b = findForecast(snap.forecast.stores, "B");
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(b != nullptr && !b->hasShortfall
				&& approx(b->netRateAtStart, 0.0)
				&& approx(b->unmetOutflowTotal, 0.0)
				&& approx(b->projectedDrawableAtHorizon, 10.0, 1e-3),
				"the full intermediate did not hold steady under symmetric through-flow");
			require(c != nullptr
				&& approx(c->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the full-intermediate chain end did not receive the full horizon");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the full-intermediate chain projection does not conserve");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain full live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "C")->currentQualified, 20.0, 1e-3),
				"the live full-intermediate chain did not deliver the through-flow");
			require(approx(findForecast(live.forecast.stores, "C")->projectedDrawableAtHorizon,
				findStore(live.stores, "C")->currentQualified + 86400.0, 1e-3),
				"the full-intermediate re-forecast did not continue the live trajectory");
			require(!findForecast(live.forecast.stores, "B")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "B")->unmetOutflowTotal, 0.0),
				"the full intermediate invents unmet demand after the live run");
		}
		// Scenario L: projected inventory at a later delivery on a
		// through-flow chain. A drop lands on C at t=40; the forecast
		// taken after the 20 s live run must see C at 20 plus 20 s of
		// continued through-flow (40 kg drawable) at the arrival, accept
		// the whole drop, and read it as a rescue.
		{
			core::World world("ForecastChainDelivery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 0.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			core::ResourceDeliverySettings drop;
			drop.id = "drop"; drop.storeId = "C"; drop.unit = "kg";
			drop.quantity = 100.0; drop.firstOccurrenceSeconds = 40.0;
			settings.deliveries = { drop };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain delivery live run"); // t = 20 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(c != nullptr && c->arrivals.size() == 1,
				"the later delivery is missing from the through-flow chain forecast");
			if (c != nullptr && c->arrivals.size() == 1)
			{
				auto const& arrival = c->arrivals[0];
				require(arrival.deliveryId == "drop"
					&& approx(arrival.arrivalSeconds, 40.0),
					"the later delivery lost its schedule");
				require(approx(arrival.projectedDrawableBefore, 40.0, 1e-3),
					"the projected inventory at the later delivery ignored the through-flow");
				require(approx(arrival.acceptedQuantity, 100.0)
					&& approx(arrival.rejectedQuantity, 0.0)
					&& arrival.rescuesStore,
					"the later delivery was not accepted as a rescue");
			}
			require(approx(c->projectedDrawableAtHorizon, 86520.0, 1e-3),
				"the through-flow chain horizon total did not include the accepted delivery");
			require(std::abs(c->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the through-flow delivery projection does not conserve");
		}
		// Scenario M: generator fuel is projected demand, never
		// sustained inventory (#594). The ticket's minimal generator: a
		// manually commanded 1 kW machine at efficiency 1 burns a
		// 0.001 kWh store dry in 3.6 s. The forecast must report the
		// shortfall with the generator as the limiting service and the
		// post-depletion draw as uncovered demand, and the live run must
		// agree: the fuel really is gone inside the window the forecast
		// named.
		{
			core::World world("ForecastGeneratorFuel", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings fuel;
			fuel.id = "fuel"; fuel.unit = "kWh";
			fuel.capacity = 1.0; fuel.startingQualified = 0.001;
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings generator;
			generator.id = "gen"; generator.fuelStoreId = "fuel";
			generator.thermalClass = "generation"; generator.heatSinkId = "sink";
			generator.ratedOutputKW = 1.0; generator.commandedOutputKW = 1.0;
			generator.efficiency = 1.0; generator.maxRampKWPerSecond = 60.0;
			settings.stores = { fuel };
			settings.thermalReservoirs = { sink };
			settings.generators = { generator };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* fuelEntry = findForecast(snap.forecast.stores, "fuel");
			require(fuelEntry != nullptr,
				"no forecast entry for the generator fuel store");
			if (fuelEntry != nullptr)
			{
				require(approx(fuelEntry->netRateAtStart, -1.0 / 3600.0, 1e-12),
					"the commanded generator fuel draw is not in the projected net rate");
				require(fuelEntry->basis == core::StationForecastBasis::Shortfall
					&& fuelEntry->hasShortfall,
					"a finite fuel store under a commanded generator read as sustained");
				require(approx(fuelEntry->shortfallOnsetSeconds, 3.6, 1e-6),
					"the projected fuel depletion onset is wrong");
				require(fuelEntry->limitingServiceId == "gen"
					&& fuelEntry->limitingServiceKind == "generator",
					"the limiting service is not the generator");
				require(approx(fuelEntry->unmetOutflowTotal,
					(core::StationForecastHorizonSeconds - 3.6) / 3600.0, 1e-6),
					"the fuel demand after depletion is not reported as uncovered");
				require(!fuelEntry->processConsumptionPossible
					&& !fuelEntry->biologyConsumptionPossible,
					"a projected manual generator draw must not also read as unprojected consumption");
				require(std::abs(fuelEntry->conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the generator-fuel projection does not conserve");
			}
			// The live run agrees: the whole 0.001 kWh inventory is gone
			// well inside the window the forecast called short, and the
			// re-forecast from the empty store reads an immediate
			// shortfall rather than sustained supply.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "generator fuel live run"); // t = 20 s
			auto const live = world.getStationSystemsSnapshot();
			auto const* liveFuel = findStore(live.stores, "fuel");
			require(liveFuel != nullptr && liveFuel->currentQualified <= 1e-12,
				"the live run did not consume the fuel the forecast projected away");
			auto const* reEntry = findForecast(live.forecast.stores, "fuel");
			require(reEntry != nullptr && reEntry->hasShortfall
				&& approx(reEntry->drawableAtStart, 0.0)
				&& approx(reEntry->shortfallOnsetSeconds, live.forecast.asOfSeconds),
				"the re-forecast of the empty fuel store is not an immediate shortfall");
			require(live.healthy && world.getStationSystemsDiagnostics().empty(),
				"the generator-fuel forecast scenario produced a Station diagnostic");
		}
		// Scenario N: under Automatic dispatch the generator's draw
		// follows the unmodelled net load, so it is not projected - but
		// the fuel store must carry the unprojected-consumption flag so
		// its Sustained basis is never read as a promise (#594).
		{
			core::World world("ForecastGeneratorAuto", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.dispatchMode = core::StationDispatchMode::Automatic;
			core::ResourceStoreSettings fuel;
			fuel.id = "fuel"; fuel.unit = "kWh";
			fuel.capacity = 1.0; fuel.startingQualified = 0.001;
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings generator;
			generator.id = "gen"; generator.fuelStoreId = "fuel";
			generator.thermalClass = "generation"; generator.heatSinkId = "sink";
			generator.ratedOutputKW = 1.0; generator.commandedOutputKW = 1.0;
			generator.efficiency = 1.0; generator.maxRampKWPerSecond = 60.0;
			settings.stores = { fuel };
			settings.thermalReservoirs = { sink };
			settings.generators = { generator };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* fuelEntry = findForecast(snap.forecast.stores, "fuel");
			require(fuelEntry != nullptr, "no forecast entry for the fuel store");
			if (fuelEntry != nullptr)
			{
				require(approx(fuelEntry->netRateAtStart, 0.0),
					"an Automatic-dispatch generator must not project a fixed fuel draw");
				require(fuelEntry->processConsumptionPossible,
					"the fuel store under Automatic dispatch is not flagged for unprojected consumption");
				require(!fuelEntry->biologyConsumptionPossible,
					"the generator fuel store carries a biology consumption flag");
			}
			require(snap.forecast.uncertaintyLabel.find("processConsumptionPossible")
				!= std::string::npos,
				"the uncertainty label does not explain the unprojected consuming pathways");
		}
		// Scenario O: every deliberately-excluded consuming pathway
		// flags the stores it drains (#594): electrolysis feed water,
		// fuel-cell reactants and consumed maintenance spares carry
		// processConsumptionPossible; pure product destinations and an
		// uncommanded manual generator's fuel store carry neither flag.
		{
			core::World world("ForecastConsumptionFlags", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto plain = [](std::string const& id, double capacity, double starting)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				return settings;
			};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings idleGen;
			idleGen.id = "idleGen"; idleGen.fuelStoreId = "diesel";
			idleGen.thermalClass = "generation"; idleGen.heatSinkId = "genSink";
			idleGen.ratedOutputKW = 1.0; idleGen.commandedOutputKW = 0.0;
			idleGen.efficiency = 0.35; idleGen.maxRampKWPerSecond = 60.0;
			core::ThermalReservoirSettings genSink;
			genSink.id = "genSink"; genSink.thermalClass = "generation";
			genSink.heatCapacityKJPerK = 10000.0; genSink.startingTemperatureC = 20.0;
			genSink.minTemperatureC = 0.0; genSink.maxTemperatureC = 100.0;
			core::ElectrolyzerSettings electrolyzer;
			electrolyzer.id = "elektro"; electrolyzer.hazardClass = "normal";
			electrolyzer.waterStoreId = "feedWater";
			electrolyzer.oxygenStoreId = "o2"; electrolyzer.hydrogenStoreId = "h2";
			electrolyzer.thermalClass = "moderate"; electrolyzer.heatSinkId = "sink";
			electrolyzer.rateKgPerS = 0.001;
			core::FuelCellSettings fuelCell;
			fuelCell.id = "cell"; fuelCell.hazardClass = "normal";
			fuelCell.hydrogenStoreId = "cellH2"; fuelCell.oxygenStoreId = "cellO2";
			fuelCell.waterStoreId = "cellWater";
			fuelCell.thermalClass = "moderate"; fuelCell.heatSinkId = "sink";
			fuelCell.ratedOutputKW = 1.0; fuelCell.commandedOutputKW = 0.0;
			core::MaintenanceJobSettings job;
			job.id = "service"; job.installationId = "idleGen";
			job.firstOccurrenceSeconds = 0.0; job.durationSeconds = 10.0;
			core::MaintenanceRequirementSettings spare;
			spare.storeId = "spares"; spare.quantity = 1.0; spare.consumed = true;
			job.requirements = { spare };
			job.wasteStoreId = "usedParts";
			core::ResourceStoreSettings diesel;
			diesel.id = "diesel"; diesel.unit = "kWh";
			diesel.capacity = 100.0; diesel.startingQualified = 50.0;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				diesel,
				plain("feedWater", 100.0, 50.0),
				plain("o2", 100.0, 1.0), plain("h2", 100.0, 1.0),
				plain("cellH2", 100.0, 10.0), plain("cellO2", 100.0, 10.0),
				plain("cellWater", 100.0, 0.0),
				plain("spares", 10.0, 5.0), plain("usedParts", 10.0, 0.0) };
			settings.thermalReservoirs = { sink, genSink };
			settings.generators = { idleGen };
			settings.electrolyzers = { electrolyzer };
			settings.fuelCells = { fuelCell };
			settings.maintenanceJobs = { job };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const requireProcess = [&](char const* id) {
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->processConsumptionPossible,
					"a store feeding an unprojected consuming pathway is not flagged");
				require(!entry->biologyConsumptionPossible,
					"a process-feed store carries a biology consumption flag"); };
			requireProcess("feedWater");
			requireProcess("cellH2");
			requireProcess("cellO2");
			requireProcess("spares");
			// Product destinations and the uncommanded manual generator's
			// fuel store carry no omitted drain.
			auto const requireClean = [&](char const* id) {
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && !entry->processConsumptionPossible
					&& !entry->biologyConsumptionPossible,
					"a store with no omitted consuming pathway carries a consumption flag"); };
			requireClean("diesel");
			requireClean("o2");
			requireClean("h2");
			requireClean("cellWater");
			requireClean("usedParts");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "consumption-flags live run");
			require(world.getStationSystemsSnapshot().healthy
				&& world.getStationSystemsDiagnostics().empty(),
				"the consumption-flag scenario produced a Station diagnostic");
		}
		// Scenario N: exactly-timely resupply (#595). The store runs dry
		// at the very instant the delivery lands; because arrivals run
		// ahead of the tick's drains, that delivery prevents the t=10
		// shortage and the first projected shortfall is the later one.
		// The arrival must classify as a rescue even though the drawable
		// immediately before it is zero - the old drawable-positive rule
		// called it late while the same forecast put the onset at t=110.
		{
			core::World world("ForecastExactResupply", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1000.0, 10.0),
				chainStores("B", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("AB", "A", "B", 1.0, 1) };
			core::ResourceDeliverySettings exact;
			exact.id = "exact"; exact.storeId = "A"; exact.unit = "kg";
			exact.quantity = 100.0; exact.firstOccurrenceSeconds = 10.0;
			settings.deliveries = { exact };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available && snap.forecast.asOfSeconds == 0.0,
				"the exactly-timely forecast was not read from the paused t=0 state");
			auto const* a = findForecast(snap.forecast.stores, "A");
			require(a != nullptr && a->hasShortfall
				&& a->basis == core::StationForecastBasis::Shortfall,
				"the exactly-timely resupply store did not forecast a shortfall");
			require(approx(a->shortfallOnsetSeconds, 110.0, 1e-5),
				"the exactly-timely delivery did not shift the shortfall onset past itself");
			require(a->arrivals.size() == 1, "the exactly-timely delivery is missing");
			if (a != nullptr && a->arrivals.size() == 1)
			{
				auto const& arrival = a->arrivals[0];
				require(arrival.deliveryId == "exact"
					&& approx(arrival.arrivalSeconds, 10.0),
					"the exactly-timely arrival lost its schedule");
				require(approx(arrival.projectedDrawableBefore, 0.0, 1e-9),
					"the exactly-timely arrival did not see a zero drawable");
				require(approx(arrival.acceptedQuantity, 100.0)
					&& approx(arrival.rejectedQuantity, 0.0),
					"the exactly-timely delivery was not fully accepted");
				require(arrival.arrivesQualified && arrival.rescuesStore,
					"an exactly-timely zero-drawable arrival was not a rescue");
			}
			require(approx(a->unmetOutflowTotal,
				core::StationForecastHorizonSeconds - 110.0, 1e-2),
				"the drain after the exactly-timely rescue is not reported as uncovered");
			require(std::abs(a->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the exactly-timely resupply projection does not conserve");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(b != nullptr
				&& approx(b->projectedDrawableAtHorizon, 110.0, 1e-3),
				"the receiving store did not collect the rescued through-flow total");
		}
		// Scenario O: genuinely late and multiple deliveries after a
		// previous depletion (#595). The store runs dry at t=10; a 100 kg
		// delivery lands at t=20 and a 50 kg delivery at t=60. Neither is
		// a rescue: the t=20 arrival lands after the shortfall with a
		// zero drawable, and the t=60 arrival - despite a positive 60 kg
		// drawable restored by the earlier delivery - still lands after
		// the shortage that already occurred at t=10.
		{
			core::World world("ForecastLateDeliveries", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1000.0, 10.0),
				chainStores("B", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("AB", "A", "B", 1.0, 1) };
			core::ResourceDeliverySettings late;
			late.id = "late"; late.storeId = "A"; late.unit = "kg";
			late.quantity = 100.0; late.firstOccurrenceSeconds = 20.0;
			core::ResourceDeliverySettings later;
			later.id = "later"; later.storeId = "A"; later.unit = "kg";
			later.quantity = 50.0; later.firstOccurrenceSeconds = 60.0;
			settings.deliveries = { late, later };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* a = findForecast(snap.forecast.stores, "A");
			require(a != nullptr && a->hasShortfall,
				"the late-delivery store did not forecast a shortfall");
			require(approx(a->shortfallOnsetSeconds, 10.0, 1e-5),
				"the shortfall onset moved even though every arrival is later");
			require(a->arrivals.size() == 2,
				"the late deliveries are not both in the forecast");
			if (a != nullptr && a->arrivals.size() == 2)
			{
				auto const& first = a->arrivals[0];
				require(first.deliveryId == "late"
					&& approx(first.arrivalSeconds, 20.0)
					&& approx(first.projectedDrawableBefore, 0.0, 1e-9)
					&& approx(first.acceptedQuantity, 100.0)
					&& !first.rescuesStore,
					"the genuinely late t=20 arrival did not read as after-the-shortfall");
				auto const& second = a->arrivals[1];
				require(second.deliveryId == "later"
					&& approx(second.arrivalSeconds, 60.0)
					&& approx(second.projectedDrawableBefore, 60.0, 1e-3)
					&& approx(second.acceptedQuantity, 50.0)
					&& !second.rescuesStore,
					"a delivery after a previous depletion read as a rescue on its positive drawable");
			}
			require(approx(a->unmetOutflowTotal,
				10.0 + (core::StationForecastHorizonSeconds - 170.0), 1e-2),
				"the blocked drain around the late deliveries is not fully reported");
			require(std::abs(a->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the late-delivery projection does not conserve");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(b != nullptr
				&& approx(b->projectedDrawableAtHorizon, 160.0, 1e-3),
				"the receiving store did not collect the late-delivery through-flow total");
		}
		// Scenario P: equipment-to-transfer through-flow at a dry
		// intermediate store (#605), the ticket fixture. A producer
		// delivers 1 kg/s into a dry 1 kg buffer and a transfer drains
		// the buffer at 1 kg/s. The live station flows straight through;
		// the forecast gated the transfer on the inventory at the
		// segment start and the equipment on the destination at the
		// segment start, so the solve alternated fill/drain half-duty
		// phases: it burned the whole segment cap, invented a buffer
		// shortage and delivered only a fraction of the through-flow.
		// The solve now settles every pathway together: no shortage, no
		// truncation, a segment count bounded well below the cap and
		// independent of the horizon, full-horizon throughput at the
		// sink, closed conservation, and agreement with the live
		// trajectory.
		{
			core::World world("ForecastEquipmentThroughDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("buffer", 1.0, 0.0),
				chainStores("sink", 100000.0, 0.0) };
			core::EquipmentLoadSettings producer;
			producer.id = "producer";
			producer.electricalDemandKW = 1.0;
			producer.thermalClass = "warm";
			producer.materialExchanges = {{"buffer", 1.0, false}};
			settings.equipmentLoads = {producer};
			settings.solarArrays = {{"solar", 100.0, {}}};
			settings.transfers = {
				chainTransfer("drain", "buffer", "sink", 1.0, 0) };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available && !snap.forecast.truncated,
				"the equipment-to-transfer through-flow truncated the forecast");
			require(snap.forecast.segmentsComputed <= 2u,
				"a constant equipment through-flow needed more than a couple of segments");
			auto const* buffer = findForecast(snap.forecast.stores, "buffer");
			auto const* sink = findForecast(snap.forecast.stores, "sink");
			require(buffer != nullptr && sink != nullptr,
				"an equipment-through-flow store is missing from the forecast");
			require(buffer != nullptr && !buffer->hasShortfall
				&& approx(buffer->netRateAtStart, 0.0)
				&& approx(buffer->unmetOutflowTotal, 0.0),
				"the equipment-fed dry intermediate invented a shortage");
			require(sink != nullptr
				&& approx(sink->netRateAtStart, 1.0)
				&& approx(sink->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the sink did not receive the full horizon of equipment through-flow");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the equipment-through-flow projection does not conserve");
			// Agreement with the live trajectory: the producer adds
			// 1/60 kg per tick and the transfer moves it out the same
			// tick, leaving the buffer empty.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "equipment through-flow live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "buffer")->currentQualified, 0.0, 1e-9)
				&& approx(findStore(live.stores, "sink")->currentQualified, 20.0, 1e-3),
				"the live equipment-to-transfer chain did not flow straight through");
			auto const f2 = live.forecast;
			require(!f2.truncated && f2.segmentsComputed <= 2u,
				"the equipment through-flow truncated after the live run");
			require(!findForecast(f2.stores, "buffer")->hasShortfall
				&& approx(findForecast(f2.stores, "buffer")->unmetOutflowTotal, 0.0),
				"the equipment-fed intermediate still invents demand after the live run");
			require(approx(findForecast(f2.stores, "sink")->projectedDrawableAtHorizon,
				86420.0, 1e-3),
				"the re-forecast did not continue the equipment through-flow trajectory");
			for (auto const& entry : f2.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the post-run equipment-through-flow projection does not conserve");
		}
		// Scenario Q: the dual boundary - a full intermediate store
		// supplied by a transfer and drained by equipment consumption
		// (#605). The incoming leg must not stop while the equipment
		// drains: the through-flow is symmetric with the dry case and
		// the intermediate holds steady at capacity for the horizon.
		{
			core::World world("ForecastEquipmentThroughFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("source", 1.0e6, 100000.0),
				chainStores("mid", 10.0, 10.0),
				chainStores("product", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("fill", "source", "mid", 1.0, 1) };
			core::EquipmentLoadSettings drainer;
			drainer.id = "drainer";
			drainer.electricalDemandKW = 1.0;
			drainer.thermalClass = "warm";
			drainer.materialExchanges = {
				{"mid", 1.0, true}, {"product", 1.0, false} };
			settings.equipmentLoads = {drainer};
			settings.solarArrays = {{"solar", 100.0, {}}};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"a full-intermediate equipment through-flow chain truncated the forecast");
			auto const* mid = findForecast(snap.forecast.stores, "mid");
			auto const* source = findForecast(snap.forecast.stores, "source");
			auto const* product = findForecast(snap.forecast.stores, "product");
			require(mid != nullptr && !mid->hasShortfall
				&& approx(mid->netRateAtStart, 0.0)
				&& approx(mid->unmetOutflowTotal, 0.0)
				&& approx(mid->projectedDrawableAtHorizon, 10.0, 1e-3),
				"the full intermediate did not hold steady under equipment through-flow");
			require(source != nullptr
				&& approx(source->netRateAtStart, -1.0)
				&& approx(source->projectedDrawableAtHorizon, 13600.0, 1e-3),
				"the transfer source did not project the full-horizon through-flow drain");
			require(product != nullptr
				&& approx(product->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the equipment product store did not receive the full horizon");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the full-intermediate equipment through-flow does not conserve");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "full-intermediate equipment live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "mid")->currentQualified, 10.0, 1e-3)
				&& approx(findStore(live.stores, "product")->currentQualified, 20.0, 1e-3),
				"the live full-intermediate equipment chain did not hold the store steady");
			require(!findForecast(live.forecast.stores, "mid")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "mid")->unmetOutflowTotal, 0.0),
				"the full intermediate invents unmet demand after the live run");
		}
		// Scenario R: shared-feed competition at a dry store (#605).
		// One producer feeds 1 kg/s into a dry store and two equipment
		// loads each want 1 kg/s from it. Only one load can run: the
		// other stops whole and its demand stays uncovered - the
		// forecast must never promise the same kilogram twice.
		{
			core::World world("ForecastSharedFeed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("feed", 1.0, 0.0),
				chainStores("out1", 1.0e6, 0.0),
				chainStores("out2", 1.0e6, 0.0) };
			core::EquipmentLoadSettings supplier;
			supplier.id = "supplier";
			supplier.electricalDemandKW = 1.0;
			supplier.thermalClass = "warm";
			supplier.materialExchanges = {{"feed", 1.0, false}};
			core::EquipmentLoadSettings consumer1;
			consumer1.id = "consumer1";
			consumer1.electricalDemandKW = 1.0;
			consumer1.thermalClass = "warm";
			consumer1.materialExchanges = {
				{"feed", 1.0, true}, {"out1", 1.0, false} };
			core::EquipmentLoadSettings consumer2;
			consumer2.id = "consumer2";
			consumer2.electricalDemandKW = 1.0;
			consumer2.thermalClass = "warm";
			consumer2.materialExchanges = {
				{"feed", 1.0, true}, {"out2", 1.0, false} };
			settings.equipmentLoads = {supplier, consumer1, consumer2};
			settings.solarArrays = {{"solar", 100.0, {}}};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the shared-feed competition truncated the forecast");
			auto const* feed = findForecast(snap.forecast.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 0.0),
				"the over-subscribed dry feed did not read as short from the start");
			require(approx(feed->unmetOutflowTotal,
				core::StationForecastHorizonSeconds, 1e-2),
				"the losing consumer's demand is not fully reported as uncovered");
			// Exactly one consumer runs: one output store collects the
			// full horizon of through-flow and the other stays empty.
			auto const* out1 = findForecast(snap.forecast.stores, "out1");
			auto const* out2 = findForecast(snap.forecast.stores, "out2");
			require(out1 != nullptr && out2 != nullptr, "a consumer output is missing");
			if (out1 != nullptr && out2 != nullptr)
			{
				auto const one = out1->projectedDrawableAtHorizon;
				auto const other = out2->projectedDrawableAtHorizon;
				require((approx(one, 86400.0, 1e-3) && approx(other, 0.0, 1e-9))
					|| (approx(other, 86400.0, 1e-3) && approx(one, 0.0, 1e-9)),
					"the shared feed did not run exactly one consumer at full rate");
			}
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the shared-feed competition does not conserve");
		}
		// Scenario S: crew metabolism through-flow at dry food and
		// water stores (#605). The crew's food and water stores start
		// empty but transfers cover the draws concurrently, so the
		// metabolism runs at full rate for the whole horizon: the
		// waste streams accumulate at the full crew rate and the dry
		// food stores are a covered pass-through, not a shortage.
		{
			core::World world("ForecastCrewThroughFlow", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto const compound = [](std::string const& id, double capacity,
				double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg";
				store.capacity = capacity; store.startingQualified = starting;
				store.composition = std::move(composition);
				return store;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				compound("bulkFood", 1.0e6, 100000.0, foodComposition),
				compound("bulkWater", 1.0e6, 100000.0, waterComposition),
				compound("food", 1.0, 0.0, foodComposition),
				compound("water", 1.0, 0.0, waterComposition),
				compound("urine", 1.0e6, 0.0, urineComposition),
				compound("organicWaste", 1.0e6, 0.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			settings.transfers = {
				chainTransfer("foodFill", "bulkFood", "food", 0.1, 1),
				chainTransfer("waterFill", "bulkWater", "water", 0.2, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the crew through-flow forecast truncated or churned segments");
			auto const* food = findForecast(snap.forecast.stores, "food");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(food != nullptr && !food->hasShortfall
				&& approx(food->netRateAtStart, 0.0)
				&& approx(food->unmetOutflowTotal, 0.0),
				"the covered dry food store invented a crew food shortage");
			require(water != nullptr && !water->hasShortfall
				&& approx(water->netRateAtStart, 0.0)
				&& approx(water->unmetOutflowTotal, 0.0),
				"the covered dry water store invented a crew water shortage");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			auto const* waste = findForecast(snap.forecast.stores, "organicWaste");
			require(urine != nullptr
				&& approx(urine->netRateAtStart, 0.15)
				&& approx(urine->projectedDrawableAtHorizon, 12960.0, 1e-2),
				"the covered crew did not project full-rate urine");
			require(waste != nullptr
				&& approx(waste->netRateAtStart, 0.02)
				&& approx(waste->projectedDrawableAtHorizon, 1728.0, 1e-2),
				"the covered crew did not project full-rate organic waste");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the crew through-flow projection does not conserve");
			// The live station agrees: the crew eats what the transfers
			// deliver each tick and the waste streams accumulate at the
			// full crew rate (minus the first tick the empty food store
			// costs the metabolism).
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "crew through-flow live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "food")->currentQualified, 0.1 / 60.0, 1e-9)
				&& approx(findStore(live.stores, "urine")->currentQualified,
					0.15 * (1199.0 / 60.0), 1e-2),
				"the live crew did not run on the delivered through-flow");
			require(!findForecast(live.forecast.stores, "food")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "food")->unmetOutflowTotal, 0.0),
				"the covered crew invents unmet demand after the live run");
		}
		// Scenario T: a closed equipment cycle between two dry stores
		// cannot bootstrap itself (#605). Load X consumes A and
		// produces B; load Y consumes B and produces A; both stores
		// start empty with no external supply. Neither load is
		// supported: both stop whole, their feed demand stays
		// uncovered, and no phantom material appears anywhere.
		{
			core::World world("ForecastCycleDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 10.0, 0.0),
				chainStores("B", 10.0, 0.0) };
			core::EquipmentLoadSettings x;
			x.id = "x";
			x.thermalClass = "warm";
			x.materialExchanges = {{"A", 1.0, true}, {"B", 1.0, false}};
			core::EquipmentLoadSettings y;
			y.id = "y";
			y.thermalClass = "warm";
			y.materialExchanges = {{"B", 1.0, true}, {"A", 1.0, false}};
			settings.equipmentLoads = {x, y};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the unsupported equipment cycle churned the segment budget");
			auto const* a = findForecast(snap.forecast.stores, "A");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(a != nullptr && a->hasShortfall
				&& approx(a->shortfallOnsetSeconds, 0.0)
				&& approx(a->projectedDrawableAtHorizon, 0.0, 1e-9)
				&& approx(a->unmetOutflowTotal,
					core::StationForecastHorizonSeconds, 1e-2),
				"the unsupported cycle revived store A instead of reporting its demand uncovered");
			require(b != nullptr && b->hasShortfall
				&& approx(b->shortfallOnsetSeconds, 0.0)
				&& approx(b->projectedDrawableAtHorizon, 0.0, 1e-9)
				&& approx(b->unmetOutflowTotal,
					core::StationForecastHorizonSeconds, 1e-2),
				"the unsupported cycle revived store B instead of reporting its demand uncovered");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the unsupported equipment cycle does not conserve");
		}
	}

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerStationOperationsChecks(checks);
	registerStationResilienceChecks(checks);
	checks.insert(checks.end(), {{"trends", trends}, {"endurance", endurance}, {"forecast", forecast}});
	registerStationRecoveryChecks(checks);
	registerStationBiologyChecks(checks);
	registerStationThermalChecks(checks);
	registerStationGenerationChecks(checks);
	registerStationAtmosphereChecks(checks);
	registerStationWaterChecks(checks);
	registerStationCompoundPowerChecks(checks);
	registerStationBatteryChecks(checks);
	registerStationLogisticsChecks(checks);
	registerStationCoreChecks(checks);
	registerStationPortChecks(checks);
	registerStationSeamChecks(checks);
	registerStationPowerChecks(checks);
	registerStationDocumentChecks(checks);
	registerScheduledTransferChecks(checks);
	registerStationConnectionChecks(checks);
	registerBatteryObservationChecks(checks);
	registerObservationIntegrationChecks(checks);
	registerMaterialObservationChecks(checks);
	registerCoupledStationChecks(checks);
	registerReferenceStationChecks(checks);
	registerScenarioChecks(checks);
	registerStabilityChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
