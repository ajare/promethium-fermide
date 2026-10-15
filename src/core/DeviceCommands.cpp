#include "core/World.h"
#include "core/ExtensibleObject.h"
#include "core/Shuttle.h"
#include "core/Window.h"

namespace core
{
	std::string deviceCommandName(DeviceCommand const& command)
	{
		switch (command.type)
		{
		case DeviceCommandType::SetSectorLights:
			return command.desiredState ? "Set sector lights on" : "Set sector lights off";
		case DeviceCommandType::OpenDoor: return "Open door";
		case DeviceCommandType::SetExtendedState:
			return command.desiredState ? "Extend resource" : "Retract resource";
		case DeviceCommandType::ToggleExtendedState: return "Toggle resource extension";
		case DeviceCommandType::CallLift: return "Call lift";
		case DeviceCommandType::SelectLiftDestination: return "Select lift destination";
		case DeviceCommandType::CallShuttle: return "Call shuttle";
		case DeviceCommandType::SelectShuttleDestination: return "Select shuttle destination";
		case DeviceCommandType::RequestAirlock: return "Request Airlock";
		case DeviceCommandType::SetBoothWindowState: return "Set BoothWindow shutter target";
		case DeviceCommandType::ToggleBoothWindow: return "Toggle BoothWindow shutter";
		case DeviceCommandType::PressDumbwaiterLanding: return "Press Dumbwaiter landing";
		case DeviceCommandType::SetAccessPanelState:
			return command.desiredState ? "Open Access panel" : "Close Access panel";
		}
		return "Device command";
	}

	bool isDeviceCommandCoalescible(DeviceCommandType type)
	{
		switch (type)
		{
		case DeviceCommandType::SetSectorLights:
		case DeviceCommandType::OpenDoor:
		case DeviceCommandType::SetExtendedState:
		case DeviceCommandType::CallLift:
		case DeviceCommandType::SelectLiftDestination:
		case DeviceCommandType::CallShuttle:
		case DeviceCommandType::SelectShuttleDestination:
		case DeviceCommandType::RequestAirlock:
		case DeviceCommandType::SetBoothWindowState:
			return true;
		case DeviceCommandType::SetAccessPanelState: // Per-press activation/supersession.
		case DeviceCommandType::ToggleExtendedState: // Resolve once per activation.
		case DeviceCommandType::ToggleBoothWindow:
		case DeviceCommandType::PressDumbwaiterLanding: // Reserve busy at acceptance.
			return false;
		}
		return false;
	}

	bool World::hasValidDeviceCommandTarget(DeviceCommand const& command) const
	{
		auto resource = mTraversalResources.find(command.traversalResource);
		switch (command.type)
		{
		case DeviceCommandType::SetSectorLights:
			return command.target && command.target.value <= mSectors.size();
		case DeviceCommandType::OpenDoor:
			return resource && resource->mDoor;
		case DeviceCommandType::SetExtendedState:
		case DeviceCommandType::ToggleExtendedState:
			return resource && resource->mExtensible;
		case DeviceCommandType::CallLift:
		case DeviceCommandType::SelectLiftDestination:
		case DeviceCommandType::CallShuttle:
		case DeviceCommandType::SelectShuttleDestination:
			// Historically both command families accept either transport kind.
			// Do not tighten creation to match execution's device-specific checks.
			return resource && (resource->mLift || resource->mShuttle)
				&& command.stopIndex < resource->mLiftStops.size();
		case DeviceCommandType::RequestAirlock:
			return command.target && command.target.value <= mSectors.size()
				&& mSectors[command.target.value - 1]->getType() == SectorType::Airlock
				&& command.stopIndex < 2;
		case DeviceCommandType::SetBoothWindowState:
		case DeviceCommandType::ToggleBoothWindow:
		{
			auto booth = lookupBoothWindow(command.boothWindow);
			return booth && !booth->getDumbwaiterOwner();
		}
		case DeviceCommandType::PressDumbwaiterLanding:
			return lookupDumbwaiter(command.dumbwaiter) && command.stopIndex < 2;
		case DeviceCommandType::SetAccessPanelState:
			return bool(lookupAccessPanel(command.accessPanel));
		}
		return false;
	}

	bool World::isDeviceCommandTargetBroken(DeviceCommand const& command) const
	{
		auto resource = mTraversalResources.find(command.traversalResource);
		if (!resource) return false;
		switch (command.type)
		{
		case DeviceCommandType::OpenDoor:
			return resource->mDoor && resource->mDoor->isBroken();
		case DeviceCommandType::SetExtendedState:
		case DeviceCommandType::ToggleExtendedState:
			return resource->mExtensible && resource->mExtensible->isBroken();
		case DeviceCommandType::CallLift:
		case DeviceCommandType::SelectLiftDestination:
			return resource->mLift && resource->mLift->isBroken();
		case DeviceCommandType::CallShuttle:
		case DeviceCommandType::SelectShuttleDestination:
			return resource->mShuttle && resource->mShuttle->isBroken();
		case DeviceCommandType::SetSectorLights:
		case DeviceCommandType::RequestAirlock:
		case DeviceCommandType::SetBoothWindowState:
		case DeviceCommandType::ToggleBoothWindow:
		case DeviceCommandType::PressDumbwaiterLanding:
		case DeviceCommandType::SetAccessPanelState:
			return false;
		}
		return false;
	}
}
