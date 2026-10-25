#include "StationAlertDashboard.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <format>

namespace
{
	// Resolve units from the same catalogue validation and evaluation use.
	std::string metricUnit(std::string_view subjectType, std::string_view metric)
	{
		for (auto const& entry : core::stationMonitorMetricCatalogue(subjectType))
			if (entry.name == metric) return std::string(entry.unit);
		return {};
	}

	// The same shortest round-trip spelling the #868 announcement puts on the
	// event stream, so a number read off the dashboard matches the one read
	// off the log and the two never look like different measurements.
	std::string number(double value) { return std::format("{}", value); }

	// Severity first: the condition that could kill the crew goes above the
	// one worth a glance. Longest-standing breaks the tie, because within one
	// severity the operator's next question is how long they have already
	// been living with it. The monitor id settles any remaining tie, so two
	// Alerts fired on the same step still have exactly one defined order.
	bool activePrecedes(core::StationAlertSnapshot const& a, core::StationAlertSnapshot const& b)
	{
		if (a.severity != b.severity)
			return static_cast<int>(a.severity) > static_cast<int>(b.severity);
		if (a.firedAtSeconds != b.firedAtSeconds) return a.firedAtSeconds < b.firedAtSeconds;
		return a.monitorId < b.monitorId;
	}

	// Newest close-out first, matching the fault panel's transitions: the
	// thing that just happened is the thing worth reading, and the closed
	// list only ever grows from the other end.
	bool closedPrecedes(core::StationAlertSnapshot const& a, core::StationAlertSnapshot const& b)
	{
		auto const at = a.clearedAtSeconds.value_or(0.0);
		auto const bt = b.clearedAtSeconds.value_or(0.0);
		if (at != bt) return at > bt;
		return a.monitorId < b.monitorId;
	}

	std::string formatDuration(double seconds)
	{
		if (!(seconds > 0.0)) seconds = 0.0;
		auto const whole = static_cast<long long>(seconds + 0.5);
		if (whole < 60) return std::to_string(whole) + "s";
		if (whole < 3600)
			return std::to_string(whole / 60) + "m " + std::to_string(whole % 60) + "s";
		return std::to_string(whole / 3600) + "h " + std::to_string((whole % 3600) / 60) + "m";
	}

	std::string formatSimTime(double seconds)
	{
		return std::format("{:.3f}s", seconds);
	}

	ImVec4 severityColour(core::AlertSeverity severity)
	{
		switch (severity)
		{
		case core::AlertSeverity::Critical: return {1.0f, 0.36f, 0.3f, 1.0f};
		case core::AlertSeverity::Warning: return {1.0f, 0.76f, 0.35f, 1.0f};
		case core::AlertSeverity::Advisory: return {0.62f, 0.78f, 0.95f, 1.0f};
		}
		return {0.75f, 0.8f, 0.88f, 1.0f};
	}

	std::string withUnit(std::string const& value, std::string const& unit)
	{
		return unit.empty() ? value : value + " " + unit;
	}
}

bool StationAlertDashboard::passes(core::StationAlertSnapshot const& alert, double now) const
{
	if (!severityVisible(alert.severity)) return false;
	if (!mSubjectTypeFilter.empty() && alert.subjectType != mSubjectTypeFilter) return false;
	// Measured from fire time, not clear time: the question the operator asks
	// of a window is "what started recently". An Alert that started inside
	// the window stays listed for as long as it runs, even past the edge of
	// the window, because dropping it mid-crisis would hide the one thing the
	// window was narrowed to find.
	if (mWindowEnabled && alert.firedAtSeconds < now - mWindowMinutes * 60.0) return false;
	return true;
}

void StationAlertDashboard::render(core::StationSystemsSnapshot const* snapshot)
{
	mActiveRows.clear();
	mClosedRows.clear();
	mSubjectTypes.clear();
	if (!snapshot || !snapshot->enabled)
	{
		// "Nothing is alarming" and "nothing is running" are different
		// claims. Showing an empty Active list for a stopped scenario would
		// say the first when the truth is the second.
		ImGui::TextDisabled("No Station scenario is running, so there are no Alerts to show.");
		ImGui::TextDisabled("Start Station systems, or Apply the draft, to watch Monitors live.");
		return;
	}
	double const now = snapshot->elapsedSeconds;

	// The offered subject types come from the ledger itself, so the combo
	// never lists a type the operator cannot filter to.
	for (auto const* ledger : {&snapshot->activeAlerts, &snapshot->closedAlerts})
		for (auto const& alert : *ledger)
			if (std::find(mSubjectTypes.begin(), mSubjectTypes.end(), alert.subjectType)
				== mSubjectTypes.end())
				mSubjectTypes.push_back(alert.subjectType);
	std::sort(mSubjectTypes.begin(), mSubjectTypes.end());
	// A pick left pointing at a type this run does not have would show an
	// empty dashboard that reads like a quiet station rather than a stale
	// filter. Drop it rather than mislead.
	if (!mSubjectTypeFilter.empty()
		&& std::find(mSubjectTypes.begin(), mSubjectTypes.end(), mSubjectTypeFilter)
			== mSubjectTypes.end())
		mSubjectTypeFilter.clear();

	for (auto const& alert : snapshot->activeAlerts)
		if (passes(alert, now))
			mActiveRows.push_back({alert, now - alert.firedAtSeconds,
				metricUnit(alert.subjectType, alert.metric)});
	for (auto const& alert : snapshot->closedAlerts)
		if (passes(alert, now))
			mClosedRows.push_back({alert, metricUnit(alert.subjectType, alert.metric)});
	std::stable_sort(mActiveRows.begin(), mActiveRows.end(), [](auto const& a, auto const& b) {
		return activePrecedes(a.alert, b.alert);
	});
	std::stable_sort(mClosedRows.begin(), mClosedRows.end(), [](auto const& a, auto const& b) {
		return closedPrecedes(a.alert, b.alert);
	});

	ImGui::SeparatorText("Filters");
	ImGui::TextUnformatted("Severity");
	ImGui::SameLine();
	for (auto const severity : {core::AlertSeverity::Critical, core::AlertSeverity::Warning,
		core::AlertSeverity::Advisory})
	{
		// The ## suffix keeps each checkbox addressable on its own: the row text
		// beside it says "Critical" too, and a filter that could not be told
		// apart from the thing it filters would be undrivable.
		auto const label = std::string(core::stationAlertSeverityTag(severity))
			+ "##AlertSeverityFilter";
		auto& slot = mSeverityFilter[static_cast<size_t>(severity)];
		ImGui::Checkbox(label.c_str(), &slot);
		ImGui::SameLine();
	}
	// Radio buttons rather than a combo: the offered set is small, derived from
	// the ledger, and every choice should be visible at once so the operator
	// can see at a glance which subject types this run actually has.
	ImGui::SameLine();
	ImGui::TextUnformatted("Subject type");
	ImGui::SameLine();
	if (ImGui::RadioButton("All##AlertSubjectType", mSubjectTypeFilter.empty()))
		mSubjectTypeFilter.clear();
	for (auto const& type : mSubjectTypes)
	{
		ImGui::SameLine();
		auto const label = type + "##AlertSubjectType";
		if (ImGui::RadioButton(label.c_str(), mSubjectTypeFilter == type)) mSubjectTypeFilter = type;
	}
	ImGui::SameLine();
	ImGui::Checkbox("Fired within last##AlertWindowEnabled", &mWindowEnabled);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputDouble("sim-minutes##AlertWindowMinutes", &mWindowMinutes, 1.0, 10.0, "%.1f");
	ImGui::SameLine();
	ImGui::Text("(of %zu active, %zu closed out)", snapshot->activeAlerts.size(),
		snapshot->closedAlerts.size());

	ImGui::SeparatorText("Active Alerts");
	if (mActiveRows.empty())
		ImGui::TextDisabled("Nothing is alarming right now.");
	for (size_t index = 0; index < mActiveRows.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		auto const& row = mActiveRows[index];
		ImGui::TextColored(severityColour(row.alert.severity), "%s  %s '%s'",
			std::string(core::stationAlertSeverityTag(row.alert.severity)).c_str(),
			row.alert.subjectType.c_str(), row.alert.subjectId.c_str());
		ImGui::Text("  %s  value %s  threshold %s  (%s)", row.alert.metric.c_str(),
			withUnit(number(row.alert.currentValue), row.unit).c_str(),
			withUnit(number(row.alert.threshold), row.unit).c_str(),
			std::string(core::stationMonitorProvenanceTag(row.alert.provenance)).c_str());
		ImGui::Text("  monitor '%s'  alarming for %s since %s", row.alert.monitorId.c_str(),
			formatDuration(row.liveDurationSeconds).c_str(),
			formatSimTime(row.alert.firedAtSeconds).c_str());
		ImGui::PopID();
	}

	ImGui::SeparatorText("Closed out this run");
	if (mClosedRows.empty())
		ImGui::TextDisabled("Nothing has closed out yet this run.");
	for (size_t index = 0; index < mClosedRows.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		auto const& row = mClosedRows[index];
		// A closed-out Alert keeps the severity it fired at: the record says
		// how bad it was, not how calm things are now.
		ImGui::TextColored(severityColour(row.alert.severity), "%s  %s '%s'  %s",
			std::string(core::stationAlertSeverityTag(row.alert.severity)).c_str(),
			row.alert.subjectType.c_str(), row.alert.subjectId.c_str(), row.alert.metric.c_str());
		ImGui::Text("  monitor '%s'  fired %s  cleared %s  reason %s",
			row.alert.monitorId.c_str(), formatSimTime(row.alert.firedAtSeconds).c_str(),
			formatSimTime(row.alert.clearedAtSeconds.value_or(0.0)).c_str(),
			row.alert.clearReason
				? std::string(core::stationAlertClearReasonTag(*row.alert.clearReason)).c_str()
				: "Unknown");
		ImGui::Text("  value at clear %s  threshold %s",
			withUnit(number(row.alert.currentValue), row.unit).c_str(),
			withUnit(number(row.alert.threshold), row.unit).c_str());
		ImGui::PopID();
	}
}
