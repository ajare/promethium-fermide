#pragma once
#include "StationFlowDiagram.h"
#include "core/StationSystems.h"

#include <map>
#include <string>
#include <vector>

namespace core { class World; }

// #849: the alarm panel for the degradation-and-failure framework. It
// lists every active Fault by derived impact - the figure the operator
// alarms on, never an authored severity, so the list cannot drift from
// reality when parameters are retuned - and drives the repair tiers
// through the same validated World command surface a headless client
// uses. It owns no fault state: every row is re-read from the
// authoritative snapshot each frame, so a repair that landed clears the
// alarm without the panel being told, and a Fault that latched while the
// panel was closed appears in it on the next frame.
class StationFaultPanel
{
public:
	// One alarm row: the Fault, and what the operator can do about it
	// right now. `resettable` is a latched Trip - the free, instant fix,
	// legal on nothing else. `repairable` means the Installation has a
	// repair plan authored for that mode; without one, repairFault would
	// refuse, and the row says so instead of offering a button that fails.
	struct Row
	{
		station_flow::FaultBadge fault;
		bool resettable{false};
		bool repairable{false};
		std::string repairJobId;   // set once this panel started the repair
	};
	// Renders inside the caller's window/child region. A null snapshot means
	// no Station scenario is running, which is not the same claim as an
	// empty alarm list and is said differently.
	void render(core::World& world, core::StationSystemsSnapshot const* snapshot);

	std::vector<Row> const& rows() const { return mRows; }
	// The last command this panel issued that was refused, with the core's
	// own diagnostic. Cleared by the next accepted command.
	std::string const& notice() const { return mNotice; }
	// The fault/trip/repair transitions the panel last listed, newest
	// first, taken from the bounded event log's fault/trip/repair
	// categories. The panel never parses a message to decide what kind of
	// event it is - the category already says.
	std::vector<std::string> const& transitions() const { return mTransitions; }

private:
	std::vector<Row> mRows;
	std::vector<std::string> mTransitions;
	std::string mNotice;
	// The rows are rebuilt from the snapshot every frame, so a job id set
	// only on the frame the button was pressed would vanish on the next.
	// The started repairs are remembered here, keyed by installation and
	// mode, and read back into the row until the Fault itself is gone.
	std::map<std::string, std::string> mRepairJobs;
};
