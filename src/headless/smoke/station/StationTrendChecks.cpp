#include "StationTrendChecks.h"
#include "StationRig.h"
#include "core/AgentTagRegistryDocument.h"
#include <algorithm>
#include <cstdio>

namespace
{
	using namespace smoke::station;

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
			auto stale = replaceAll(text, "version: 100", "version: 94");
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

}

void registerStationTrendChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), { { "trends", trends }, { "endurance", endurance } });
}
