// A tall Door set into a Facade clears exactly as a tall Door set into a
// Room does (#1060).
//
// Door clearance is the fit of an Agent's traversal vertical envelope
// below the doorway top relative to the approach Floor. Nothing in that
// rule reads the wall the Door stands in, and admitting a tall Door into a
// Facade must not make it start: a Facade stands the Room's 0.9 high, so
// the same opening admits the same Agents and refuses the same ones, at
// the same exact-fit boundaries and with the same selected Pose.
//
// The check builds the two walls side by side and compares them decision
// for decision rather than asserting one number once. A sweep across the
// tolerance band on both sides of the fit is what makes a quiet gain or
// loss of clearance visible; a single "the Human fits" assertion would
// pass with the Facade's opening two centimetres taller or shorter, and
// neither of those is the unchanged behaviour the ticket asks for.
//
// The comparison is made twice: against the Door's own standing-height
// fit, and against the crossing-mode selection every admission gate asks
// of a live Agent. The second is the one that would catch a wall leaking
// into Pose choice - a Facade that Crouched an Agent the Room let stand,
// or the reverse, is wrong in a way the height test alone cannot see.

#include "Checks.h"

#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorEdge.h"
#include "core/Facade.h"
#include "core/Graph.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vertex.h"
#include "core/World.h"

#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
	using smoke::require;

	std::shared_ptr<core::Door> onlyDoor(core::World const& world)
	{
		std::shared_ptr<core::Door> found;
		for (auto const& edge : world.getGraph()->getEdges())
		{
			if (edge->getType() != core::EdgeType::Door) continue;
			require(found == nullptr, "The fixture carries more than one Door edge");
			found = static_cast<core::DoorEdge const&>(*edge).getDoor();
		}
		require(found != nullptr, "The fixture carries no Door edge");
		return found;
	}

	// One tall ordinary Door between two Locations on adjacent Layers, with
	// the front wall the caller's choice of family. Everything else about
	// the two fixtures is identical, so the wall is the only thing the
	// comparison is allowed to see.
	struct Fixture
	{
		std::shared_ptr<core::World> world;
		uint32_t frontSector{};
		std::shared_ptr<core::Door> door;
	};

	Fixture buildFixture(std::string const& frontKind)
	{
		auto world = std::make_shared<core::World>("Facade clearance", 8, 2);
		uint32_t front{};
		if (frontKind == "facade")
			front = world->addFacade("Front", 0, 0, 0, 6, 1, CORE_ROOM_MAX_HEIGHT,
				core::Facade::defaultColour());
		else
			front = world->addRoom("Front", 0, 0, 0, 6, 1);
		world->addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.width = 1;
		options.height = core::Door::Height::Tall;
		world->addSectorDoor(0, 0, 2, options);
		world->finishBuild();
		world->pauseSimulation();
		Fixture fixture;
		fixture.world = world;
		fixture.frontSector = front;
		fixture.door = onlyDoor(*world);
		return fixture;
	}

	std::string describe(core::Door::DoorCrossingMode mode)
	{
		switch (mode)
		{
		case core::Door::DoorCrossingMode::Standing: return "Standing";
		case core::Door::DoorCrossingMode::Crouching: return "Crouching";
		case core::Door::DoorCrossingMode::Crawling: return "Crawling";
		case core::Door::DoorCrossingMode::None: return "None";
		}
		return "unknown";
	}

	// The exact-fit approach floor for a standing baseline Human under the
	// tall opening, and the sweep across the tolerance band on both sides
	// of it that the two walls have to answer identically.
	std::vector<float> clearanceOffsets(core::Door const& door, float standing)
	{
		float const fit = door.getPosition().y
			+ core::Door::effectiveHeight(core::Door::Height::Tall) - standing;
		return {
			fit - 1.0f, fit - 0.2f,
			fit - core::Door::StandingClearanceTolerance * 2.0f,
			fit - core::Door::StandingClearanceTolerance * 0.5f,
			fit,
			fit + core::Door::StandingClearanceTolerance * 0.5f,
			fit + core::Door::StandingClearanceTolerance,
			fit + core::Door::StandingClearanceTolerance * 2.0f,
			fit + 0.2f, fit + 0.5f };
	}
}

void facadeTallDoorClearanceMatchesTheRoomRule(smoke::Context const&)
{
	auto const room = buildFixture("room");
	auto const facade = buildFixture("facade");

	require(facade.door->getFrontSector()->getType() == core::SectorType::Facade,
		"The Facade fixture's Door is not authored with a Facade in front");
	require(room.door->getFrontSector()->getType() == core::SectorType::Location,
		"The control fixture's Door is not authored with a Room in front");
	require(facade.door->getHeight() == core::Door::Height::Tall
			&& room.door->getHeight() == core::Door::Height::Tall,
		"A fixture Door is not Tall, so the sweep below compares nothing");
	require(facade.door->getSize().y == room.door->getSize().y
			&& facade.door->getPosition().y == room.door->getPosition().y,
		"The tall Facade Door is not placed and sized like the tall Room Door, "
		"so a clearance difference could hide in the geometry");

	auto const standing = core::bundledHumanBaseline().standingHeight;
	auto const offsets = clearanceOffsets(*facade.door, standing);

	for (auto const feet : offsets)
	{
		auto const want = room.door->admitsStandingHeight(standing, feet);
		auto const got = facade.door->admitsStandingHeight(standing, feet);
		require(got == want, ("A tall Facade Door's standing clearance answers "
			+ std::string(got ? "admits" : "refuses") + " where the tall Room "
			"Door answers " + (want ? "admits" : "refuses") + " at feet "
			+ std::to_string(feet)).c_str());
		require(facade.door->admitsVerticalExtent(standing, feet)
				== room.door->admitsVerticalExtent(standing, feet),
			("A tall Facade Door's vertical-extent fit disagrees with the tall "
				"Room Door's at feet " + std::to_string(feet)).c_str());
	}

	// The band is real: the sweep crosses the fit rather than sitting wholly
	// on one side of it, which is the only way the agreement above means
	// anything.
	require(room.door->admitsStandingHeight(standing, offsets.front()),
		"The control sweep admits nothing at all, so the agreement above is "
		"vacuous");
	require(!room.door->admitsStandingHeight(standing, offsets.back()),
		"The control sweep admits everything, so the agreement above is "
		"vacuous");

	// The same comparison on a live Agent, through the crossing-mode
	// selection every admission gate asks. Sweeping the approach floor
	// carries the Agent through Standing, Crouching and Crawling and out
	// the other side, so the whole band of Pose choice is compared and not
	// just the fit. A Facade that Crouched an Agent the Room let stand, or
	// the reverse, fails here even where the height test alone would pass.
	auto const roomId = room.world->createAgent("Clearance probe",
		room.frontSector, 0, 2.5f);
	auto const facadeId = facade.world->createAgent("Clearance probe",
		facade.frontSector, 0, 2.5f);
	auto const roomAgent = room.world->lookupAgent(roomId).entity;
	auto const facadeAgent = facade.world->lookupAgent(facadeId).entity;
	require(roomAgent != nullptr && facadeAgent != nullptr, "A fixture Agent could not be created");
	require(std::abs(roomAgent->getGlobalPosition().y
			- facadeAgent->getGlobalPosition().y) < 1e-4f,
		"The two fixtures put the same Agent at different feet heights, so "
		"the comparison below is not like for like");

	std::set<core::Door::DoorCrossingMode> modesSeen;
	for (auto const feet : offsets)
	{
		auto const want = room.door->classifyAgentCrossing(*roomAgent, feet);
		auto const got = facade.door->classifyAgentCrossing(*facadeAgent, feet);
		modesSeen.insert(want);
		require(want == got, ("A tall Facade Door selected " + describe(got)
			+ " at feet " + std::to_string(feet) + " where the tall Room "
			"Door selected " + describe(want)).c_str());
		require(facade.door->admitsAgentTraversal(*facadeAgent, feet)
				== room.door->admitsAgentTraversal(*roomAgent, feet),
			("A tall Facade Door's traversal admission disagrees with the "
				"tall Room Door's at feet " + std::to_string(feet)).c_str());
	}
	// The sweep has to reach more than one mode, or the agreement above is
	// one answer wearing a loop.
	require(modesSeen.size() >= 3,
		"The crossing-mode sweep reached only " + std::to_string(modesSeen.size())
		+ " mode(s), so it does not compare the Pose choice at all");
}

void agent_smoke::registerFacadeDoorClearance(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "facadeTallDoorClearanceMatchesTheRoomRule",
		facadeTallDoorClearanceMatchesTheRoomRule });
}
