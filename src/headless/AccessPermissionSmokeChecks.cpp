#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include "imgui/imgui.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"

void runAccessPermissionSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> load(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("target", 1, 1);
		core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
		require(world->deserialize(*reader, work), "permission World did not load");
		return world;
	}

	void authorizationAndPersistence()
	{
		core::World world("permissions", 3, 2);
		auto corridor = world.addCorridor(0, 0, 3);
		auto control = world.addSectorLightSwitch(corridor, 1);
		world.finishBuild(); world.pauseSimulation();
		auto agent = world.createAgent("operator", corridor, 0, 1.5f);
		auto red = world.addAccessPermission("  Red key  ");
		auto blue = world.addAccessPermission("Blue key");
		std::string diagnostic;
		require(world.setInteractionPointPermissionRequirement(control.interactionPoint,
			{ red, blue }, &diagnostic), diagnostic);

		world.resumeSimulation();
		auto denied = world.requestInteraction(control.interactionPoint, agent);
		require(static_cast<bool>(denied), "unauthorized request had no observable outcome");
		auto rejected = world.lookupInteractionRequest(denied);
		require(rejected && rejected.entity->getResult() == core::InteractionResult::Rejected,
			"unauthorized request was not rejected");
		require(rejected.entity->getMissingPermissions().size() == 2,
			"rejection did not identify every missing permission");
		require(rejected.entity->getOperations().empty(), "rejection created a Device operation");
		require(world.getSimulationSnapshot().deviceOperations.empty(), "rejection changed device coordination");

		world.pauseSimulation();
		require(world.grantAgentAccessPermission(agent, red, &diagnostic), diagnostic);
		require(world.grantAgentAccessPermission(agent, blue, &diagnostic), diagnostic);
		require(world.resumeSimulation(), "authorized fixture did not resume");
		auto authorized = world.requestInteraction(control.interactionPoint, agent);
		require(static_cast<bool>(authorized), "authorized request was refused");
		auto admitted = world.lookupInteractionRequest(authorized);
		require(admitted && admitted.entity->getResult() == core::InteractionResult::Pending
			&& !admitted.entity->getOperations().empty(), "authorized request did not operate normally");
		world.advanceTicks(900);
		require(!world.getSector(corridor)->areLightsOn(), "authorized Agent did not operate the protected point");
		world.pauseSimulation();
		auto yaml = save(world);
		auto restored = load(yaml);
		require(restored->getAccessPermissionCount() == 2, "definitions did not persist");
		require(restored->getAgentEffectiveAccessGrants(agent).size() == 2, "grants did not persist");
		require(restored->getInteractionPointPermissionRequirement(control.interactionPoint).size() == 2,
			"requirement did not persist");

		restored->pauseSimulation();
		auto usage = restored->getAccessPermissionUsage(red);
		require(usage.directAgentGrants == 1 && usage.interactionPointRequirements == 1,
			"authoritative usage counts are wrong");
		require(restored->deleteAccessPermission(red, &diagnostic), diagnostic);
		require(restored->getAgentEffectiveAccessGrants(agent).size() == 1
			&& restored->getInteractionPointPermissionRequirement(control.interactionPoint).size() == 1,
			"deletion did not atomically clear references");
		auto reused = restored->addAccessPermission("Green key");
		require(reused == red, "cleared fixed-capacity slot was not reusable");
		require(restored->getAgentEffectiveAccessGrants(agent).size() == 1,
			"reused identity inherited an old grant");
	}

	void malformedAuthorizationIsTransactional()
	{
		core::World authored("malformed source", 2, 1);
		authored.addCorridor(0, 0, 2); authored.finishBuild(); authored.pauseSimulation();
		authored.addAccessPermission("Original");
		auto yaml = save(authored);
		auto section = yaml.find("accessPermissions:");
		auto id = yaml.find("id: 1", section);
		require(section != std::string::npos && id != std::string::npos, "permission fixture did not serialize definitions");
		yaml.replace(id, std::string("id: 1").size(), "id: 257");

		core::World target("untouched", 5, 3);
		target.addLayer(); target.addCorridor(0, 0, 5); target.finishBuild();
		auto before = save(target);
		bool refused = false;
		try
		{
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
			target.deserialize(*reader, work);
		}
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "out-of-range serialized authorization was accepted");
		require(save(target) == before, "refused authorization partially changed the target World");
	}

	void panelCommitParticipatesInHistory()
	{
		auto world = std::make_shared<core::World>("panel", 2, 1);
		auto corridor = world->addCorridor(0, 0, 2);
		auto control = world->addSectorLightSwitch(corridor, 1);
		world->finishBuild(); world->pauseSimulation();
		auto agent = world->createAgent("agent", corridor, 0, 0.5f);
		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();
		std::string diagnostic;
		auto id = commitAccessPermissionAdd(world, "Staff", diagnostic);
		require(id && gWorldDocumentHistory.canUndo() && world->isModified(),
			"Permissions panel commit did not create one document edit");

		ImGui::CreateContext();
		auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Permissions test");
		renderPermissionsPanel(world);
		renderAgentAccessPermissions(world, agent);
		renderInteractionPermissionRequirements(world, control.interactionPoint);
		ImGui::End(); ImGui::Render(); ImGui::DestroyContext();
	}
}

void runAccessPermissionSmokeChecks()
{
	authorizationAndPersistence();
	malformedAuthorizationIsTransactional();
	panelCommitParticipatesInHistory();
}
