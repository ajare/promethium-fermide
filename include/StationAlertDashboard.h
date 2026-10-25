#pragma once
#include "core/StationSystems.h"

#include <array>
#include <string>
#include <vector>

// #870 (spec §6): the Alert dashboard. A read-only surface over the
// snapshot's Alert ledger - what is alarming right now, and what has closed
// out this run. It owns no Alert state and issues no commands: every row is
// re-read from the authoritative snapshot each frame, so an Alert that fired
// while the dashboard was closed appears on the next frame, and one that
// auto-cleared leaves without the dashboard being told anything.
//
// The filters are presentation only. They narrow what the operator sees and
// never reach the runtime, so no filter setting can change what fires, what
// clears, or what is recorded. That is what makes a dashboard like this safe
// to bolt on: it is a reader, and the ledger behind it is the same one the
// event stream and the fault surfaces already agree on.
//
// This is a separate surface from the #849 alarm panel. That one explains
// broken machines by derived impact; this one reports monitored conditions by
// authored severity. Neither is a filter over the other.
class StationAlertDashboard
{
public:
	// One Active Alert as the operator reads it. `liveDurationSeconds` is
	// measured against the snapshot's own clock rather than wall time, so
	// the number means the same thing paused or running and cannot drift with
	// the frame rate.
	struct ActiveRow
	{
		core::StationAlertSnapshot alert;
		double liveDurationSeconds{0};
		std::string unit;
	};
	// One closed-out Alert. The fired/cleared pair places it in the run's
	// timeline; the clear reason is the difference between a condition that
	// recovered and one the operator stopped watching.
	struct ClosedRow
	{
		core::StationAlertSnapshot alert;
		std::string unit;
	};

	// Renders inside the caller's window region. A null or disabled snapshot
	// is "nothing is running", which is a different claim from "nothing is
	// alarming" and is said differently.
	void render(core::StationSystemsSnapshot const* snapshot);

	// The rows as currently filtered and ordered - what the operator sees,
	// not the ledger behind them.
	std::vector<ActiveRow> const& activeRows() const { return mActiveRows; }
	std::vector<ClosedRow> const& closedRows() const { return mClosedRows; }

	// Filter state, kept as plain values so a headless check can assert that
	// the widget drove the model rather than only repainting something.
	bool severityVisible(core::AlertSeverity severity) const
	{ return mSeverityFilter[static_cast<size_t>(severity)]; }
	void setSeverityVisible(core::AlertSeverity severity, bool visible)
	{ mSeverityFilter[static_cast<size_t>(severity)] = visible; }
	// Empty means every subject type.
	std::string const& subjectTypeFilter() const { return mSubjectTypeFilter; }
	void setSubjectTypeFilter(std::string type) { mSubjectTypeFilter = std::move(type); }
	// The window narrows by fire time: "what started recently". A long-running
	// Alert that started inside the window stays listed until it closes.
	bool windowEnabled() const { return mWindowEnabled; }
	double windowMinutes() const { return mWindowMinutes; }
	// The subject types the current ledger actually offers, sorted. The combo
	// never lists a type the operator cannot filter to.
	std::vector<std::string> const& subjectTypes() const { return mSubjectTypes; }

private:
	// Indexed by AlertSeverity: Advisory, Warning, Critical.
	std::array<bool, 3> mSeverityFilter{true, true, true};
	std::string mSubjectTypeFilter;
	bool mWindowEnabled{false};
	double mWindowMinutes{10.0};
	std::vector<std::string> mSubjectTypes;
	std::vector<ActiveRow> mActiveRows;
	std::vector<ClosedRow> mClosedRows;

	bool passes(core::StationAlertSnapshot const& alert, double now) const;
};
