#include "Checks.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Pathing.h"
#include "core/Agent.h"
#include <cmath>

namespace
{
	using smoke::require;
	using core::Window;
	using core::DeviceOperationState;
	struct Fixture
	{
		core::World world{"Shutter commands", 8, 3};
		std::shared_ptr<const core::BoothWindow> booth;
		Fixture(bool open = false)
		{
			world.addRoom("Front", 0, 0, 0, 7, 2);
			world.addRoom("Back", 1, 0, 0, 7, 2);
			booth = std::static_pointer_cast<const core::BoothWindow>(world.addBoothWindow(0, 0, 3,
				open ? Window::State::Open : Window::State::Closed).object);
			world.finishBuild(); world.markSaved();
		}
		core::DeviceCommand command(core::DeviceCommandType type = core::DeviceCommandType::ToggleBoothWindow) const
		{
			core::DeviceCommand result; result.type = type; result.boothWindow = booth->getDeviceId(); return result;
		}
		core::DeviceOperationId toggle() { return world.submitDeviceCommand(command()); }
		void ticks(unsigned count)
		{
			for (unsigned i = 0; i < count; ++i)
			{
				require(world.advanceTick(), "Shutter tick failed");
				auto graph = world.getGraph();
				auto front = graph->getVertexAtPosition(0, 3.5f, 0, 0.01f);
				auto back = graph->getVertexAtPosition(1, 3.5f, 0, 0.01f);
				require(front && back && !core::pathing::findPath(nullptr, graph.get(), front, back)
					&& world.getSimulationSnapshot().traversalResources.empty(), "Motion created crossing topology/admission");
			}
		}
		void progress(float expected) const
		{
			require(std::abs(booth->getProgress() - expected) < 0.00001f, "Shutter progress jumped or speed changed");
		}
	};
	void timing(smoke::Context const&)
	{
		Fixture f;
		auto opening = f.toggle();
		require(opening && f.world.lookupDeviceOperation(opening).entity->getState() == DeviceOperationState::Pending,
			"Editor command did not create a queryable typed operation");
		f.progress(0); // Acceptance is not physical activation.
		f.ticks(1); f.progress(1.0f / 48);
		require(f.booth->getState() == Window::State::Opening && f.booth->getTargetOpen(), "Opening/target not exposed");
		f.ticks(46); f.progress(47.0f / 48);
		require(f.booth->getState() == Window::State::Opening, "Opening completed before 0.8 seconds");
		f.ticks(1); f.progress(1);
		require(f.booth->getState() == Window::State::Open
			&& f.world.lookupDeviceOperation(opening).entity->getState() == DeviceOperationState::Succeeded,
			"Full opening missed 0.8-second endpoint/outcome");
		f.ticks(600); f.progress(1); require(!f.world.lookupDeviceOperation(opening), "Consumed operation leaked");
		auto closing = f.toggle(); f.ticks(47); f.progress(1.0f / 48);
		require(f.booth->getState() == Window::State::Closing && !f.booth->getTargetOpen(), "Closing target/state incorrect");
		f.ticks(1); f.progress(0);
		require(f.booth->getState() == Window::State::Closed
			&& f.world.lookupDeviceOperation(closing).entity->getState() == DeviceOperationState::Succeeded,
			"Full closing missed 0.8-second endpoint");
		f.ticks(600); f.progress(0);
		for (unsigned cycle = 0; cycle < 4; ++cycle)
		{
			auto out = f.toggle(); f.ticks(12); f.progress(0.25f);
			auto reverse = f.toggle(); f.progress(0.25f);
			f.ticks(1); f.progress(11.0f / 48);
			require(f.world.lookupDeviceOperation(out).entity->getState() == DeviceOperationState::Cancelled,
				"Reversal did not retire superseded travel");
			f.ticks(10); f.progress(1.0f / 48); f.ticks(1); f.progress(0);
			require(f.world.lookupDeviceOperation(reverse).entity->getState() == DeviceOperationState::Succeeded,
				"Partial closing did not take proportional time");
		}
		f.toggle(); f.ticks(24); f.progress(0.5f);
		f.toggle(); f.ticks(6); f.progress(0.375f);
		f.toggle(); f.ticks(29); f.progress(47.0f / 48); f.ticks(1); f.progress(1);
		// Multiple activations in one tick are separate toggles, not one shared operation.
		auto first = f.toggle(), second = f.toggle(); require(first != second, "Toggles coalesced");
		f.ticks(1); f.progress(1);
		require(f.booth->getTargetOpen(), "Repeated toggle replayed on later ticks");
		f.ticks(100); f.progress(1);
		f.toggle(); f.ticks(10); auto paused = f.booth->getProgress();
		f.world.pauseSimulation(); f.world.update(5); f.progress(paused);
		require(!f.world.advanceTick(), "Paused simulation advanced shutter tick");
		f.world.resumeSimulation(); f.ticks(38); f.progress(0);
		f.toggle(); f.ticks(12); f.world.pauseSimulation();
		f.world.update(5); f.progress(0.25f);
		f.world.resumeSimulation(); f.world.setSimulationTimeScale(2);
		f.world.update(0.3f); f.progress(1);
		require(!f.world.isModified(), "Runtime command became an authored edit");
	}
	void activation(smoke::Context const&)
	{
		Fixture f;
		auto actor = f.world.createAgent("Operator", 1, 0, 3.5f);
		auto position = f.world.lookupAgent(actor).entity->getGlobalPosition();
		auto point = f.world.createInteractionPoint("Invisible shutter test binding", core::SectorId{2},
			position, 0.1f, f.world.getFixedTimestep() * 3, {{f.command(), core::InteractionBindingRequirement::Required}});
		auto request = f.world.requestInteraction(point, actor);
		require(bool(request), "Typed shutter Interaction binding refused");
		auto operation = f.world.lookupInteractionRequest(request).entity->getOperations().front().first;
		auto duration = f.world.lookupInteractionPoint(point).entity->getDurationTicks();
		f.ticks(static_cast<unsigned>(duration)); f.progress(0);
		require(!f.booth->getTargetOpen(), "Toggle was applied at request rather than activation");
		f.ticks(1); f.progress(1.0f / 48);
		require(f.world.lookupDeviceOperation(operation).entity->getCommand().desiredState,
			"Activated toggle did not expose resolved target");
		f.ticks(47); f.progress(1);
		require(f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Succeeded,
			"Interaction did not observe shutter operation outcome");
		f.ticks(120); f.progress(1);
		// Desired-state commands remain idempotent and share pending equivalent work.
		auto close = f.command(core::DeviceCommandType::SetBoothWindowState);
		auto first = f.world.submitDeviceCommand(close), second = f.world.submitDeviceCommand(close);
		require(first == second, "Equivalent desired-state commands did not coalesce");
		f.ticks(12); f.progress(0.75f);
		close.desiredState = true;
		auto reverse = f.world.submitDeviceCommand(close); f.ticks(12); f.progress(1);
		require(f.world.lookupDeviceOperation(reverse).entity->getState() == DeviceOperationState::Succeeded,
			"Desired-state reversal did not complete proportional opening");
	}

	void panelPermissions()
	{
		for (unsigned scenario = 0; scenario < 11; ++scenario)
		{
			Fixture f;
			auto id = f.world.createAgent("Protected operator", scenario == 7 ? 0 : 1, 0,
				scenario == 8 ? 4.0f : 3.4f);
			auto actor = f.world.lookupAgent(id).entity;
			auto position = actor->getGlobalPosition();
			f.world.pauseSimulation();
			auto a = f.world.addAccessPermission("A"), b = f.world.addAccessPermission("B");
			auto set = f.world.addPermissionSet("Operators");
			require(f.world.setPermissionSetAccessPermission(set, b, true), "Set grant fixture failed");
			require(f.world.setInteractionPointPermissionRequirement(f.booth->getPanel(), {a,b}), "Owned panel requirement refused");
			require(f.world.setAgentIndividualPermissionAdherence(id, false), "Adherence edit failed");
			if (scenario != 0) require(f.world.grantAgentAccessPermission(id, a), "Direct grant failed");
			if (scenario >= 2)
			{
				if (scenario == 2) require(f.world.grantAgentAccessPermission(id, b), "Second direct grant failed");
				else require(f.world.setAgentPermissionSetAssignment(id, set, true), "Set assignment failed");
			}
			if (scenario == 9)
			{
				require(f.world.grantAgentAccessPermission(id, b), "Overlapping direct grant failed");
				require(f.world.setAgentRuntimePermissionSetAssignment(id, set, false), "Overlap set loss failed");
			}
			f.world.resumeSimulation();
			auto request = f.world.requestInteraction(f.booth->getPanel(), id);
			if (scenario < 2 || scenario == 7 || scenario == 8)
			{
				if (scenario < 2)
				{
					auto outcome = f.world.lookupInteractionRequest(request).entity;
					require(outcome && outcome->getResult() == core::InteractionResult::Rejected
						&& outcome->getMissingPermissions().size() == (scenario == 0 ? 2u : 1u),
						"Missing all-of permission bypassed by non-adherence");
				}
				else require(!request, "Held permissions bypassed reach/side eligibility");
				f.ticks(3); f.progress(0);
				require(!f.booth->getTargetOpen() && actor->getGlobalPosition().distanceTo(position) == 0
					&& f.world.getSimulationSnapshot().deviceOperations.empty(), "Refused press changed target/Agent/work");
				continue;
			}
			require(bool(request), "Effective direct/set grants refused");
			// Revoke after admission but before the one-tick physical press.
			if (scenario == 4 || scenario == 5)
			{
				if (scenario == 4) require(f.world.setAgentRuntimeAccessPermissionGrant(id, a, false), "Grant loss failed");
				else require(f.world.setAgentRuntimePermissionSetAssignment(id, set, false), "Set loss failed");
				f.ticks(1);
				auto outcome = f.world.lookupInteractionRequest(request).entity;
				require(outcome && outcome->getResult() == core::InteractionResult::Rejected
					&& !outcome->getMissingPermissions().empty(), "Pre-activation loss did not report normal authorization refusal");
				f.ticks(3); f.progress(0);
				require(!f.booth->getTargetOpen() && actor->getGlobalPosition().distanceTo(position) == 0,
					"Rejected press moved operator or toggled target");
				require(f.world.getSimulationSnapshot().deviceOperations.empty()
					&& !f.world.lookupInteractionPoint(f.booth->getPanel()).entity->getActiveRequest(),
					"Authorization rejection retained operations or panel reservation");
				continue;
			}
			if (scenario == 10)
			{
				f.ticks(1); // Physical press accepted; device advancement is next tick.
				require(f.world.setAgentRuntimePermissionSetAssignment(id, set, false), "Activated press grant loss failed");
				f.ticks(1);
			}
			else f.ticks(2);
			require(f.booth->getTargetOpen(), "Authorized press failed to activate");
			if (scenario == 6)
			{
				require(f.world.setAgentRuntimePermissionSetAssignment(id, set, false), "Post-press loss failed");
			}
			f.ticks(47); f.progress(1);
			require(f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Succeeded
				&& actor->getGlobalPosition().distanceTo(position) == 0, "Accepted toggle was revoked or moved Agent");
			if (scenario == 6)
			{
				auto next = f.world.requestInteraction(f.booth->getPanel(), id);
				require(next && f.world.lookupInteractionRequest(next).entity->getResult() == core::InteractionResult::Rejected,
					"Subsequent unauthorized press accepted");
				f.ticks(3); f.progress(1);
			}
		}
	}

	void panelAgents(smoke::Context const&)
	{
		panelPermissions();
		// Inclusive reach boundary, no exact arrival, no crossing intent.
		for (float x : {3.25f, 3.35f, 3.5f, 3.75f})
		{
			Fixture f;
			auto id = f.world.createAgent("Back operator", 1, 0, x);
			auto actor = f.world.lookupAgent(id).entity;
			auto position = actor->getGlobalPosition(); auto path = actor->getPath();
			auto point = f.booth->getPanel();
			auto panel = f.world.lookupInteractionPoint(point).entity;
			require(panel && panel->getDurationTicks() == 1 && panel->getReach() == 0.25f
				&& panel->getPosition().x == 3.5f && panel->getPosition().y == 0
				&& panel->getBoothWindowOwner() == f.booth->getDeviceId(), "Wrong owned panel contract");
			require(!f.world.removeInteractionPoint(point), "Owned panel independently removed");
			auto request = f.world.requestInteraction(point, id);
			require(bool(request), "Eligible within-reach operator refused");
			auto operation = f.world.lookupInteractionRequest(request).entity->getOperations().front().first;
			f.ticks(1); f.progress(0);
			require(!f.booth->getTargetOpen(), "Queued panel press captured/applied target too early");
			f.ticks(1); f.progress(1.0f / 48);
			require(f.booth->getTargetOpen() && actor->getGlobalPosition().distanceTo(position) == 0
				&& actor->getPath() == path, "Press moved operator or altered route");
			require(f.world.lookupDeviceOperation(operation).entity->getCommand().desiredState,
				"Typed panel operation did not resolve target at activation");
			f.ticks(47);
			require(f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Succeeded,
				"Panel completion not surfaced through Interaction outcome");
			auto again = f.world.requestInteraction(point, id); require(bool(again), "Repeated press refused");
			f.ticks(2); f.progress(47.0f / 48);
			require(!f.booth->getTargetOpen(), "Repeated press did not toggle current target");
		}
		for (unsigned scenario = 0; scenario < 6; ++scenario)
		{
			Fixture f;
			f.world.pauseSimulation(); f.world.addLayer(); f.world.addRoom("Other", 2, 0, 0, 7, 2);
			f.world.finishBuild(); f.world.resumeSimulation();
			auto layer = scenario == 2 ? 0u : scenario == 3 ? 2u : 1u;
			auto x = scenario == 0 ? 3.249f : scenario == 1 ? 3.751f : 3.5f;
			auto id = f.world.createAgent("Ineligible", layer, 0, x);
			auto actor = f.world.lookupAgent(id).entity;
			if (scenario == 4) actor->setActive(false);
			if (scenario == 5)
			{
				f.world.pauseSimulation();
				core::MobilityProfile profile; profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
				require(f.world.setAgentIndividualMobilityProfile(id, profile), "Mobility edit failed");
				f.world.resumeSimulation();
			}
			if (scenario == 0)
			{
				f.world.pauseSimulation(); uint32_t destination;
				f.world.addSectorMarker(1, 0, 6.5f, &destination); f.world.finishBuild();
				auto graph = f.world.getGraph();
				auto path = core::pathing::findPath(actor, graph.get(),
					graph->getVertexAtPosition(1, 3.5f, 0, 0.01f), graph->getVertexByIdentifier(destination));
				require(bool(path), "No-movement rejection route fixture failed");
				actor->setPath(path, false); f.world.resumeSimulation();
			}
			auto position = actor->getGlobalPosition(); auto path = actor->getPath();
			require(!f.world.requestInteraction(f.booth->getPanel(), id), "Ineligible panel request accepted");
			f.ticks(3); f.progress(0);
			require(actor->getGlobalPosition().distanceTo(position) == 0 && actor->getPath() == path
				&& !f.booth->getTargetOpen() && f.world.getSimulationSnapshot().deviceOperations.empty(),
				"Rejected panel request moved Agent, changed route/target, or allocated work");
		}
		{
			Fixture f;
			auto id = f.world.createAgent("Last-resort Buttons", 1, 0, 3.4f);
			f.world.pauseSimulation(); core::MobilityProfile profile;
			profile.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
			require(f.world.setAgentIndividualMobilityProfile(id, profile), "Last-resort edit failed");
			f.world.resumeSimulation();
			require(bool(f.world.requestInteraction(f.booth->getPanel(), id)), "Normal last-resort Buttons capability refused");
			f.ticks(2); require(f.booth->getTargetOpen(), "Allowed Buttons profile did not operate panel");
		}
		// Recheck queued eligibility, not just the request-time position/profile.
		for (unsigned departure = 0; departure < 4; ++departure)
		{
			Fixture f;
			auto id = f.world.createAgent("Departing", 1, 0, 3.4f);
			auto actor = f.world.lookupAgent(id).entity;
			auto request = f.world.requestInteraction(f.booth->getPanel(), id);
			require(bool(request), "Departure fixture request refused");
			if (departure < 2)
			{
				std::const_pointer_cast<core::Sector>(f.world.getSector(1))->exitAgent(actor);
				std::const_pointer_cast<core::Sector>(f.world.getSector(departure == 0 ? 1 : 0))->enterAgent(
					actor, 0, departure == 0 ? 4.0f : 3.4f);
			}
			if (departure == 2) actor->setActive(false);
			if (departure == 3)
			{
				f.world.pauseSimulation();
				core::MobilityProfile profile; profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
				require(f.world.setAgentIndividualMobilityProfile(id, profile), "Mobility edit failed");
				f.world.resumeSimulation();
			}
			auto position = actor->getGlobalPosition();
			f.ticks(1);
			require(f.world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Cancelled,
				"Departed/ineligible queued operator was not cancelled");
			f.ticks(3); f.progress(0);
			require(actor->getGlobalPosition().distanceTo(position) == 0 && !f.booth->getTargetOpen(),
				"Cancelled press auto-approached or operated late");
		}
		for (unsigned repeat = 0; repeat < 3; ++repeat)
		{
			Fixture f;
			auto a = f.world.createAgent("First", 1, 0, 3.3f), b = f.world.createAgent("Second", 1, 0, 3.7f);
			// Queue order, not Agent identity, decides the first activation.
			auto first = f.world.requestInteraction(f.booth->getPanel(), b);
			auto second = f.world.requestInteraction(f.booth->getPanel(), a);
			require(first && second && first != second, "Competing presses lost distinct requests");
			auto op1 = f.world.lookupInteractionRequest(first).entity->getOperations().front().first;
			auto op2 = f.world.lookupInteractionRequest(second).entity->getOperations().front().first;
			require(op1 != op2, "Competing toggles coalesced");
			f.ticks(2); f.progress(1.0f / 48);
			require(f.booth->getTargetOpen() && f.world.lookupDeviceOperation(op1).entity->getCommand().desiredState,
				"First queued operator did not toggle first");
			f.ticks(1); f.progress(0);
			require(!f.booth->getTargetOpen() && !f.world.lookupDeviceOperation(op2).entity->getCommand().desiredState,
				"Second press captured stale target instead of reversing");
			f.ticks(4); f.progress(0);
		}
	}

	void panelLifecycle(smoke::Context const&)
	{
		for (unsigned change = 0; change < 6; ++change)
		{
			Fixture f;
			if (change == 5) { f.world.pauseSimulation(); f.world.addLayer(); f.world.resumeSimulation(); }
			auto id = f.world.createAgent("Outstanding operator", 1, 0, 3.4f);
			auto stale = f.world.requestInteraction(f.booth->getPanel(), id);
			auto operation = f.world.lookupInteractionRequest(stale).entity->getOperations().front().first;
			if (change == 1) f.ticks(2); // Deletion during shutter travel as well as before the press.
			f.world.pauseSimulation();
			if (change < 2) require(f.world.removeSectorWindow(0, 0), "Panel owner deletion failed");
			if (change == 2) f.world.resetSimulation();
			if (change == 3)
			{
				auto plan = f.world.planMoveSectorObject(0, 0, 4, 0);
				require(plan.valid && f.world.applyObjectMove(plan), "Panel owner move failed");
			}
			if (change == 4) f.world.applyLocationEdit(f.world.planRemoveLocation(1));
			if (change == 5) f.world.applyDeleteLayer(f.world.planDeleteLayer(1));
			require(!f.world.lookupInteractionRequest(stale) && !f.world.lookupDeviceOperation(operation)
				&& !f.world.lookupBoothWindow(f.booth->getDeviceId()), "Structural edit retained stale pending/active work");
			auto snapshot = f.world.getSimulationSnapshot();
			require(snapshot.interactionPoints.size() == ((change == 2 || change == 3) ? 1u : 0u)
				&& snapshot.interactionRequests.empty() && snapshot.deviceOperations.empty(),
				"Structural edit leaked/duplicated owned panel or pending presses");
			if (change == 2 || change == 3)
			{
				auto panel = snapshot.interactionPoints.front();
				require(panel.position.x == (change == 3 ? 4.5f : 3.5f) && panel.position.y == 0
					&& panel.sectorId == core::SectorId{2}, "Panel did not relocate to owner's back-side approach");
				auto fresh = f.world.requestInteraction(panel.id, id);
				if (change == 2) require(fresh && fresh != stale, "Reset reused stale request reference");
				if (change == 3) require(!fresh, "Relocated panel auto-approached old Agent position");
			}
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (bool initial : {false, true})
		{
			Fixture f(initial); auto stale = f.command(); auto oldDevice = f.booth->getDeviceId();
			auto operation = f.toggle(); f.ticks(12);
			f.world.resetSimulation();
			require(!f.world.lookupBoothWindow(oldDevice) && !f.world.lookupDeviceOperation(operation)
				&& !f.world.submitDeviceCommand(stale), "Reset reused a retired device/operation reference");
			auto object = std::dynamic_pointer_cast<const core::WindowSectorObject>(f.world.getSector(0)->getObject(0));
			f.booth = std::static_pointer_cast<const core::BoothWindow>(object->getWindow());
			f.progress(initial ? 1.0f : 0.0f);
			require(f.booth->getTargetOpen() == initial, "Reset retained runtime target");
			auto fresh = f.toggle(); require(fresh != operation, "Operation identity aliased after reset"); f.ticks(4);
			f.world.pauseSimulation(); stale = f.command(); oldDevice = f.booth->getDeviceId();
			f.world.applyLocationEdit(f.world.planResizeLocation(0, 0, 0, 8, 2));
			require(!f.world.lookupBoothWindow(oldDevice) && !f.world.submitDeviceCommand(stale), "Reconstruction operated detached device");
			object = std::dynamic_pointer_cast<const core::WindowSectorObject>(f.world.getSector(0)->getObject(0));
			f.booth = std::static_pointer_cast<const core::BoothWindow>(object->getWindow()); f.progress(initial ? 1.0f : 0.0f);
			stale = f.command();
			require(f.world.removeSectorWindow(0, 0), "BoothWindow deletion failed");
			require(!f.world.lookupBoothWindow(stale.boothWindow) && !f.world.submitDeviceCommand(stale), "Deleted device handle stayed live");
		}
		Fixture f;
		auto pending = f.toggle(); auto stale = f.command(); f.world.resetSimulation();
		require(!f.world.lookupDeviceOperation(pending) && !f.world.submitDeviceCommand(stale),
			"Reset retained pending operations");
		auto command = f.command(); command.boothWindow = core::BoothWindowId{9999};
		require(!f.world.submitDeviceCommand(command) && f.world.getSimulationSnapshot().deviceOperations.empty(),
			"Invalid target allocated pending work");
	}
}
void registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"boothWindows/runtimeTimingAndReversal", timing});
	checks.push_back({"boothWindows/runtimeLifecycle", lifecycle});
	checks.push_back({"boothWindows/typedInteractionActivation", activation});
	checks.push_back({"boothWindows/backSideAgentPanel", panelAgents});
	checks.push_back({"boothWindows/ownedPanelLifecycle", panelLifecycle});
}
