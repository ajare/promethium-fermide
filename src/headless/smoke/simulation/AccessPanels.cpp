#include "Checks.h"
#include "core/World.h"
#include "core/MobilityProfile.h"
#include "core/AgentTagRegistry.h"
#include "core/YamlSerializer.h"
#include <cmath>
#include <limits>

namespace
{
	using smoke::require;
	using Panel = core::AccessPanel;
	struct Fixture
	{
		core::World world{"Panel operation", 9, 3};
		uint32_t room, other, index;
		std::shared_ptr<const Panel> panel;
		Fixture(unsigned kind = 0, core::AccessPanelGeometry geometry = {})
		{
			room = kind == 0 ? world.addRoom("Room", 0, 0, 0, 8, 2)
				: kind == 1 ? world.addCorridor(0, 0, 0, 8, 1) : world.addFacade(0, 0, 0, 8, 2);
			other = world.addRoom("Other", 1, 0, 0, 8, 2);
			if (kind != 1) world.addSectorWalkway(room, 1, 4);
			auto made = world.addAccessPanel(room, 0, 4, geometry); index = made.index;
			panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(made.sector->getObject(index))->getPanel();
			world.finishBuild();
		}
		std::string saved()
		{
			auto out = core::YamlSerializer::toString(); core::SerializationWorkData data;
			data.markSerializedUnmodified = false; world.serialize(*out, data); out->serialize(); return out->getSerializedString();
		}
	};
	void animatedSpeed()
	{
		Fixture f;
		f.world.pauseSimulation();
		std::string diagnostic;
		require(f.panel->getSpeed() == Panel::DefaultSpeed && !f.panel->getSpeedOverride(), "Wrong default speed");
		for (auto speed : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
			require(!f.world.configureAccessPanel(f.room, f.index, f.panel->getGeometry(), &diagnostic, speed), "Invalid speed accepted");
		require(f.world.configureAccessPanel(f.room, f.index, f.panel->getGeometry(), &diagnostic, 0.25f), "Speed override failed");
		auto actor = f.world.createAgent("Opener", f.room, 0, 4.5f);
		auto closer = f.world.createAgent("Closer", f.room, 0, 4.5f);
		auto request = f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Open, actor);
		f.world.resumeSimulation(); f.world.advanceTick(); f.world.advanceTicks(30);
		require(f.panel->getState() == Panel::State::Opening && std::abs(f.panel->getProgress() - 0.5f) < 1e-5f, "Speed is not physical height per second");
		f.world.pauseSimulation(); auto progress = f.panel->getProgress();
		f.world.advanceTicks(30); require(f.panel->getProgress() == progress, "Pause advanced animation");
		auto close = f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Close, closer);
		f.world.resumeSimulation(); f.world.advanceTick();
		require(bool(close) && f.panel->getState() == Panel::State::Closing && f.panel->getProgress() >= progress,
			"Reversal teleported leaf");
		require(f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Failed, "Superseded travel did not fail its required interaction");
		f.world.advanceTicks(60); require(f.panel->getState() == Panel::State::Closed, "Reversal did not finish");
		f.world.pauseSimulation();
		require(f.world.configureAccessPanel(f.room, f.index, f.panel->getGeometry()), "Cannot clear speed override");
		require(!f.panel->getSpeedOverride() && f.panel->getSpeed() == Panel::DefaultSpeed, "Default not restored");
	}
	void approach(smoke::Context const&)
	{
		animatedSpeed();
		for (unsigned kind = 0; kind < 3; ++kind)
			for (auto geometry : {core::AccessPanelGeometry{}, {1, 0.25f, 0.75f}, {0, 0, 1}})
			{
				Fixture f(kind, geometry);
				auto id = f.world.createAgent("Approaching", f.room, 0, 1.5f);
				auto second = f.world.createAgent("Closer", f.room, 0, 4.5f);
				auto actor = f.world.lookupAgent(id).entity;
				f.world.markSaved(); auto bytes = f.saved(); auto graph = f.world.getGraph();
				auto object = f.world.getSector(f.room)->getObject(f.index);
				auto vertex = graph->getVertexForObject(object);
				require(vertex && vertex->getPosition() == core::Vector2{4.5f, 0}, "Missing graph-connected floor approach");
				require(f.panel->getActions() == std::vector{Panel::Action::Open}
					&& f.panel->getExposedActions() == std::vector{Panel::Action::Close}, "Common Open not separate from Empty actions");
				require(!f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Close, id), "Closed offers Close");
				auto request = f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Open, id);
				require(bool(request), "Approach request refused");
				auto operation = f.world.lookupInteractionRequest(request).entity->getOperations().front().first;
				require(f.world.lookupDeviceOperation(operation).entity->getCommand().type == core::DeviceCommandType::SetAccessPanelState,
					"Not a typed device operation");
				require(f.panel->getState() == Panel::State::Closed, "Request opened before reach");
				unsigned ticks = 0;
				while (f.panel->getState() == Panel::State::Closed && ticks++ < 600)
				{
					auto before = actor->getGlobalPosition();
					require(f.world.advanceTick(), "Approach tick failed");
					if (f.panel->getState() != Panel::State::Closed)
						require(before.distanceTo(vertex->getPosition()) <= 0.25f, "Opened outside vertex reach");
				}
				require(f.panel->getState() == Panel::State::Opening && f.panel->getProgress() == 0 && actor->getGlobalPosition().x < 4.5f
					&& actor->getGlobalPosition().x >= 4.25f, "No early ordinary-reach activation");
				for (unsigned travel=0; f.panel->getState()!=Panel::State::Open && travel<180; ++travel) f.world.advanceTick();
				require(f.panel->getState() == Panel::State::Open, "Animated opening failed");
				require(f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Succeeded
					&& f.world.lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Succeeded,
					"Animated operation did not publish queryable success");
				require(f.panel->getActions() == std::vector{Panel::Action::Close}, "Empty exposes extra controls/Open");
				require(!f.world.requestInteraction(f.panel->getControl(Panel::Action::Open), second), "Unavailable action bypassed via Interaction point");
				f.world.advanceTicks(120);
				require(f.panel->getState() == Panel::State::Open && f.world.getGraph() == graph
					&& graph->getVertexForObject(object) == vertex && f.saved() == bytes && !f.world.isModified(), "Open auto-closed or changed authored state/topology");
				auto close = f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Close, second);
				require(bool(close) && f.world.advanceTick() && f.panel->getState() == Panel::State::Closing,
					"Second Agent could not start closing");
				for (unsigned travel=0; f.panel->getState()!=Panel::State::Closed && travel<180; ++travel) f.world.advanceTick();
				require(f.panel->getState() == Panel::State::Closed, "Animated closing failed");
				require(f.world.lookupInteractionRequest(close).entity->getResult() == core::InteractionResult::Succeeded, "Close outcome missing");
				f.world.advanceTicks(2);
				require(bool(f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Open, second)), "Reopen refused");
				f.world.advanceTick(); auto old = f.panel->getId(); auto control = f.panel->getControl(Panel::Action::Open);
				f.world.resetSimulation();
				auto restored = std::static_pointer_cast<const core::AccessPanelSectorObject>(f.world.getSector(f.room)->getObject(f.index))->getPanel();
				require(restored->getState() == Panel::State::Closed && !f.world.lookupAccessPanel(old)
					&& !f.world.lookupInteractionPoint(control) && f.world.lookupInteractionPoint(restored->getControl(Panel::Action::Open)), "Reset retained Open/stale controls");
			}
	}
	void eligibility(smoke::Context const&)
	{
		Fixture f;
		auto wrongRoom = f.world.createAgent("Other", f.other, 0, 4.5f);
		auto wrongLevel = f.world.createAgent("Upper", f.room, 1, 4.5f);
		auto id = f.world.createAgent("Operator", f.room, 0, 1.5f);
		auto request = [&] { return f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Open, id); };
		for (auto wrong : {wrongRoom, wrongLevel})
			require(!f.world.requestInteraction(f.panel->getControl(Panel::Action::Open), wrong), "Wrong Location/Level accepted");
		f.world.pauseSimulation(); require(f.world.setAgentActive(id, false), "Deactivate failed"); f.world.resumeSimulation();
		require(!request(), "Inactive Agent accepted");
		f.world.pauseSimulation(); f.world.setAgentActive(id, true);
		core::MobilityProfile profile; profile.uses[static_cast<size_t>(core::TraversalKind::Buttons)] = core::MobilityUse::CannotUse;
		require(f.world.setAgentIndividualMobilityProfile(id, profile), "Profile edit failed"); f.world.resumeSimulation();
		require(!request(), "Buttons forbidden accepted");
		f.world.pauseSimulation(); profile.uses[static_cast<size_t>(core::TraversalKind::Buttons)] = core::MobilityUse::OnlyIfNoOtherOption;
		f.world.setAgentIndividualMobilityProfile(id, profile); f.world.resumeSimulation();
		auto pending = request(); require(bool(pending), "Last resort Buttons refused"); f.world.advanceTick();
		f.world.pauseSimulation(); profile.uses[static_cast<size_t>(core::TraversalKind::Buttons)] = core::MobilityUse::CannotUse;
		f.world.setAgentIndividualMobilityProfile(id, profile); f.world.resumeSimulation();
		auto position = f.world.lookupAgent(id).entity->getGlobalPosition(); f.world.advanceTick();
		require(f.world.lookupInteractionRequest(pending).entity->getResult() == core::InteractionResult::Cancelled
			&& f.panel->getState() == Panel::State::Closed && f.world.lookupAgent(id).entity->getGlobalPosition() == position,
			"Eligibility change did not cancel pending approach");
		f.world.pauseSimulation(); f.world.setAgentIndividualMobilityProfile(id, std::nullopt); f.world.resumeSimulation();
		pending = request(); require(bool(pending), "Fresh request refused");
		f.world.pauseSimulation(); f.world.setAgentActive(id, false); f.world.resumeSimulation(); f.world.advanceTick();
		require(f.world.lookupInteractionRequest(pending).entity->getResult() == core::InteractionResult::Cancelled
			&& f.panel->getState() == Panel::State::Closed, "Pending deactivation activated panel");
		f.world.pauseSimulation(); f.world.setAgentActive(id, true); f.world.resumeSimulation();
		id = f.world.createAgent("At activation reach", f.room, 0, 4.5f);
		f.world.pauseSimulation();
		auto registry = core::AgentTagRegistry::create(); auto tag = registry->addAgentTag("no-buttons");
		registry->addAgentTagMobilityProfile(tag); registry->setAgentTagMobilityProfile(tag, profile);
		f.world.attachAgentTagRegistry("panels.tags.yaml", registry); f.world.assignAgentTag(id, tag);
		f.world.resumeSimulation(); require(!request(), "Inherited Buttons restriction bypassed");
		f.world.pauseSimulation(); f.world.setAgentIndividualMobilityProfile(id, core::MobilityProfile{}); f.world.resumeSimulation();
		pending = request(); require(bool(pending), "Individual Buttons override refused");
		f.world.pauseSimulation(); f.world.setAgentIndividualMobilityProfile(id, std::nullopt); f.world.resumeSimulation();
		f.world.advanceTick();
		require(f.world.lookupInteractionRequest(pending).entity->getResult() == core::InteractionResult::Cancelled
			&& f.panel->getState() == Panel::State::Closed, "Execution ignored effective tag eligibility");
		require(!f.world.removeInteractionPoint(f.panel->getControl(Panel::Action::Open))
			&& !f.world.isInteractionPointPermissionEligible(f.panel->getControl(Panel::Action::Open)), "Owned control removal/permission allowed");
	}
	void stale(smoke::Context const&)
	{
		Fixture f; auto id = f.world.createAgent("Pending", f.room, 0, 1.5f);
		auto request = f.world.requestAccessPanel(f.panel->getId(), Panel::Action::Open, id);
		auto operation = f.world.lookupInteractionRequest(request).entity->getOperations().front().first;
		auto control = f.panel->getControl(Panel::Action::Open); auto old = f.panel->getId();
		f.world.advanceTick(); f.world.pauseSimulation();
		require(f.world.removeAccessPanel(f.room, f.index), "Pending removal refused");
		require(!f.world.lookupAccessPanel(old) && !f.world.lookupInteractionPoint(control) && !f.world.lookupDeviceOperation(operation)
			&& f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Cancelled, "Owned references/pending work survived deletion");
		auto made = f.world.addAccessPanel(f.room, 0, 4); f.world.finishBuild();
		auto fresh = std::static_pointer_cast<const core::AccessPanelSectorObject>(made.sector->getObject(made.index))->getPanel();
		require(fresh->getId() != old && fresh->getControl(Panel::Action::Open) != control
			&& !f.world.requestInteraction(control, id), "Replacement reused stale identity");
		f.world.resumeSimulation(); f.world.advanceTicks(600);
		require(fresh->getState() == Panel::State::Closed, "Stale work activated replacement");
		require(bool(f.world.requestAccessPanel(fresh->getId(), Panel::Action::Open, id)), "Replacement unusable");
		f.world.advanceTicks(600); require(fresh->getState() == Panel::State::Open, "Fresh replacement request failed");
	}
}
void registerAccessPanels(std::vector<smoke::Check>& checks)
{
	checks.push_back({"accessPanels/approachAndInstantReach", approach});
	checks.push_back({"accessPanels/eligibilityChanges", eligibility});
	checks.push_back({"accessPanels/staleRequestCleanup", stale});
}
