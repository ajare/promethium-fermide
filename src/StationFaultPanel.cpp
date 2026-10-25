#include "StationFaultPanel.h"

#include "core/World.h"
#include "imgui/imgui.h"

#include <algorithm>

namespace
{
	// The event categories the fault framework writes. The panel filters on
	// the category and never parses the message to decide what kind of
	// transition it announced - the category already says (#418).
	bool isFaultTransition(std::string const& category)
	{
		return category == "fault" || category == "trip" || category == "repair";
	}
	// Presentation only. The number on screen is always the derived impact;
	// this only decides how hot to draw it.
	bool criticalImpact(double impact) { return impact >= 0.5; }
	// How many transitions are worth showing before the list needs scrolling.
	inline constexpr size_t MaxDisplayedTransitions{12};

	ImVec4 impactColour(double impact, bool latched)
	{
		if (latched || criticalImpact(impact)) return {1.0f, 0.36f, 0.3f, 1.0f};
		if (impact > 0.0) return {1.0f, 0.76f, 0.35f, 1.0f};
		return {0.75f, 0.8f, 0.88f, 1.0f};
	}
}

void StationFaultPanel::render(core::World& world,
	core::StationSystemsSnapshot const* snapshot)
{
	mRows.clear();
	mTransitions.clear();
	ImGui::SeparatorText("Active alarms");
	if (!snapshot || !snapshot->enabled)
	{
		// "Nothing is broken" and "nothing is running" are different claims;
		// showing an empty alarm list for a stopped scenario would be the
		// first of them when the truth is the second.
		ImGui::TextDisabled("No Station scenario is running, so there is nothing to alarm on.");
		ImGui::TextDisabled("Start Station systems, or Apply the draft, to watch faults live.");
		return;
	}
	auto const settings = world.getStationSystemsSettings();
	for (auto const& fault : station_flow::collectFaults(*snapshot))
	{
		Row row;
		row.fault = fault;
		// The free fix is legal on a latched Trip and on nothing else; the
		// core refuses it anywhere else, and the panel does not offer what
		// the core would turn down.
		row.resettable = fault.latched;
		for (auto const& plan : core::stationRepairPlans(settings, fault.installationId))
			if (plan.modeId == fault.modeId) { row.repairable = true; break; }
		auto const remembered = mRepairJobs.find(fault.installationId + "\x1f" + fault.modeId);
		if (remembered != mRepairJobs.end()) row.repairJobId = remembered->second;
		mRows.push_back(std::move(row));
	}
	// A repair that finished, or a Fault that cleared some other way, takes
	// its remembered job with it: the panel keeps no history of its own.
	for (auto it = mRepairJobs.begin(); it != mRepairJobs.end(); )
	{
		auto const key = it->first;
		auto const stillListed = std::any_of(mRows.begin(), mRows.end(),
			[&](Row const& row) {
				return row.fault.installationId + "\x1f" + row.fault.modeId == key;
			});
		if (stillListed) ++it;
		else it = mRepairJobs.erase(it);
	}
	if (mRows.empty())
		ImGui::TextDisabled("No active Faults - every Installation is holding its rated service.");
	ImGui::PushID("station-fault-alarms");
	for (size_t index = 0; index < mRows.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		auto const& fault = mRows[index].fault;
		ImGui::TextColored(impactColour(fault.derivedImpact, fault.latched), "%s '%s'",
			fault.nodeType.c_str(), fault.installationId.c_str());
		ImGui::SameLine();
		ImGui::Text("%s%s", fault.modeId.c_str(), fault.latched ? " (latched Trip)" : "");
		ImGui::Text("  impact %s%% since tick %llu - %s",
			station_flow::formatTelemetryNumber(fault.derivedImpact * 100.0).c_str(),
			static_cast<unsigned long long>(fault.onsetTick),
			station_flow::faultCauseName(fault.causeKind));
		if (mRows[index].resettable)
		{
			if (ImGui::Button("Reset trip"))
			{
				std::string diagnostic;
				if (world.resetStationTrip(fault.installationId, &diagnostic)) mNotice.clear();
				else mNotice = diagnostic;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("free, instant; an unresolved trigger may latch again");
		}
		if (mRows[index].repairable)
		{
			if (ImGui::Button("Repair"))
			{
				std::string jobId;
				std::string diagnostic;
				if (world.repairStationFault(fault.installationId, fault.modeId, &jobId, &diagnostic))
				{
					mNotice.clear();
					mRepairJobs[fault.installationId + "\x1f" + fault.modeId] = jobId;
					mRows[index].repairJobId = jobId;
				}
				else mNotice = diagnostic;
			}
			if (!mRows[index].repairJobId.empty())
			{
				ImGui::SameLine();
				ImGui::Text("job %s", mRows[index].repairJobId.c_str());
			}
		}
		else
		{
			ImGui::SameLine();
			ImGui::TextDisabled("no repair plan authored for this mode");
		}
		ImGui::Separator();
	}
	ImGui::PopID();
	if (!mNotice.empty())
		ImGui::TextWrapped("Refused: %s", mNotice.c_str());
	ImGui::SeparatorText("Fault, trip and repair transitions");
	auto const events = world.getStationSystemsEvents();
	for (auto const& event : events)
		if (isFaultTransition(event.category))
			mTransitions.push_back("tick " + std::to_string(event.worldTick) + " ["
				+ event.category + "] " + event.message);
	if (mTransitions.empty())
		ImGui::TextDisabled("No fault, trip or repair transition recorded yet.");
	else
	{
		// Newest first: the thing that just happened is the thing worth
		// reading, and the bounded log keeps growing from the other end.
		size_t const shown = std::min(MaxDisplayedTransitions, mTransitions.size());
		for (size_t offset = 0; offset < shown; ++offset)
		{
			auto const& line = mTransitions[mTransitions.size() - 1 - offset];
			ImGui::TextWrapped("%s", line.c_str());
		}
		if (mTransitions.size() > shown)
			ImGui::TextDisabled("(%zu older transitions omitted)", mTransitions.size() - shown);
	}
}
