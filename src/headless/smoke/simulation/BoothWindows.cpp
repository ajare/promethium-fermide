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
}
