// A window-carrying World's document is the document it wrote before the
// windowframe existed (#1075).
//
// A windowframe is not authored. It is derived at draw time from the live
// Window standing in the wall, which means the feature adds nothing to
// save - no field, no record, no per-cell entry. That is the whole reason
// the frame cannot drift out of step with the Window it frames, and it is
// also the thing worth pinning: a feature that looks like it changed the
// renderer should leave the document byte-for-byte alone, so a World saved
// by a build wearing frames reloads in a build that has never heard of
// them, and vice versa, with nothing to migrate and nothing lost in the
// middle.
//
// So the checks here are the absence and the round trip. The saved
// document names no frame, in either format. A World with a Window saves,
// reloads, and saves again to the same bytes, in YAML and in binary. And
// the Window itself survives with its own geometry intact, because the
// thing being checked is not that the document is empty but that the
// frame added nothing to a document that already had a Window in it.

#include "WorldChecks.h"

#include "core/BinarySerializer.h"
#include "core/Sector.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <memory>
#include <string>

namespace persistence
{
	using smoke::require;

	namespace
	{
		// The Window count read off the live World rather than its text,
		// so "the same document" is checked against the thing the document
		// describes and not only against itself.
		uint32_t windowCount(core::World const& world)
		{
			uint32_t count{};
			for (uint32_t index = 0; index < world.getNumSectors(); ++index)
			{
				auto const& sector = *world.getSector(index);
				for (uint32_t object = 0; object < sector.getNumObjects();
					++object)
				{
					auto const type = sector.getObject(object)->getObjectType();
					if (type == core::SectorObjectType::Window
						|| type == core::SectorObjectType::BoothWindow)
						++count;
				}
			}
			return count;
		}

		// A two-Layer World with one 1x1 Window in the front wall - the
		// smallest World that can carry a Window at all, since a Window
		// joins a Layer pair and needs a space behind to look into.
		std::shared_ptr<core::World> windowWorld()
		{
			auto world = std::make_shared<core::World>("Moulding "
				"persistence world", 12, 6);
			while (world->getLayerCount() < 2) world->addLayer();
			world->addRoom("Front", 0, 0, 0, 4, 1, CORE_ROOM_MAX_HEIGHT);
			world->addRoom("Back", 1, 0, 0, 4, 1, CORE_ROOM_MAX_HEIGHT);
			world->addSectorWindow(0, 0, 2, 1, 1);
			world->finishBuild();
			world->pauseSimulation();
			// The returned index is the object's slot and 0 is a valid
			// answer, so the fixture is checked by what it made rather
			// than by the number it handed back.
			require(windowCount(*world) == 2u,
				"The fixture World took no Window, so there is nothing here "
				"to save and the check would prove nothing");
			return world;
		}

		std::string serialize(core::World const& world, bool binary)
		{
			auto write = [&](auto writer) {
				core::SerializationWorkData work;
				work.markSerializedUnmodified = false;
				world.serialize(*writer, work);
				writer->serialize();
				return writer->getSerializedString();
			};
			return binary ? write(core::BinarySerializer::toString())
				: write(core::YamlSerializer::toString());
		}

		std::shared_ptr<core::World> load(std::string const& data, bool binary)
		{
			std::unique_ptr<core::Serializer> input = binary
				? std::unique_ptr<core::Serializer>(
					core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(
					core::YamlSerializer::fromString(data));
			input->deserialize();
			core::SerializationWorkData work;
			auto loaded = std::make_shared<core::World>("Reloaded", 1, 1);
			require(loaded->deserialize(*input, work),
				"The window-carrying document was refused on reload");
			// Deserialising finishes the World, so there is no second
			// finishBuild to call here - and calling one on a finished,
			// running World is refused outright.
			loaded->pauseSimulation();
			return loaded;
		}

		// Every ordinary Window's glass rect, as one string. The frame is
		// derived from this rect, so a reload that moved it would move the
		// moulding with it and the frame would stop lining up with the
		// glass it frames.
		std::string glassRects(core::World const& world)
		{
			std::string out;
			for (uint32_t index = 0; index < world.getNumSectors(); ++index)
			{
				auto const& sector = *world.getSector(index);
				for (uint32_t object = 0; object < sector.getNumObjects();
					++object)
				{
					auto const item = sector.getObject(object);
					if (!item
						|| item->getObjectType()
							!= core::SectorObjectType::Window)
						continue;
					auto const window = std::static_pointer_cast<
						core::WindowSectorObject>(item)->getWindow();
					if (!window) continue;
					core::Vector2 lo;
					core::Vector2 hi;
					window->getFullShape(lo, hi);
					out += sector.getName() + ":" + std::to_string(lo.x)
						+ "," + std::to_string(lo.y) + ","
						+ std::to_string(hi.x) + ","
						+ std::to_string(hi.y) + ";";
				}
			}
			return out;
		}
	}

	// The saved document names no frame. Nothing in either format records
	// a windowframe, because nothing about a frame is chosen: it follows
	// from the Window, so there is no decision for the document to keep.
	void aWindowDocumentCarriesNoWindowframe(smoke::Context const&)
	{
		auto const world = windowWorld();
		for (bool binary : { false, true })
		{
			auto const data = serialize(*world, binary);
			require(!data.empty(), "The window-carrying World saved nothing");
			// Folded to lower case and searched for the whole word family,
			// so `frame`, `windowFrame` and `window_frame` would all be
			// caught rather than slipping past one exact match.
			std::string lower;
			lower.reserve(data.size());
			for (auto const c : data)
				lower.push_back(static_cast<char>(
					(c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
			require(lower.find("frame") == std::string::npos,
				std::string("The ") + (binary ? "binary" : "YAML")
				+ " document names a frame; a windowframe is derived from "
				"the Window at draw time and has nothing to persist");
		}
	}

	// Saves and reloads identically. The round trip is byte-identical in
	// both formats, so a World that has been through a save and a reload
	// under this feature is the same World a frameless build would have
	// written, and can go back to one unchanged.
	void aWindowWorldRoundTripsIdentically(smoke::Context const&)
	{
		auto const world = windowWorld();
		// A Window registers on both Sectors of the pair it joins, so the
		// fixture's one Window reads as two. Stated here so the round trip
		// below is known to be comparing a World that has a Window in it
		// rather than one that quietly lost its only one.
		require(windowCount(*world) == 2u,
			"The fixture's one Window does not register on both Sectors of "
			"its pair, so the round trip has nothing to compare");
		for (bool binary : { false, true })
		{
			auto const first = serialize(*world, binary);
			auto const reloaded = load(first, binary);
			require(windowCount(*reloaded) == windowCount(*world),
				std::string("Reloading the window-carrying World in ")
				+ (binary ? "binary" : "YAML") + " changed how many Windows "
				"it has");
			auto const second = serialize(*reloaded, binary);
			require(first == second,
				std::string("A window-carrying World did not save the same "
					"bytes after a reload in ")
				+ (binary ? "binary" : "YAML") + "; the windowframe feature "
				"changed something the document carries");
		}
	}

	// The Window's own geometry survives, so the byte-identity above is
	// not the trivial one of a document that lost its Window on both
	// sides. The frame reads the Window's glass rect and its wall; those
	// have to come back where they were or the frame would draw somewhere
	// else after a reload.
	void theWindowSurvivesWithItsOwnGeometry(smoke::Context const&)
	{
		auto const world = windowWorld();
		auto const before = glassRects(*world);
		require(!before.empty(),
			"No ordinary Window was found to compare glass rects for, so "
			"the comparison below would compare two empty strings");

		auto const reloaded = load(serialize(*world, false), false);
		require(reloaded->getNumSectors() == world->getNumSectors(),
			"Reloading changed how many Sectors the World has");
		require(reloaded->getLayerCount() == world->getLayerCount(),
			"Reloading changed how many Layers the World has");
		require(before == glassRects(*reloaded),
			"The Window's glass rect did not survive the round trip; the "
			"windowframe is derived from that rect and would draw out of "
			"step with its glass");
	}
}
