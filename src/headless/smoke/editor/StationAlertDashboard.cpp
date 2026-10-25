#include "Checks.h"
#include "ImGuiContext.h"
#include "TestEngine.h"
#include "StationSystemsWindow.h"
#include "StationAlertDashboard.h"
#include "core/World.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// #870: the Alert dashboard, driven as the operator drives it. These checks
// run the real toolbar button, the real window and the real filter widgets
// against a real run with real fired and closed-out Alerts, and assert the
// lists, their order, the timestamps shown, and what the filters do to them.
//
// The dashboard is a reader. Every state change under test - a Monitor
// recovering, a Monitor being disabled - is authored through the same
// validated World surface a headless client uses, never through the panel,
// so a check that the dashboard had somehow driven the ledger would have to
// fail these assertions.
namespace
{
	using core::AlertClearReason;
	using core::AlertSeverity;

	// Two compartments and four Monitors spanning all three severities, with
	// two different trigger-on times so the ordering rules - severity first,
	// then longest-standing - both have something to bite on. Both
	// compartments sit below the pO2 bound from the start, so the only thing
	// that decides when an Alert appears is its own rule.
	core::StationSystemsSettings dashboardRig()
	{
		core::StationSystemsSettings s;
		s.enabled = true;
		auto compartment = [](std::string const& id, double volumeM3, double oxygenKg) {
			core::AtmosphereSettings a;
			a.id = id; a.volumeM3 = volumeM3; a.thermalCapacityKJPerK = 10000;
			a.startingOxygenKg = oxygenKg; a.startingNitrogenKg = 4 * oxygenKg;
			a.minPressureKPa = 1; a.maxPressureKPa = 200;
			return a;
		};
		s.atmospheres = {compartment("room", 100, 20), compartment("hub", 60, 12)};
		s.monitors = {
			{"room-po2-critical", "atmosphere", "room", "oxygenPartialPressureKPa",
				{17.5, core::AlertDirection::Below, 30, 60}, AlertSeverity::Critical, true},
			{"hub-po2-warning", "atmosphere", "hub", "oxygenPartialPressureKPa",
				{17.5, core::AlertDirection::Below, 30, 60}, AlertSeverity::Warning, true},
			{"room-press-advisory", "atmosphere", "room", "pressureKPa",
				{20.0, core::AlertDirection::Above, 30, 60}, AlertSeverity::Advisory, true},
			{"hub-po2-critical-late", "atmosphere", "hub", "oxygenPartialPressureKPa",
				{17.5, core::AlertDirection::Below, 120, 60}, AlertSeverity::Critical, true},
		};
		return s;
	}

	// Advance the run in chunks until the ledger says what we are waiting
	// for. Chunked rather than frame-by-frame: the waits here are measured in
	// sim-minutes and the view refreshes at 4 Hz regardless, so stepping the
	// ledger in bulk and rendering afterwards is the same observation a good
	// deal sooner.
	bool runUntil(core::World& world, int maxChunks, int ticksPerChunk,
		std::function<bool(core::StationSystemsSnapshot const&)> const& predicate)
	{
		for (int chunk = 0; chunk < maxChunks; ++chunk)
		{
			if (!world.advanceTicks(static_cast<uint64_t>(ticksPerChunk))) return false;
			if (predicate(world.getStationSystemsSnapshot())) return true;
		}
		return predicate(world.getStationSystemsSnapshot());
	}

	std::string joined(std::vector<std::string> const& parts)
	{
		std::string out;
		for (auto const& part : parts) { if (!out.empty()) out += ","; out += part; }
		return out;
	}

	std::string activeOrder(std::vector<StationAlertDashboard::ActiveRow> const& rows)
	{
		std::vector<std::string> parts;
		for (auto const& row : rows) parts.push_back(row.alert.monitorId);
		return joined(parts);
	}
	std::string closedOrder(std::vector<StationAlertDashboard::ClosedRow> const& rows)
	{
		std::vector<std::string> parts;
		for (auto const& row : rows) parts.push_back(row.alert.monitorId);
		return joined(parts);
	}

	StationAlertDashboard::ActiveRow const* findActive(
		StationSystemsWindow const& window, std::string const& monitorId)
	{
		auto const& rows = window.alertDashboardActiveRows();
		auto const found = std::find_if(rows.begin(), rows.end(),
			[&](auto const& row) { return row.alert.monitorId == monitorId; });
		return found == rows.end() ? nullptr : &*found;
	}
	StationAlertDashboard::ClosedRow const* findClosed(
		StationSystemsWindow const& window, std::string const& monitorId)
	{
		auto const& rows = window.alertDashboardClosedRows();
		auto const found = std::find_if(rows.begin(), rows.end(),
			[&](auto const& row) { return row.alert.monitorId == monitorId; });
		return found == rows.end() ? nullptr : &*found;
	}

	// The same rendering the dashboard uses, so an assertion about a shown
	// timestamp is an assertion about the ledger value behind it.
	std::string simText(double seconds)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof buffer, "%.3fs", seconds);
		return std::string(buffer);
	}

	// The run clock is count-derived (steps x step duration), so a boundary
	// lands at 30.000002 rather than 30.0. A tenth of a sim-second is one
	// Station step, so half a step of slack still pins the boundary to the
	// step it happened on.
	bool near(double value, double expected, double tolerance = 0.05)
	{
		return value >= expected - tolerance && value <= expected + tolerance;
	}

	// Let the view's 4 Hz snapshot poll catch up with the ledger. Yielding a
	// fixed number of frames is a guess about how far ImGui's clock actually
	// moved between them; waiting on the refresh itself is the honest version,
	// bounded so that a view which genuinely stopped refreshing fails the check
	// instead of hanging the run.
	bool catchUp(ImGuiTestContext* ctx, StationSystemsWindow const& window,
		size_t active, size_t closed, int maxYields = 300)
	{
		for (int i = 0; i < maxYields; ++i)
		{
			if (window.alertDashboardActiveRows().size() == active
				&& window.alertDashboardClosedRows().size() == closed) return true;
			ctx->Yield(1);
		}
		return window.alertDashboardActiveRows().size() == active
			&& window.alertDashboardClosedRows().size() == closed;
	}

	// #886: drive the real window widget across the rolling cutoff, rather
	// than duplicating the dashboard's predicate in an expected-count helper.
	void alertWindowAdmission()
	{
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationAlertDashboard dashboard;
		uint64_t run = 1;
		core::StationSystemsSnapshot snapshot;
		snapshot.enabled = true;
		snapshot.elapsedSeconds = 300;
		core::StationAlertSnapshot alert;
		alert.monitorId = "critical";
		alert.subjectType = "atmosphere"; alert.subjectId = "room";
		alert.metric = "oxygenPartialPressureKPa";
		alert.severity = AlertSeverity::Critical; alert.firedAtSeconds = 30;
		snapshot.activeAlerts = {alert};
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Admission"); dashboard.render(&snapshot, run); ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "alertWindowAdmission", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Admission");
			driver.click(ctx, "**/Fired within last##AlertWindowEnabled"); ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1, "in-window Alert was not admitted");
			snapshot.elapsedSeconds = 631; ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1,
				"an admitted active Alert disappeared after aging past the cutoff");
			// Severity changes must still hide/show a retained active episode.
			driver.click(ctx, "**/Critical##AlertSeverityFilter"); ctx->Yield(3);
			REQ(dashboard.activeRows().empty(), "severity did not hide a retained Alert");
			driver.click(ctx, "**/Critical##AlertSeverityFilter"); ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1, "severity lost the window admission");
			snapshot.activeAlerts.clear();
			alert.clearedAtSeconds = 631; alert.clearReason = AlertClearReason::AutoClear;
			snapshot.closedAlerts = {alert}; ctx->Yield(3);
			REQ(dashboard.activeRows().empty() && dashboard.closedRows().empty(),
				"closed history was filtered by clear time instead of fire time");
			// Re-firing is a new episode, never inherited from a monitor id.
			alert.clearedAtSeconds.reset(); alert.clearReason.reset();
			alert.firedAtSeconds = 632; snapshot.elapsedSeconds = 650;
			snapshot.activeAlerts = {alert}; ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1, "a new episode was not admitted");
			// Changing the selected window re-evaluates already-old Alerts.
			snapshot.elapsedSeconds = 1300;
			driver.typeValue(ctx, "**/sim-minutes##AlertWindowMinutes", "1.0"); ctx->Yield(3);
			REQ(dashboard.activeRows().empty(), "changing the window retained an already-old Alert");
			// Reset is observable even when the next render already has Alerts.
			snapshot.elapsedSeconds = 20; alert.firedAtSeconds = 10;
			snapshot.activeAlerts = {alert}; ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1, "reset-run Alert was not admitted");
			snapshot.elapsedSeconds = 80; ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1, "reset-run admission was not retained");
			// A reset/load followed by advancement can reuse the same episode
			// identity without showing a backward clock or an empty ledger.
			++run; ctx->Yield(3);
			REQ(dashboard.activeRows().empty(), "admission leaked into a replacement run");
			driver.click(ctx, "**/Fired within last##AlertWindowEnabled"); ctx->Yield(3);
			REQ(dashboard.activeRows().size() == 1, "disabling the window hid an old Alert");
			driver.click(ctx, "**/Fired within last##AlertWindowEnabled"); ctx->Yield(3);
			REQ(dashboard.activeRows().empty(), "selecting a window admitted an already-old Alert");
		});
		#undef REQ
		smoke::require(driver.passed(), "Alert window admission regression failed");
	}

	void alertDashboardWorkflow(smoke::Context const& context)
	{
		alertWindowAdmission();
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto const path = context.temporaryRoot() / "alert-dashboard.station.yaml";
		window.setFilePicker([&](bool) -> std::optional<std::string> { return path.string(); });
		auto world = std::make_shared<core::World>("Alert dashboard", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		require(world->configureStationSystems(dashboardRig(), false, &diagnostic),
			("configure the alert dashboard rig: " + diagnostic).c_str());
		world->resumeSimulation();
		require(runUntil(*world, 400, 30, [](auto const& s) { return s.activeAlerts.size() >= 3; }),
			"the dashboard rig never raised three Alerts");

		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration");
			window.renderFlowContents(world);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "alertDashboard", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			// A live Alert is visible with the window closed, which is the
			// whole point of counting it on the button.
			REQ(driver.contains("Alerts (3)"), "the Alerts button did not count the active Alerts");
			driver.click(ctx, "**/Alerts (3)"); ctx->Yield(3);
			REQ(window.alertDashboardVisible(), "the Alerts button did not open the dashboard");
			ctx->SetRef("Alert dashboard");
			REQ(driver.contains("Active Alerts"), "the dashboard did not open");

			// --- Active list: order and contents -------------------------
			REQ(window.alertDashboardActiveRows().size() == 3,
				"the Active list does not match the ledger count");
			REQ(activeOrder(window.alertDashboardActiveRows())
				== "room-po2-critical,hub-po2-warning,room-press-advisory",
				"the Active list is not ordered severity-first");
			auto const* row = findActive(window, "room-po2-critical");
			REQ(row != nullptr, "the Critical Alert is missing from the Active list");
			auto const shown = *row;
			REQ(shown.alert.subjectType == "atmosphere" && shown.alert.subjectId == "room",
				"the Active row does not name the subject");
			REQ(shown.alert.metric == "oxygenPartialPressureKPa" && shown.alert.threshold == 17.5,
				"the Active row lost the metric or the threshold");
			REQ(shown.unit == "kPa", "the row lost the metric's unit");
			REQ(near(shown.alert.firedAtSeconds, 30.0),
				"the Monitor did not fire at its trigger-on time");
			// The value shown is the live one the Monitor is reading now, not
			// the one that happened to trip the rule.
			REQ(shown.alert.currentValue
					== world->getStationSystemsSnapshot().activeAlerts.front().currentValue
					|| shown.alert.currentValue < shown.alert.threshold,
				"the Active row shows a value that is not the alarming one");
			REQ(driver.contains("atmosphere 'room'"), "the Active row does not show the subject");
			REQ(driver.contains("17.5 kPa"), "the threshold is not shown with its unit");
			REQ(driver.contains(("since " + simText(shown.alert.firedAtSeconds)).c_str()),
				"the Active row does not show the fire time from the ledger");
			// Live duration since fire, measured against the run clock.
			auto const clockNow = world->getStationSystemsSnapshot().elapsedSeconds;
			REQ(shown.liveDurationSeconds == clockNow - shown.alert.firedAtSeconds,
				"the live duration is not measured from the run clock");
			REQ(driver.contains("alarming for"), "the Active row shows no duration since fire");
			REQ(driver.contains("Nothing has closed out yet this run.")
				|| window.alertDashboardClosedRows().empty(),
				"the closed-out section is not empty before anything has closed");

			// --- ordering within one severity: longest-standing first ----
			require(runUntil(*world, 400, 30, [](auto const& s) { return s.activeAlerts.size() >= 4; }),
				"the late Monitor never fired");
			REQ(catchUp(ctx, window, 4, 0), "the dashboard never caught up with the fourth Alert");
			REQ(activeOrder(window.alertDashboardActiveRows())
				== "room-po2-critical,hub-po2-critical-late,hub-po2-warning,room-press-advisory",
				"within one severity the longer-standing Alert does not come first");
			REQ(window.alertSubjectTypes().size() == 1 && window.alertSubjectTypes()[0] == "atmosphere",
				"the subject-type filter does not offer the subject types this run actually has");

			// --- closed by disabling the Monitor -------------------------
			// Disabled first and the auto-clear second, deliberately: the
			// close-out ordering assertion below only has teeth if the later
			// close-out is the alphabetically later id, or the monitor-id
			// tie-break would produce the same list by accident.
			auto disabled = world->getStationSystemsSettings();
			for (auto& monitor : disabled.monitors)
				if (monitor.id == "hub-po2-warning") monitor.enabled = false;
			REQ(world->updateStationSystemsSettings(disabled, &diagnostic),
				("disabling the Monitor was refused: " + diagnostic).c_str());
			require(runUntil(*world, 600, 30,
				[](auto const& s) { return !s.closedAlerts.empty(); }),
				"disabling the Monitor never closed its Alert");
			REQ(catchUp(ctx, window, 3, 1),
				"the dashboard never caught up with the disabled close");
			auto const* stopped = findClosed(window, "hub-po2-warning");
			REQ(stopped != nullptr
				&& stopped->alert.clearReason == AlertClearReason::MonitorDisabled,
				"disabling a Monitor did not close its Alert as MonitorDisabled");
			REQ(driver.contains("reason MonitorDisabled"),
				"the dashboard cannot tell an auto-clear from a disabled-Monitor close");

			// --- auto-clear ---------------------------------------------
			// The recovery is an authored settings change through the World,
			// not something the dashboard did.
			auto recovered = world->getStationSystemsSettings();
			for (auto& atmosphere : recovered.atmospheres)
				if (atmosphere.id == "room") atmosphere.volumeM3 = 50;
			REQ(world->updateStationSystemsSettings(recovered, &diagnostic),
				("recovering the compartment was refused: " + diagnostic).c_str());
			require(runUntil(*world, 900, 30, [](auto const& s) {
				return std::any_of(s.closedAlerts.begin(), s.closedAlerts.end(),
					[](auto const& alert) { return alert.monitorId == "room-po2-critical"; });
			}), "the recovered Monitor never auto-cleared");
			REQ(catchUp(ctx, window, 2, 2), "the dashboard never caught up with the auto-clear");
			auto const* cleared = findClosed(window, "room-po2-critical");
			REQ(cleared != nullptr, "the recovered Alert did not reach the closed-out list");
			auto const clearedCopy = *cleared;
			REQ(clearedCopy.alert.clearReason == AlertClearReason::AutoClear,
				"the recovered Alert did not close as an auto-clear");
			// The recovery was authored at roughly 120 s, so the clear lands
			// one trigger-off window after that rather than at a fixed clock
			// value. What is pinned is the shape the operator reads: it fired at
			// its on-boundary, it cleared no sooner than its off-boundary
			// allowed, and both times are shown.
			REQ(near(clearedCopy.alert.firedAtSeconds, 30.0)
				&& clearedCopy.alert.clearedAtSeconds.has_value()
				&& *clearedCopy.alert.clearedAtSeconds - clearedCopy.alert.firedAtSeconds >= 60.0,
				"the closed-out row does not show the fired/cleared pair from the ledger");
			REQ(driver.contains("Closed out this run"), "the closed-out section is missing");
			REQ(driver.contains("reason AutoClear"), "the clear reason is not shown");
			REQ(driver.contains(("fired " + simText(clearedCopy.alert.firedAtSeconds)).c_str())
				&& driver.contains(("cleared " + simText(*clearedCopy.alert.clearedAtSeconds)).c_str()),
				"the closed-out row does not show its fired and cleared times");
			REQ(findActive(window, "room-po2-critical") == nullptr,
				"the closed-out Alert is still listed as active");
			// Newest close-out first: the thing that just happened is the thing
			// worth reading, and the closed list only grows from the other end.
			REQ(closedOrder(window.alertDashboardClosedRows())
				== "room-po2-critical,hub-po2-warning",
				"the closed-out list is not ordered newest-close-out-first");

			// --- filters -------------------------------------------------
			// These checks select filters without advancing time; episode aging
			// and retained admission are covered by alertWindowAdmission above.
			auto expectedCounts = [&](size_t& active, size_t& closed) {
				auto const snap = world->getStationSystemsSnapshot();
				// Disabled means no window at all: a cutoff far enough back
				// that nothing was fired before it.
				auto const cutoff = snap.elapsedSeconds
					- (window.alertWindowEnabled() ? window.alertWindowMinutes() * 60.0 : 1e30);
				active = closed = 0;
				auto const keep = [&](core::StationAlertSnapshot const& alert) {
					if (!window.alertSeverityVisible(alert.severity)) return false;
					if (!window.alertSubjectTypeFilter().empty()
						&& alert.subjectType != window.alertSubjectTypeFilter()) return false;
					return alert.firedAtSeconds >= cutoff;
				};
				for (auto const& alert : snap.activeAlerts) if (keep(alert)) ++active;
				for (auto const& alert : snap.closedAlerts) if (keep(alert)) ++closed;
			};
			auto checkFilters = [&](char const* what) {
				size_t active = 0, closed = 0;
				expectedCounts(active, closed);
				REQ(window.alertDashboardActiveRows().size() == active
					&& window.alertDashboardClosedRows().size() == closed,
					(std::string("the ") + what + " did not narrow the lists as the ledger says").c_str());
			};

			auto const ledgerBeforeFilters = world->getStationSystemsSnapshot();
			auto const draftBeforeFilters = window.flowModified();

			// Severity.
			driver.click(ctx, "**/Critical##AlertSeverityFilter"); ctx->Yield(3);
			REQ(!window.alertSeverityVisible(AlertSeverity::Critical),
				"the Critical checkbox did not take");
			checkFilters("severity filter");
			REQ(std::none_of(window.alertDashboardActiveRows().begin(),
				window.alertDashboardActiveRows().end(), [](auto const& row) {
					return row.alert.severity == AlertSeverity::Critical;
				}), "the severity filter still lists Critical Alerts");
			REQ(!window.alertDashboardActiveRows().empty(),
				"the severity filter hid everything, not just Critical");
			driver.click(ctx, "**/Critical##AlertSeverityFilter"); ctx->Yield(3);
			REQ(window.alertSeverityVisible(AlertSeverity::Critical),
				"the severity filter could not be turned back on");
			checkFilters("severity filter restored");

			// Subject type.
			driver.click(ctx, "**/atmosphere##AlertSubjectType"); ctx->Yield(3);
			REQ(window.alertSubjectTypeFilter() == "atmosphere",
				"the subject-type control did not take");
			checkFilters("subject-type filter");
			// Only the atmosphere compartment is monitorable today, so this
			// cannot discriminate the predicate by count - every row is a
			// candidate either way. What it does pin is that the control drives
			// the model and that nothing shown violates the pick. The
			// discriminating case arrives with a second subject type.
			for (auto const& row : window.alertDashboardActiveRows())
				REQ(row.alert.subjectType == window.alertSubjectTypeFilter(),
					"a row the subject-type filter excludes is still shown");
			driver.click(ctx, "**/All##AlertSubjectType"); ctx->Yield(3);
			REQ(window.alertSubjectTypeFilter().empty(), "the subject-type filter could not be cleared");
			checkFilters("subject-type filter cleared");

			// Time window: only Alerts that fired inside it stay listed.
			driver.click(ctx, "**/Fired within last##AlertWindowEnabled"); ctx->Yield(2);
			REQ(window.alertWindowEnabled(), "the window checkbox did not take");
			auto const beforeWindow = window.alertDashboardActiveRows().size();
			driver.typeValue(ctx, "**/sim-minutes##AlertWindowMinutes", "1.0"); ctx->Yield(3);
			REQ(window.alertWindowMinutes() == 1.0, "the window length did not take");
			checkFilters("time-window filter");
			REQ(window.alertDashboardActiveRows().size() < beforeWindow,
				"the time-window filter narrowed nothing");
			{
				auto const snap = world->getStationSystemsSnapshot();
				auto const cutoff = snap.elapsedSeconds - window.alertWindowMinutes() * 60.0;
				for (auto const& row : window.alertDashboardActiveRows())
					REQ(row.alert.firedAtSeconds >= cutoff,
						"the window shows an Alert that fired before it opened");
			}

			// Filtering is presentation only. The ledger behind it is exactly
			// what it was, and the draft never heard about any of it.
			auto const ledgerAfterFilters = world->getStationSystemsSnapshot();
			REQ(ledgerAfterFilters.activeAlerts == ledgerBeforeFilters.activeAlerts
				&& ledgerAfterFilters.closedAlerts == ledgerBeforeFilters.closedAlerts,
				"a filter changed the Alert ledger");
			REQ(window.flowModified() == draftBeforeFilters,
				"a filter touched the authored draft");

			// And with nothing at all on screen, the run keeps recording.
			driver.click(ctx, "**/Critical##AlertSeverityFilter"); ctx->Yield(2);
			driver.click(ctx, "**/Warning##AlertSeverityFilter"); ctx->Yield(2);
			driver.click(ctx, "**/Advisory##AlertSeverityFilter"); ctx->Yield(2);
			REQ(window.alertDashboardActiveRows().empty()
				&& window.alertDashboardClosedRows().empty(),
				"the filters did not hide every row");
			auto blind = world->getStationSystemsSettings();
			for (auto& monitor : blind.monitors)
				if (monitor.id == "room-press-advisory") monitor.enabled = false;
			REQ(world->updateStationSystemsSettings(blind, &diagnostic),
				("disabling the last Monitor was refused: " + diagnostic).c_str());
			// The ledger records it even though the dashboard is showing
			// nothing: give it the chance to, so this cannot pass by accident.
			for (int i = 0; i < 30; ++i) ctx->Yield(1);
			auto const blindLedger = world->getStationSystemsSnapshot();
			REQ(std::any_of(blindLedger.closedAlerts.begin(), blindLedger.closedAlerts.end(),
				[](auto const& alert) {
					return alert.monitorId == "room-press-advisory"
						&& alert.clearReason == AlertClearReason::MonitorDisabled;
				}), "a clear that happened while nothing was on screen was not recorded");
			REQ(window.alertDashboardClosedRows().empty(),
				"a filtered-out Alert reached the dashboard anyway");

			// --- run-scoped: a reset clears it ---------------------------
			// Put the subject-type pick back where it can go stale, so the
			// self-heal below has something to heal.
			driver.click(ctx, "**/atmosphere##AlertSubjectType"); ctx->Yield(3);
			REQ(window.alertSubjectTypeFilter() == "atmosphere",
				"the subject-type pick could not be restored before the reset");
			world->resetSimulation();
			for (int i = 0; i < 30; ++i) ctx->Yield(1);
			auto const reset = world->getStationSystemsSnapshot();
			REQ(reset.activeAlerts.empty() && reset.closedAlerts.empty(),
				"the reset left Alert ledger entries behind");
			REQ(window.alertDashboardActiveRows().empty()
				&& window.alertDashboardClosedRows().empty(),
				"the dashboard carried rows over from the reset run");
			// The stale subject-type pick has nothing to point at any more,
			// and an empty dashboard must not read as a quiet station.
			REQ(window.alertSubjectTypeFilter().empty(),
				"a subject-type filter survived a run with no such subject");
			REQ(driver.contains("Nothing is alarming right now."),
				"the dashboard did not say the run is quiet after the reset");
		});
		#undef REQ
		require(driver.passed(), "the Alert dashboard failed on the Test Engine");
	}

	// The dashboard's other honest state: no scenario running. An empty
	// Active list is a claim about the station; this is a claim about there
	// being no station to make claims about.
	void alertDashboardNoScenario(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("No scenario", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		world->resumeSimulation();
		world->advanceTicks(120);
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration");
			window.renderFlowContents(world);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "alertDashboardNoScenario", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			REQ(driver.contains("Alerts"), "the Alerts control is missing entirely");
			driver.click(ctx, "**/Alerts"); ctx->Yield(3);
			REQ(window.alertDashboardVisible(), "the Alerts control did not open the dashboard");
			ctx->SetRef("Alert dashboard");
			REQ(driver.contains("No Station scenario is running"),
				"the dashboard did not say there is no scenario to alarm on");
			REQ(window.alertDashboardActiveRows().empty()
				&& window.alertDashboardClosedRows().empty(),
				"the dashboard listed rows with no scenario running");
		});
		#undef REQ
		require(driver.passed(), "the Alert dashboard's no-scenario state failed on the Test Engine");
	}
}

namespace editor_smoke
{
	void registerStationAlertDashboard(std::vector<smoke::Check>& checks)
	{
		checks.push_back({"station/alertDashboardWorkflow", alertDashboardWorkflow});
		checks.push_back({"station/alertDashboardNoScenario", alertDashboardNoScenario});
	}
}
