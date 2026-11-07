#include "Checks.h"
#include "EditorState.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "AgentClipboard.h"
#include "AgentDropTargets.h"
#include "DocumentEdit.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool value, std::string const& message)
	{ if (!value) throw std::runtime_error(message); }

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	void placement(bool corridor)
	{
		auto world = std::make_shared<core::World>("Authored placement", 12, 1);
		auto protectedLocation = corridor ? world->addCorridor(0, 0, 0, 6, 1) : world->addRoom("Protected", 0, 0, 0, 6, 1);
		auto outside = world->addRoom("Outside", 0, 0, 6, 6, 1);
		world->finishBuild(); world->pauseSimulation();
		auto id = world->createAgent("Source", outside, 0, 0.5f);
		auto red = world->addAccessPermission("Red"), blue = world->addAccessPermission("Blue");
		auto card = world->addPermissionSet("Card");
		require(world->setPermissionSetAccessPermission(card, blue, true), "Card refused");
		require(world->setLocationPermissionRequirement(protectedLocation, { red, blue }), "Requirement refused");
		auto sector = world->getSector(protectedLocation);
		gUISettings.visibleLayer = 0;
		std::string diagnostic;
		PendingAgentPlacement pending;
		core::AgentId placed;
		for (unsigned grants = 0; grants < 5; ++grants)
		{
			if (grants == 1) require(world->grantAgentAccessPermission(id, red), "Red refused");
			if (grants == 2) require(world->revokeAgentAccessPermission(id, red)
				&& world->setAgentPermissionSetAssignment(id, card, true), "Partial set refused");
			if (grants == 3) require(world->grantAgentAccessPermission(id, red), "Combined grants refused");
			if (grants == 4) require(world->grantAgentAccessPermission(id, blue)
				&& world->setAgentPermissionSetAssignment(id, card, false), "Direct grants refused");
			auto payload = makeAgentClipboardPayload(*world, id, "Copy");
			payload.group = "New group";
			world->markSaved(); world->consumeSimulationEvents();
			gWorldDocumentHistory.clear(); gWorldDocumentHistory.markSaved();
			auto document = save(*world);
			auto pegman = pegmanAgentTargetAtWorld(world, { 1.5f, 0.5f });
			require(!pegman && pegman.diagnostic.find("('Red')") != std::string::npos
				&& pegman.diagnostic.find("('Blue')") != std::string::npos, "Pegman admitted ungranted creation / incomplete diagnostic");
			auto move = getAgentMoveTarget(world, world->lookupAgent(id).entity, { 1.5f, 0.5f });
			require(static_cast<bool>(move) == (grants >= 3), "Drag target ignored current authorization");
			if (grants < 3)
			{
				auto checkDiagnostic = [&]
				{
					require((diagnostic.find("('Red')") != std::string::npos) == (grants != 1)
						&& (diagnostic.find("('Blue')") != std::string::npos) == (grants != 2), "Clipboard omitted missing grants: " + diagnostic);
				};
				require(!armAgentPlacement(pending, *world, payload, sector, 0, 1.5f, diagnostic) && !pending.armed(), "Unauthorized fall armed");
				checkDiagnostic();
				require(!commitAgentPlacement(world, payload, sector, 0, 1.5f, placed, diagnostic) && !placed, "Unauthorized direct paste created Agent");
				checkDiagnostic();
				require(save(*world) == document && !world->isModified() && world->getAgentGroupCount() == 0
					&& sector->getAgents().empty() && world->consumeSimulationEvents().empty()
					&& !gWorldDocumentHistory.canUndo(), "Rejected paste changed document, ownership, events or history");
			}
			else
			{
				require(armAgentPlacement(pending, *world, payload, sector, 0, 1.5f, diagnostic), diagnostic);
				require(commitPendingAgentPlacement(pending, world, placed, diagnostic) && placed && !pending.armed(), diagnostic);
				require(world->lookupAgent(placed).entity->getSector()->getIndex() == protectedLocation
					&& gWorldDocumentHistory.canUndo(), "Authorized paste lost placement/history");
				require(world->removeAgent(placed).removed, "Copy removal refused");
				require(world->deleteAgentGroup(world->getAgentGroupIds()[0]), "Group removal refused");
			}
		}
		// A fall is rechecked against current set membership at landing.
		require(world->revokeAgentAccessPermission(id, blue)
			&& world->setAgentPermissionSetAssignment(id, card, true), "Fall authorization refused");
		auto payload = makeAgentClipboardPayload(*world, id, "Falling");
		require(armAgentPlacement(pending, *world, payload, sector, 0, 1.5f, diagnostic), diagnostic);
		require(world->setPermissionSetAccessPermission(card, blue, false), "Mid-fall membership loss refused");
		gWorldDocumentHistory.clear(); world->markSaved(); world->consumeSimulationEvents();
		auto document = save(*world);
		require(!commitPendingAgentPlacement(pending, world, placed, diagnostic) && !placed && !pending.armed()
			&& diagnostic.find("('Blue')") != std::string::npos, "Mid-fall loss bypassed landing gate");
		require(save(*world) == document && !world->isModified() && !gWorldDocumentHistory.canUndo()
			&& world->consumeSimulationEvents().empty() && sector->getAgents().empty(), "Refused landing partially created Agent");
		// Runtime grants govern direct drag relocation independently from copied authored grants.
		require(world->setAgentRuntimeAccessPermissionGrant(id, blue, true), "Runtime gain refused");
		require(static_cast<bool>(getAgentMoveTarget(world, world->lookupAgent(id).entity, { 1.5f, 0.5f })), "Drag ignored runtime grant");
		auto agent = world->lookupAgent(id).entity;
		world->validateAgentLocationPlacement(*sector, *agent);
		const_cast<core::Sector*>(world->getSector(outside).get())->exitAgent(agent);
		const_cast<core::Sector*>(sector.get())->enterAgent(agent, 0, 1.5f);
		require(agent->getSector()->getIndex() == protectedLocation && agent->getGlobalPosition() == core::Vector2{ 1.5f, 0 }, "Authorized direct relocation failed");
		require(world->setAgentRuntimeAccessPermissionGrant(id, blue, false), "Runtime loss refused");
		require(!getAgentMoveTarget(world, agent, { 2.5f, 0.5f })
			&& getAgentMoveTarget(world, agent, { 7.5f, 0.5f }), "Unauthorized occupant drag admitted internal placement / locked exit");
		// Cross-World clipboard grants are stripped before placement authorization.
		auto other = std::make_shared<core::World>("Other", 6, 1);
		auto room = other->addRoom("Other protected", 0, 0, 0, 6, 1);
		other->finishBuild(); other->pauseSimulation();
		auto key = other->addAccessPermission("Other key");
		require(other->setLocationPermissionRequirement(room, { key }), "Other requirement refused");
		require(!commitAgentPlacement(other, payload, other->getSector(room), 0, 1.5f, placed, diagnostic)
			&& diagnostic.find("('Other key')") != std::string::npos && other->getSector(room)->getAgents().empty(), "Foreign grants authorized paste");
	}
}

void permission_smoke::registerLocationPlacementEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "permissions/locationPlacementRoom", [](smoke::Context const&) { EditorState state; placement(false); } });
	checks.push_back({ "permissions/locationPlacementCorridor", [](smoke::Context const&) { EditorState state; placement(true); } });
}
