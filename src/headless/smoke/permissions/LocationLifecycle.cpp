#include "Checks.h"
#include "core/World.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include <memory>
#include <stdexcept>
#include <string>

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

	void roundTrip(core::World& world)
	{
		auto expected = save(world);
		auto verify = [&](auto writer, auto fromString)
		{
			core::SerializationWorkData work;
			world.serialize(*writer, work); writer->serialize();
			auto reader = fromString(writer->getSerializedString()); reader->deserialize();
			core::World restored("restored", 1, 1);
			require(restored.deserialize(*reader, work) && save(restored) == expected,
				"Complete Location lifecycle round trip changed document");
		};
		verify(core::YamlSerializer::toString(), core::YamlSerializer::fromString);
		verify(core::BinarySerializer::toString(), core::BinarySerializer::fromString);
	}

	uint32_t add(core::World& world, bool corridor, uint32_t x)
	{
		return corridor ? world.addCorridor(0, 0, x, 4, 2)
			: world.addRoom("Room", 0, 0, x, 4, 2);
	}

	void lifecycle(bool corridor)
	{
		core::World world("Lifecycle", 24, 4);
		auto earlier = add(world, !corridor, 0);
		auto selected = add(world, corridor, 6);
		auto unrelated = add(world, !corridor, 18);
		world.finishBuild(); world.pauseSimulation();
		auto red = world.addAccessPermission("Red");
		auto blue = world.addAccessPermission("Blue");
		require(world.setLocationPermissionRequirement(earlier, { red })
			&& world.setLocationPermissionRequirement(selected, { red, blue })
			&& world.setLocationPermissionRequirement(unrelated, { blue }), "Requirements refused");
		auto check = [&]
		{
			require(world.getLocationPermissionRequirement(selected) == std::vector<core::AccessPermissionId>{ red, blue }
				&& world.getLocationPermissionRequirement(unrelated) == std::vector<core::AccessPermissionId>{ blue },
				"Structural reconstruction lost or aliased protection");
			roundTrip(world);
		};
		auto apply = [&](core::World::LocationEditPlan const& plan)
		{
			require(plan.valid, plan.diagnostic);
			return world.applyLocationEdit(plan);
		};
		selected = apply(world.planResizeLocation(selected, 8, 1, 4, 2)); check();
		selected = apply(world.planResizeLocation(selected, 8, 1, 6, corridor ? 2 : 3)); check();
		selected = apply(world.planResizeLocation(selected, 8, 1, 3, corridor ? 2 : 1)); check();
		// Replay initiated by another Location must retain both protected owners.
		apply(world.planRemoveLocation(earlier)); --selected; --unrelated; check();
		require(world.getAccessPermissionUsage(red).locationRequirements == 1
			&& world.getAccessPermissionUsage(blue).locationRequirements == 2, "Deletion retained stale usage");
		world.resetSimulation(); world.pauseSimulation(); check();
		require(world.renameAccessPermission(red, "Renamed")
			&& world.getAccessPermissionName(red) == "Renamed", "Rename refused"); check();
		// Exhaust the registry: deletion must make that exact capacity reusable.
		for (uint32_t i = 2; i < 256; ++i)
			require(static_cast<bool>(world.addAccessPermission("Extra " + std::to_string(i))), "Could not fill permission capacity");
		world.markSaved();
		auto full = save(world);
		bool refused = false;
		try { (void)world.addAccessPermission("Overflow"); }
		catch (std::invalid_argument const&) { refused = true; }
		require(refused && save(world) == full && !world.isModified(),
			"Exhausted registry mutation was not atomic");
		require(world.deleteAccessPermission(red), "Permission deletion refused");
		require(world.getLocationPermissionRequirement(selected) == std::vector<core::AccessPermissionId>{ blue }
			&& world.getLocationPermissionRequirement(unrelated) == std::vector<core::AccessPermissionId>{ blue },
			"Permission deletion lost unrelated references");
		auto reused = world.addAccessPermission("Replacement");
		require(reused == red && world.getAccessPermissionCount() == 256
			&& world.getAccessPermissionUsage(reused).locationRequirements == 0,
			"Freed permission slot inherited Location usage");
		roundTrip(world);
		apply(world.planRemoveLocation(selected)); --unrelated;
		require(world.getAccessPermissionUsage(blue).locationRequirements == 1
			&& world.getLocationPermissionRequirement(unrelated) == std::vector<core::AccessPermissionId>{ blue },
			"Location deletion changed unrelated requirement");
		auto recreated = add(world, corridor, 6);
		world.finishBuild(); world.pauseSimulation();
		require(world.getLocationPermissionRequirement(recreated).empty(), "Recreated Location inherited protection");
		require(world.setLocationPermissionRequirement(recreated, { reused }), "Reused permission was not usable");
		roundTrip(world);
	}

	void malformed(bool corridor)
	{
		core::World world("Transactional", 12, 3);
		auto owner = add(world, corridor, 0);
		world.addFacade("Facade", 0, 0, 4, 4, 2);
		world.addBackground(0, 0, 8, 4, 2);
		// A Transit owns a construction record too, but cannot own this requirement.
		world.addRoom("Landing", 0, 2, 0, 4, 1);
		world.addLadder(1, 0, 1, { 3, false, false, 1 });
		world.finishBuild(); world.pauseSimulation();
		auto key = world.addAccessPermission("Key");
		require(world.setLocationPermissionRequirement(owner, { key }), "Requirement refused");
		world.markSaved();
		auto before = save(world);
		std::string diagnostic;
		require(!world.isLocationPermissionEligible(4)
			&& !world.setLocationPermissionRequirement(4, { key }, &diagnostic)
			&& !diagnostic.empty() && save(world) == before && !world.isModified(),
			"Transit accepted a Location requirement through the public API");
		auto reject = [&](YAML::Node const& root)
		{
			core::SerializationWorkData work;
			bool refused = false;
			try
			{
				auto reader = core::YamlSerializer::fromString(YAML::Dump(root)); reader->deserialize();
				world.deserialize(*reader, work);
			}
			catch (core::SerializationException const& error) { refused = !std::string(error.what()).empty(); }
			require(refused && save(world) == before && !world.isModified()
				&& world.getLocationPermissionRequirement(owner) == std::vector<core::AccessPermissionId>{ key },
				"Malformed requirement partially changed destination World");
		};
		for (auto value : { "[1, 1]", "[0]", "[2]", "[256]", "[257]", "[4294967296]", "[-1]", "broken", "{}" })
		{
			auto root = YAML::Load(before);
			root["construction"][0]["locationPermissionRequirement"] = YAML::Load(value);
			reject(root);
		}
		for (auto record : { 1u, 2u, 4u })
			for (auto value : { "[]", "[1]" })
			{
				auto root = YAML::Load(before);
				root["construction"][record]["locationPermissionRequirement"] = YAML::Load(value);
				reject(root);
			}
		for (auto version : { 31u, 49u })
		{
			auto root = YAML::Load(before); root["version"] = version; reject(root);
		}
		for (auto version : { 31u, 32u })
		{
			auto root = YAML::Load(before); root["version"] = version;
			root["construction"][0].remove("locationPermissionRequirement");
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(YAML::Dump(root)); reader->deserialize();
			core::World restored("old", 1, 1);
			require(restored.deserialize(*reader, work) && restored.getLocationPermissionRequirement(owner).empty(),
				"Supported document without requirements became restricted");
		}
	}
}

void permission_smoke::registerLocationLifecycle(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "locationLifecycleRoom", [](smoke::Context const&) { lifecycle(false); } });
	checks.push_back({ "locationLifecycleCorridor", [](smoke::Context const&) { lifecycle(true); } });
	checks.push_back({ "locationLifecycleMalformedRoom", [](smoke::Context const&) { malformed(false); } });
	checks.push_back({ "locationLifecycleMalformedCorridor", [](smoke::Context const&) { malformed(true); } });
}
