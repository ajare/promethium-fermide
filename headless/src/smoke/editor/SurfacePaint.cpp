// Painting a variant onto a single cell (#1042).
//
// Pick a swatch, click a cell, that cell wears the variant. The base swatch
// clears it back. A cell of the wrong Sector type refuses, and so does a
// cell that is not in any Sector of that type at all.
//
// Every check here drives the authoring seam rather than the ImGui widgets.
// The widgets have no state of their own to be wrong: the mode the tool is
// in, the resolve that says what a click would do, the World edit that
// records or removes an entry, and the renderer that draws the result are
// all production code the tray calls, and a check that goes round them by
// another route would be testing its own route.
//
// What is asserted here:
//
//   * picking any swatch, the base included, moves the tool out of create
//     and into surface-paint, and a tool with nothing picked is in create;
//   * clicking the swatch that is already picked deselects it and the tool
//     falls back to create, while clicking a *different* swatch moves the
//     pick rather than dropping it;
//   * a cell of the matching Sector type takes the variant and the real
//     surface pass draws the variant tile on that cell and the base on
//     every other one, with no rebuild in between;
//   * the base swatch removes the entry entirely - the live Sector holds no
//     entry and the saved document writes no field, which is what "not a
//     stored zero" means where anyone can check it;
//   * a wrong-type cell, a cell with no Sector, a flat-coloured Sector and
//     a cell off the map are all refused in the same shape, and none of
//     them changes the document or takes an undo step;
//   * one undo step per accepted click, and undoing restores the document
//     byte for byte;
//   * an accepted click that changes nothing - the base swatch on a cell
//     with no entry, a variant swatch on the cell already wearing that
//     variant - is accepted rather than refused, and spends no snapshot,
//     leaves no undo step and leaves the document unmodified (#1051);
//   * create mode is untouched: with the strip on screen and nothing
//     picked, the create path still builds Sectors and puts no variant
//     entry on anything.

#include "Checks.h"
#include "State.h"

#include "DocumentEdit.h"
#include "EditorDocument.h"
#include "PaintTool.h"
#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "SectorVariantStrip.h"
#include "SurfacePaint.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "game/WorldAuthoring.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern UISettings gUISettings;

namespace
{
	using smoke::require;

	char const* const kTileset{ "resources/textures/sectors-new.tileset.yaml" };

	// Closes whatever document the check opened, so a later check in the
	// same process never reads a World this one left behind.
	struct CloseOnExit
	{
		~CloseOnExit() { editor::closeDocument(); }
	};

	// Installs the authored new-family sheet the editor ships with, so the
	// swatches, the refusal rules and the drawn tiles all read the same real
	// artwork: `room` and its three declared variants are on the sheet.
	SectorTileset installAuthoredAtlas(smoke::Context const& context)
	{
		auto const atlas = SectorTileset::load(context.fixture(kTileset));
		require(atlas.family == SectorTilesetFamily::New,
			"The authored new set did not install as the New family");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorSurfaceVariantCount(drawingSectorSheet(), "room") == 3,
			"The installed atlas does not report the family the file declared");
		return atlas;
	}

	// The viewport pinned to zero offsets and unit zoom, so a drawn quad's
	// screen rect is plain arithmetic on the World cell it covers and
	// nothing a previous check left in the shared settings can move it.
	void pinViewport()
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 4096.0f;
		gUISettings.worldViewportHeight = 4096.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.worldZoom = 1.0f;
	}

	// Every paint tool that paints a tiled surface, recovered from the
	// mapping itself rather than written out by hand, so a tool added to the
	// enum is covered by these checks without being added to them again.
	std::vector<std::pair<PaintTool, std::string>> surfaceTools()
	{
		std::vector<std::pair<PaintTool, std::string>> tools;
		for (int index = 0; index < static_cast<int>(PaintTool::Count); ++index)
		{
			auto const tool = static_cast<PaintTool>(index);
			auto const* mapped = sectorKindForPaintTool(tool);
			if (mapped) tools.emplace_back(tool, std::string{ mapped });
		}
		require(tools.size() == 10,
			"Expected the ten surface-bearing paint tools, found "
				+ std::to_string(tools.size()));
		return tools;
	}

	std::string describe(std::vector<core::SectorSurfaceVariant> const& entries)
	{
		std::string out{ "[" };
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			if (index) out += ", ";
			out += std::to_string(entries[index].column) + ","
				+ std::to_string(entries[index].level) + "->"
				+ std::to_string(entries[index].variant);
		}
		return out + "]";
	}

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// ------------------------------------------------------------------
	// The document under test.
	//
	// One Room, one Corridor and one Background on Layer 0, built through
	// the authoring seam. The Room is what the swatch paints; the Corridor
	// is the wrong type that is still a Sector; the Background is the
	// Sector that draws no tiled surface.
	// ------------------------------------------------------------------

	struct Fixture
	{
		editor::DocumentPtr document{};
		uint32_t roomIndex{ 0 };
		uint32_t corridorIndex{ 0 };
		uint32_t backgroundIndex{ 0 };

		// The Room spans cells x 1..3, y 2..3. Its floor line is y 2, so
		// level 0 is the row at y 2 and column 0 the column at x 1.
		static constexpr uint32_t roomX{ 1 };
		static constexpr uint32_t roomY{ 2 };
		static constexpr uint32_t roomWide{ 3 };
		static constexpr uint32_t roomHigh{ 2 };
		// The Corridor spans x 1..4 at y 1, one level high.
		static constexpr uint32_t corridorX{ 1 };
		static constexpr uint32_t corridorY{ 1 };
		static constexpr uint32_t corridorWide{ 4 };
		// The Background spans x 8..9 at y 3.
		static constexpr uint32_t backgroundX{ 8 };
		static constexpr uint32_t backgroundY{ 3 };

		core::World& world() const { return *document->world(); }
		core::World const& documentWorld() const { return *document->world(); }
		game::WorldAuthoring& authoring() const { return document->authoring(); }

		std::shared_ptr<const core::Sector> sector(uint32_t index) const
		{
			return documentWorld().getSector(index);
		}
	};

	Fixture makeFixture(bool withCorridor, bool withBackground)
	{
		require(editor::currentDocument() != nullptr,
			"opening a World left the editor with no document");
		Fixture fixture;
		fixture.document = editor::currentDocument();
		auto& authoring = fixture.authoring();
		authoring.world().pauseSimulation();
		fixture.roomIndex = authoring.addRoom("Room", 0, Fixture::roomY,
			Fixture::roomX, Fixture::roomWide, Fixture::roomHigh);
		if (withCorridor)
			fixture.corridorIndex = authoring.addCorridor(0, Fixture::corridorY,
				Fixture::corridorX, Fixture::corridorWide, 1);
		if (withBackground)
			fixture.backgroundIndex = authoring.addBackground(0,
				Fixture::backgroundY, Fixture::backgroundX, 2, 1);
		authoring.finishBuild();
		require(fixture.sector(fixture.roomIndex)->getCellsWide()
				== Fixture::roomWide
				&& fixture.sector(fixture.roomIndex)->getLevelsHigh()
					== Fixture::roomHigh,
			"The fixture Room is not the size it was authored as");
		if (withCorridor)
			require(fixture.sector(fixture.corridorIndex)->getCellsWide()
					== Fixture::corridorWide,
				"The fixture Corridor is not the width it was authored as");
		return fixture;
	}

	// The editor's own edit transaction over one surface-paint click:
	// resolve the click, capture the state it is made from, apply through
	// the authoring seam, and commit what was captured only if the write
	// changed something. A click the resolve refuses spends no snapshot and
	// commits nothing, which is why a refusal costs the author no undo step
	// (#1042). A click that was accepted and changed nothing captures a
	// snapshot and drops it, and costs the same nothing (#1051): the press
	// is legal, the ground was already painted, and an undo step for it
	// would be a press that visibly does nothing.
	struct PaintOutcome
	{
		bool applied{ false };
		bool refused{ false };
		// Whether the accepted write left the cell holding anything
		// different. Never true without `applied`.
		bool changed{ false };
		bool committed{ false };
		std::string diagnostic;
	};

	PaintOutcome paintClick(Fixture& fixture, PaintTool tool,
		SectorSwatchSelection const& swatches, uint32_t layer, uint32_t x,
		uint32_t y)
	{
		PaintOutcome outcome;
		auto const target = resolveSurfacePaint(fixture.documentWorld(), tool,
			swatches, layer, x, y);
		if (!target.ok)
		{
			outcome.refused = true;
			outcome.diagnostic = target.diagnostic;
			return outcome;
		}
		auto const before = captureDocumentSnapshot(fixture.document->world(),
			fixture.authoring().history());
		require(before.has_value(),
			"the state a paint click is made from could not be captured");
		std::string diagnostic;
		auto const write = fixture.authoring().setSectorSurfaceVariant(
			target.sectorIndex, target.column, target.level, target.variant,
			&diagnostic);
		outcome.applied = write.accepted;
		outcome.changed = write.changed;
		if (write.accepted && write.changed)
		{
			commitDocumentEdit(std::move(before), fixture.authoring().history());
			outcome.committed = true;
		}
		outcome.diagnostic = diagnostic;
		return outcome;
	}

	// The editor's own undo, walked through the authoring seam: rebuild the
	// snapshot into a World of its own, adopt it, and carry the document's
	// history across the handover (#971, #975). The fixture's document
	// handle is rebound, because the undo really does replace it.
	void undoOnce(Fixture& fixture)
	{
		auto const carried = fixture.document;
		auto& authoring = carried->authoring();
		require(authoring.canUndo(), "there was nothing to undo");
		auto const current = captureDocumentSnapshot(carried->world(),
			authoring.history());
		require(current.has_value(),
			"the state being left could not be captured for the undo");
		std::unique_ptr<core::World> rebuilt;
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			rebuilt = deserializeDocumentSnapshot(snapshot, carried->world(), {});
			return rebuilt != nullptr;
		};
		require(authoring.undo(current, restore), "the undo was refused");
		editor::adoptWorld(std::move(rebuilt));
		fixture.document = editor::currentDocument();
		require(fixture.document != nullptr,
			"the adoption left the editor with no document");
		fixture.document->authoring().adoptHistoryFrom(authoring);
	}

	// ------------------------------------------------------------------
	// What the real surface pass drew.
	// ------------------------------------------------------------------

	// The tile a textured triangle samples, by the centre of its UV range.
	// A crop moves the corner of that range but cannot move its centre off
	// the tile it came from, and the sheet's tiles are disjoint.
	std::string tileSampled(WorldDrawList::Triangle const& triangle,
		SectorTileset const& atlas)
	{
		float left = 1.0f, right = 0.0f, top = 1.0f, bottom = 0.0f;
		for (auto const& uv : triangle.texcoords)
		{
			left = std::min(left, uv.x);
			right = std::max(right, uv.x);
			top = std::min(top, uv.y);
			bottom = std::max(bottom, uv.y);
		}
		auto const u = (left + right) * 0.5f;
		auto const v = (top + bottom) * 0.5f;
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const tileLeft = float(region.x) / float(atlas.width);
			auto const tileRight = float(region.x + region.width) / float(atlas.width);
			auto const tileTop = float(region.y) / float(atlas.height);
			auto const tileBottom = float(region.y + region.height) / float(atlas.height);
			if (u >= tileLeft && u < tileRight && v >= tileTop && v < tileBottom)
				return name;
		}
		return {};
	}

	// Records the production `renderSector` pass over a live Sector and
	// pairs each surface cell with the tile that was drawn on it. This is
	// the renderer's own mapping - x scales by the cell width, y by the
	// Level height and flips, the viewport offsets are added - inverted at
	// unit zoom with the offsets pinned to zero, so the cell a quad covers
	// is read straight off where it was drawn. The two triangles one cell
	// emits land on the same cell and resolve to the same tile, which is
	// asserted rather than assumed.
	std::map<std::pair<uint32_t, uint32_t>, std::string> drawSurface(
		std::shared_ptr<const core::Sector> sector, uint32_t layer,
		SectorTileset const& atlas)
	{
		WorldDrawList commands{ WorldDrawList::ClipRectangle{
			{ -100000.0f, -100000.0f }, { 100000.0f, 100000.0f } } };
		renderSector(sector, layer, LayerRenderStyle::Solid, false, White,
			&commands);

		auto const cellWidth = float(CORE_CELL_WIDTH_PIXELS);
		auto const levelHeight = float(CORE_LEVEL_HEIGHT_PIXELS);
		auto const viewportHeight = gUISettings.worldViewportHeight;

		std::map<std::pair<uint32_t, uint32_t>, std::string> drawn;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle
					|| triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
			for (auto const& point : triangle->positions)
			{
				left = std::min(left, point.x);
				right = std::max(right, point.x);
				top = std::min(top, point.y);
				bottom = std::max(bottom, point.y);
			}
			auto const key = std::make_pair(
				uint32_t((left + right) * 0.5f / cellWidth),
				// Screen y runs down from the viewport top; World y runs up.
				uint32_t((viewportHeight - (top + bottom) * 0.5f) / levelHeight));
			auto const tile = tileSampled(*triangle, atlas);
			auto const placed = drawn.emplace(key, tile);
			require(placed.second || placed.first->second == tile,
				"The two triangles of one surface cell resolved to different tiles at "
					+ std::to_string(key.first) + ", " + std::to_string(key.second));
		}
		return drawn;
	}

	// The tile the pass drew over one World cell. A cell it drew nothing for
	// answers empty, which no expected tile name matches.
	std::string tileDrawnAt(
		std::map<std::pair<uint32_t, uint32_t>, std::string> const& drawn,
		uint32_t cellX, uint32_t cellY)
	{
		auto const found = drawn.find({ cellX, cellY });
		return found == drawn.end() ? std::string{} : found->second;
	}

	// ------------------------------------------------------------------
	// The mode the tool is in.
	// ------------------------------------------------------------------

	// Any swatch of a surface tool's strip, the base included, puts the
	// tool into surface-paint. Nothing picked leaves it in create, and a
	// tool with no strip of its own never leaves create at all.
	void pickingAnySwatchSwitchesTheToolIntoSurfacePaint(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		for (auto const& [tool, kind] : surfaceTools())
		{
			SectorSwatchSelection swatches{ sectorVariantSwatches(tool) };
			require(!swatches.empty(),
				kind + "'s strip arrived empty, so there was nothing to pick");
			require(paintModeOf(tool, swatches) == PaintMode::Create,
				kind + "'s tool was not in create before a swatch was picked");
			for (int index = 0; index < int(swatches.size()); ++index)
			{
				require(swatches.pick(index),
					"Picking " + kind + "'s swatch " + std::to_string(index)
						+ " was refused although the strip holds it");
				require(paintModeOf(tool, swatches) == PaintMode::SurfacePaint,
					"Picking " + kind + "'s swatch " + std::to_string(index)
						+ " (" + swatches.swatches()[size_t(index)].tileName
						+ ") left the tool out of surface-paint");
			}
			// The base is the swatch that matters most here: it is a swatch
			// like any other, and picking it switches just the same.
			require(swatches.pick(0)
					&& swatches.selectedVariant().value_or(-1) == 0,
				kind + "'s swatch 0 is not the base");
			require(paintModeOf(tool, swatches) == PaintMode::SurfacePaint,
				"Picking " + kind + "'s base swatch left the tool out of "
				"surface-paint");
		}

		// A tool with no surface kind has no strip and so no way into
		// surface-paint, and no tool at all is create rather than a third
		// state nothing can paint with.
		SectorSwatchSelection nothing;
		require(paintModeOf(PaintTool::None, nothing) == PaintMode::Create,
			"No tool armed reported a mode other than create");
		for (auto const tool : { PaintTool::Background, PaintTool::Facade })
		{
			SectorSwatchSelection flat{ sectorVariantSwatches(tool) };
			require(flat.empty(),
				"A flat-coloured tool grew a strip to pick from");
			require(paintModeOf(tool, flat) == PaintMode::Create,
				"A tool with no strip of its own switched into surface-paint");
		}
	}

	// Clicking the swatch that is already picked deselects it and the tool
	// falls back to create. Clicking a different swatch moves the pick
	// instead, and a click on a swatch that is not on the strip changes
	// nothing at all.
	void deselectingTheSwatchReturnsTheToolToCreate(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Corridor) };
		require(swatches.size() == 4,
			"The Corridor strip is not the base plus its three variants");

		require(swatches.toggle(2), "The first click on a swatch did not pick it");
		require(paintModeOf(PaintTool::Corridor, swatches)
				== PaintMode::SurfacePaint,
			"Picking a swatch left the tool out of surface-paint");
		require(!swatches.toggle(2),
			"Clicking the already-picked swatch did not deselect it");
		require(!swatches.hasSelection(),
			"Deselecting left the highlight lit");
		require(paintModeOf(PaintTool::Corridor, swatches) == PaintMode::Create,
			"Deselecting the swatch did not return the tool to create");

		// The base deselects the same way. It is a swatch, and the ticket
		// says picking it means the base tile, not "stay picked forever".
		require(swatches.toggle(0), "Clicking the base swatch did not pick it");
		require(paintModeOf(PaintTool::Corridor, swatches)
				== PaintMode::SurfacePaint,
			"Picking the base swatch left the tool out of surface-paint");
		require(!swatches.toggle(0),
			"Clicking the already-picked base swatch did not deselect it");
		require(paintModeOf(PaintTool::Corridor, swatches) == PaintMode::Create,
			"Deselecting the base swatch did not return the tool to create");

		// A different swatch moves the pick rather than dropping it.
		require(swatches.toggle(1), "Clicking a swatch did not pick it");
		require(swatches.toggle(3), "Clicking another swatch was refused");
		require(swatches.isSelected(3) && !swatches.isSelected(1),
			"Clicking a different swatch deselected instead of moving");
		require(paintModeOf(PaintTool::Corridor, swatches)
				== PaintMode::SurfacePaint,
			"Moving the pick dropped the tool out of surface-paint");

		// A click outside the strip is refused and changes nothing - not the
		// pick, and not the mode.
		require(!swatches.toggle(4),
			"A click past the end of the strip was accepted");
		require(!swatches.toggle(-1), "A click before the strip was accepted");
		require(swatches.isSelected(3),
			"A refused click moved the pick");
		require(paintModeOf(PaintTool::Corridor, swatches)
				== PaintMode::SurfacePaint,
			"A refused click changed the tool's mode");
	}

	// ------------------------------------------------------------------
	// What a matching click records, and what gets drawn.
	// ------------------------------------------------------------------

	// A cell of the matching Sector type takes the variant, and the real
	// surface pass draws the variant tile on that cell and the base on every
	// other one. No rebuild and no reload sits between the click and the
	// picture: the renderer reads the live Sector, which is why the edit
	// writes the live Sector as well as the record.
	void aMatchingCellRecordsTheVariantAndItRendersImmediately(
		smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		pinViewport();
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint match", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, false);

		// Before: every cell of the Room draws the base tile.
		auto const before = drawSurface(fixture.sector(fixture.roomIndex), 0,
			atlas);
		require(before.size() == Fixture::roomWide * Fixture::roomHigh,
			"The Room drew " + std::to_string(before.size())
				+ " surface cells for a grid of "
				+ std::to_string(Fixture::roomWide) + " by "
				+ std::to_string(Fixture::roomHigh));
		for (uint32_t level = 0; level < Fixture::roomHigh; ++level)
			for (uint32_t column = 0; column < Fixture::roomWide; ++column)
				require(tileDrawnAt(before, Fixture::roomX + column,
						Fixture::roomY + level) == "room",
					"An unpainted Room cell did not draw its base tile at column "
					+ std::to_string(column) + ", level "
					+ std::to_string(level));

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(2), "Picking room-V2 was refused");
		// Cell (2, 3) is column 1, level 1 of a Room anchored at (1, 2).
		auto const target = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 2, 3);
		require(target.ok,
			"A Room cell refused the Room's own variant: " + target.diagnostic);
		require(target.sectorIndex == fixture.roomIndex,
			"The click resolved to a different Sector than the Room");
		require(target.column == 1 && target.level == 1,
			"The click resolved to column " + std::to_string(target.column)
				+ ", level " + std::to_string(target.level)
				+ "; cell (2, 3) of a Room anchored at (1, 2) is column 1, level 1");
		require(target.variant.value_or(99) == 2,
			"The click did not carry the picked variant");

		auto const outcome = paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3);
		require(outcome.applied,
			"The paint click was not applied: " + outcome.diagnostic);
		require(fixture.sector(fixture.roomIndex)->surfaceVariantAt(1, 1).value_or(99)
				== 2,
			"The live Sector does not answer the variant that was just painted");

		// And the picture follows without anything being rebuilt.
		auto after = drawSurface(fixture.sector(fixture.roomIndex), 0, atlas);
		require(tileDrawnAt(after, 2, 3) == "room-V2",
			"The painted cell drew '" + tileDrawnAt(after, 2, 3)
				+ "'; it should draw room-V2");
		for (uint32_t level = 0; level < Fixture::roomHigh; ++level)
			for (uint32_t column = 0; column < Fixture::roomWide; ++column)
			{
				if (column == 1 && level == 1) continue;
				require(tileDrawnAt(after, Fixture::roomX + column,
						Fixture::roomY + level) == "room",
					"Painting one cell changed what another cell draws, at column "
					+ std::to_string(column) + ", level "
					+ std::to_string(level));
			}
	}

	// The base swatch removes the entry rather than storing a zero. The
	// live Sector holds no entry for the cell, and the saved document writes
	// no `surfaceVariants` field at all - which is the same shape a Room
	// that never used variants saves in, and the only proof that nothing
	// zero-valued was left behind for a later reader to find.
	void theBaseSwatchRemovesTheEntryEntirely(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint base clears", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, false);

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(1), "Picking room-V1 was refused");
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).applied,
			"Painting room-V1 was refused");
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().size() == 1,
			"The painted Room did not hold exactly one entry: "
				+ describe(fixture.sector(fixture.roomIndex)->getSurfaceVariants()));

		// The base swatch, picked and clicked on the painted cell.
		require(swatches.pick(0), "Picking the base swatch was refused");
		auto const target = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 2, 3);
		require(target.ok,
			"The base swatch refused a cell the variant swatch painted: "
				+ target.diagnostic);
		require(!target.variant.has_value(),
			"The base swatch resolved to a variant number instead of a clearing "
			"request");

		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).applied,
			"The base click was refused");
		auto const& entries = fixture.sector(fixture.roomIndex)
			->getSurfaceVariants();
		require(entries.empty(),
			"The base click left entries on the Sector: " + describe(entries));
		require(!fixture.sector(fixture.roomIndex)->surfaceVariantAt(1, 1)
				.has_value(),
			"The cleared cell still answers a variant");

		// Saved, the document says nothing about variants at all. A stored
		// zero would show up here as a field, and a field is exactly what
		// "not a zero variant" means there is not.
		auto const saved = serializeWorld(fixture.documentWorld());
		require(saved.find("surfaceVariants") == std::string::npos,
			"A cleared cell still wrote a surfaceVariants field into the document");
	}

	// ------------------------------------------------------------------
	// What refuses.
	// ------------------------------------------------------------------

	// A cell of the wrong Sector type is refused, the hover gate says so,
	// and the click leaves the World exactly as it was.
	void aCellOfTheWrongSectorTypeIsRefusedAndChangesNothing(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint wrong type", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(true, false);
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const undoDepth = fixture.authoring().undoDepth();

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(1), "Picking room-V1 was refused");

		for (uint32_t column = 0; column < Fixture::corridorWide; ++column)
		{
			auto const x = Fixture::corridorX + column;
			auto const refused = resolveSurfacePaint(fixture.documentWorld(),
				PaintTool::Room, swatches, 0, x, Fixture::corridorY);
			require(!refused.ok,
				"A Corridor cell accepted a Room variant at x "
					+ std::to_string(x));
			require(!refused.diagnostic.empty(),
				"The refusal carried no reason for the hover to show");
			require(refused.diagnostic.find("corridor") != std::string::npos
					&& refused.diagnostic.find("room") != std::string::npos,
				"The refusal does not name both types so the author can see "
				"which end was wrong: " + refused.diagnostic);
			require(!canSurfacePaint(fixture.documentWorld(), PaintTool::Room,
					swatches, 0, x, Fixture::corridorY),
				"The hover gate said a refused cell could be painted");
		}

		// The same refusal the other way round: a Corridor swatch over a
		// Room cell.
		SectorSwatchSelection corridorSwatches{
			sectorVariantSwatches(PaintTool::Corridor) };
		require(corridorSwatches.pick(1), "Picking corridor-V1 was refused");
		require(!canSurfacePaint(fixture.documentWorld(), PaintTool::Corridor,
				corridorSwatches, 0, Fixture::roomX, Fixture::roomY),
			"A Room cell accepted a Corridor swatch");

		require(paintClick(fixture, PaintTool::Room, swatches, 0,
				Fixture::corridorX + 1, Fixture::corridorY).refused,
			"The refused click was applied anyway");
		require(fixture.sector(fixture.corridorIndex)->getSurfaceVariants()
				.empty(),
			"The refused click put an entry on the Corridor: "
				+ describe(fixture.sector(fixture.corridorIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().empty(),
			"The refused click put an entry on the Room: "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"A refused click changed the document");
		require(fixture.authoring().undoDepth() == undoDepth,
			"A refused click took an undo step");
	}

	// A cell outside any Sector of that type is refused the same way: the
	// same shape of answer, the same nothing-happens, the same red. Empty
	// space, a Sector that draws no tiled surface, and a cell off the map
	// are all refused rather than one of them being a crash or a silent
	// success.
	void aCellOutsideAnySectorOfThatTypeIsRefusedTheSameWay(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint outside", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, true);
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const undoDepth = fixture.authoring().undoDepth();

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(1), "Picking room-V1 was refused");

		// Empty space on the Layer.
		auto const empty = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 10, 0);
		require(!empty.ok && !empty.diagnostic.empty(),
			"Empty space accepted a Room variant");
		require(empty.diagnostic.find("no Sector") != std::string::npos,
			"The refusal for empty space does not say there is no Sector: "
				+ empty.diagnostic);

		// A Background: a Sector that draws a tile of its own like any
		// wall, but one with no variant family to wear. The refusal names
		// what the cell really is, which is the fact the author needs.
		auto const background = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, Fixture::backgroundX,
			Fixture::backgroundY);
		require(!background.ok && !background.diagnostic.empty(),
			"A Background accepted a Room variant");
		require(background.diagnostic.find("background") != std::string::npos,
			"The refusal for a Background does not name what the cell is: "
				+ background.diagnostic);

		// Off the map entirely, and on a Layer that does not exist.
		auto const offmap = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 99, 99);
		require(!offmap.ok && !offmap.diagnostic.empty(),
			"A cell outside the World accepted a Room variant");
		auto const noLayer = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 99, 2, 3);
		require(!noLayer.ok && !noLayer.diagnostic.empty(),
			"A Layer this World does not have accepted a Room variant");

		// Every one of them refused the same way: nothing painted, nothing
		// changed, no undo step spent.
		for (auto const& cell : std::vector<std::pair<uint32_t, uint32_t>>{
			{ 10, 0 }, { Fixture::backgroundX, Fixture::backgroundY },
			{ 99, 99 } })
		{
			require(paintClick(fixture, PaintTool::Room, swatches, 0, cell.first,
					cell.second).refused,
				"A refused cell was applied anyway at " + std::to_string(cell.first)
					+ ", " + std::to_string(cell.second));
		}
		require(paintClick(fixture, PaintTool::Room, swatches, 99, 2, 3).refused,
			"A click on a Layer that does not exist was applied");
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().empty(),
			"A refused click put an entry on the Room: "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.backgroundIndex)->getSurfaceVariants()
				.empty(),
			"A refused click put an entry on the Background: "
				+ describe(fixture.sector(fixture.backgroundIndex)
					->getSurfaceVariants()));
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"A refused click changed the document");
		require(fixture.authoring().undoDepth() == undoDepth,
			"A refused click took an undo step");
	}

	// ------------------------------------------------------------------
	// Undo.
	// ------------------------------------------------------------------

	// One undo step per accepted click, and undoing restores the previous
	// state exactly - the whole document, byte for byte, not just the cell
	// that was painted.
	void oneUndoStepPerClickAndUndoRestoresExactly(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint undo", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, false);
		fixture.authoring().markSaved();

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(1), "Picking room-V1 was refused");

		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		// Click one: one step, and the entry is on.
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).applied,
			"The first paint click was refused");
		require(fixture.authoring().undoDepth() == baseline + 1,
			"One paint click put "
				+ std::to_string(fixture.authoring().undoDepth() - baseline)
				+ " steps on the undo stack instead of one");
		require(fixture.sector(fixture.roomIndex)->surfaceVariantAt(1, 1).value_or(99)
				== 1,
			"The painted entry is not what was clicked");

		// Undo: exactly the state before the click, and nothing else moved.
		undoOnce(fixture);
		require(fixture.authoring().undoDepth() == baseline,
			"The undo left the stack at a depth other than where the click found it");
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().empty(),
			"The undo kept the painted entry: "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"The undo did not restore the document exactly");

		// Click two: the same click again is one more step, and undoing it
		// lands on the same pristine state rather than somewhere near it.
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).applied,
			"The second paint click was refused");
		require(fixture.authoring().undoDepth() == baseline + 1,
			"A repeated paint click did not put exactly one step back on the stack");
		undoOnce(fixture);
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"Undoing the repeated click did not restore the same pristine state");

		// A base-clear click is a step of its own, and undoing it puts the
		// painted entry back rather than clearing twice.
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).applied,
			"Painting before the clear was refused");
		require(swatches.pick(0), "Picking the base swatch was refused");
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).applied,
			"The base-clear click was refused");
		require(fixture.authoring().undoDepth() == baseline + 2,
			"Paint then clear did not put two steps on the stack, got "
				+ std::to_string(fixture.authoring().undoDepth() - baseline));
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().empty(),
			"The clear left entries behind: "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));
		undoOnce(fixture);
		require(fixture.sector(fixture.roomIndex)->surfaceVariantAt(1, 1).value_or(99)
				== 1,
			"Undoing the clear did not put the painted variant back");
		undoOnce(fixture);
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"Undoing both clicks did not restore the document exactly");
		require(fixture.authoring().undoDepth() == baseline,
			"Both undos left the stack away from where the clicks found it");
	}

	// ------------------------------------------------------------------
	// Accepted, and nothing changed.
	//
	// #1042 named two cases: refused, which spends no snapshot, and
	// accepted, which takes one. #1051 names the third, which is neither.
	// The request is legal, so the hover must not go red over it; the write
	// leaves the cell holding exactly what it held, so the press must not
	// leave a step behind. A check that only ever clicks a cell that needs
	// the paint never drives this, and what it lets through is a press the
	// author has to undo through and a document dirtied by a gesture that
	// authored nothing.
	// ------------------------------------------------------------------

	// The base swatch on a cell that has no entry. There is nothing to
	// clear, the seam says so instead of writing an empty change, and the
	// author's history is no longer for it.
	void clearingACellThatHasNothingToClearIsAcceptedAndChangesNothing(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint clear empty", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, false);
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(0), "Picking the base swatch was refused");
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().empty(),
			"The fixture Room did not start with nothing to clear: "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));

		// The hover reads this as accepted, not as a refusal. "Already the
		// base" is not a reason to colour a cell red; only a cell that
		// cannot wear the swatch is.
		require(canSurfacePaint(fixture.documentWorld(), PaintTool::Room,
				swatches, 0, 2, 3),
			"A cell with nothing to clear failed the hover gate, so the "
			"affordance cannot tell 'already base' from 'wrong Sector type'");

		auto const outcome = paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3);
		require(outcome.applied && !outcome.refused,
			"Clearing an empty cell was refused: " + outcome.diagnostic);
		require(!outcome.changed,
			"Clearing a cell with no entry reported a change");
		require(!outcome.committed,
			"Clearing a cell with no entry committed an edit");
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().empty(),
			"Clearing an empty cell put entries on the Room: "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));
		require(fixture.authoring().undoDepth() == baseline,
			"Clearing a cell with no entry took an undo step, leaving a press "
			"that undoes nothing");
		require(!fixture.authoring().isModified(),
			"A no-op clear marked the document modified");
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"A no-op clear changed the document");
	}

	// Repainting the variant a cell already wears. Accepted, and nothing
	// written - including when the cell's entry is not the last in the
	// list, which is where a write that erased the entry and pushed a new
	// one on the end would turn an unchanged cell into a shuffled list, a
	// dirty document and a spent undo step.
	void repaintingTheVariantAlreadyWornIsAcceptedAndChangesNothing(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint repaint", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, false);

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };

		// Two entries, so the cell repainted below is the first of them and
		// not the last. Cell (1, 2) is column 0, level 0; cell (3, 3) is
		// column 2, level 1.
		require(swatches.pick(1), "Picking room-V1 was refused");
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 1, 2).changed,
			"The first room-V1 paint reported no change");
		require(swatches.pick(2), "Picking room-V2 was refused");
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 3, 3).changed,
			"The first room-V2 paint reported no change");
		auto const painted = describe(fixture.sector(fixture.roomIndex)
			->getSurfaceVariants());
		require(fixture.sector(fixture.roomIndex)->getSurfaceVariants().size() == 2,
			"The two paints did not leave two entries: " + painted);
		fixture.authoring().markSaved();
		auto const baseline = fixture.authoring().undoDepth();
		auto const saved = serializeWorld(fixture.documentWorld());

		// Repaint the first entry with the variant it already wears.
		require(swatches.pick(1), "Picking room-V1 again was refused");
		require(canSurfacePaint(fixture.documentWorld(), PaintTool::Room,
				swatches, 0, 1, 2),
			"A cell already wearing the picked variant failed the hover gate");
		auto const outcome = paintClick(fixture, PaintTool::Room, swatches, 0, 1, 2);
		require(outcome.applied && !outcome.refused,
			"Repainting a cell the variant it wears was refused: "
				+ outcome.diagnostic);
		require(!outcome.changed,
			"Repainting room-V1 on a cell already wearing room-V1 reported a "
			"change");
		require(!outcome.committed, "A repaint committed an edit");
		require(describe(fixture.sector(fixture.roomIndex)
				->getSurfaceVariants()) == painted,
			"The repaint moved entries in the list: was " + painted + ", now "
				+ describe(fixture.sector(fixture.roomIndex)
					->getSurfaceVariants()));
		require(fixture.authoring().undoDepth() == baseline,
			"A repaint took an undo step, leaving a press that undoes nothing");
		require(!fixture.authoring().isModified(),
			"A repaint marked the document modified");
		require(serializeWorld(fixture.documentWorld()) == saved,
			"A repaint changed the saved document");

		// The control: the same cell, a variant it does not wear, is a
		// change and exactly one step - the nothing above is the rule about
		// unchanged writes and not a rule about this cell.
		require(swatches.pick(3), "Picking room-V3 was refused");
		auto const control = paintClick(fixture, PaintTool::Room, swatches, 0, 1, 2);
		require(control.applied && control.changed && control.committed,
			"Painting a different variant over an occupied cell reported no "
			"change: " + control.diagnostic);
		require(fixture.authoring().undoDepth() == baseline + 1,
			"The control paint did not put exactly one step on the stack, got "
				+ std::to_string(fixture.authoring().undoDepth() - baseline));
		require(fixture.authoring().isModified(),
			"A paint that changed a cell left the document unmodified");
	}

	// Refusal and accepted-but-unchanged are two answers, and the two are
	// not interchangeable at either end: the hover reads `accepted` and must
	// still go red for the cell that cannot wear the swatch and stay yellow
	// for the one that already does, while the transaction reads `changed`
	// and must spend nothing on either. Collapsing them into one bool loses
	// one of those decisions whichever way the bool is read.
	void refusalAndAcceptedButUnchangedAreNotTheSameAnswer(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint two answers", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(true, true);
		fixture.authoring().markSaved();

		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.pick(1), "Picking room-V1 was refused");
		require(paintClick(fixture, PaintTool::Room, swatches, 0, 2, 3).changed,
			"The setup paint was refused");
		fixture.authoring().markSaved();
		auto const baseline = fixture.authoring().undoDepth();
		auto const saved = serializeWorld(fixture.documentWorld());

		// Accepted, unchanged: the cell already wears room-V1.
		auto const already = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 2, 3);
		require(already.ok && canSurfacePaint(fixture.documentWorld(),
				PaintTool::Room, swatches, 0, 2, 3),
			"A cell already wearing the picked variant is not paintable to the "
			"hover: " + already.diagnostic);
		std::string alreadyDiagnostic;
		auto const alreadyWrite = fixture.authoring().setSectorSurfaceVariant(
			already.sectorIndex, already.column, already.level, already.variant,
			&alreadyDiagnostic);
		require(alreadyWrite.accepted,
			"The already-painted cell came back refused: " + alreadyDiagnostic);
		require(!alreadyWrite.changed,
			"The already-painted cell came back changed");

		// Refused at the resolve: a Corridor cell under the Room's swatch.
		auto const wrong = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, Fixture::corridorX + 1,
			Fixture::corridorY);
		require(!wrong.ok
				&& !canSurfacePaint(fixture.documentWorld(), PaintTool::Room,
					swatches, 0, Fixture::corridorX + 1, Fixture::corridorY),
			"A Corridor cell under a Room swatch came back paintable");
		require(!wrong.diagnostic.empty(),
			"The refusal carried no reason for the hover to show");

		// Refused at the write: a Background, which the World will not let
		// wear an entry at all. Accepted is false here and true above, which
		// is the distinction the hover depends on and the one a single bool
		// carrying "was it a no-op" would have thrown away.
		std::string flatDiagnostic;
		auto const flatWrite = fixture.authoring().setSectorSurfaceVariant(
			fixture.backgroundIndex, 0, 0, 1, &flatDiagnostic);
		require(!flatWrite.accepted && !flatWrite.changed,
			"A Background write did not come back refused in both fields: "
				+ flatDiagnostic);
		require(!flatDiagnostic.empty(),
			"The write refusal carried no reason: " + flatDiagnostic);

		// And the two answers differ where they are supposed to: accepted
		// separates the already-painted cell from the flat one, and neither
		// of them is the refusal the resolve gave for the Corridor.
		require(alreadyWrite.accepted && !flatWrite.accepted,
			"Accepted and refused came back the same way, so the hover cannot "
			"tell them apart");
		require(serializeWorld(fixture.documentWorld()) == saved,
			"Driving the accepted-unchanged and refused answers changed the "
			"document");
		require(fixture.authoring().undoDepth() == baseline,
			"Driving the accepted-unchanged answer took an undo step");
		require(!fixture.authoring().isModified(),
			"Driving the accepted-unchanged answer marked the document modified");
	}

	// ------------------------------------------------------------------
	// Create mode, unaffected.
	// ------------------------------------------------------------------

	// With the strip on screen and nothing picked, the tool is in create and
	// the create path still builds Sectors exactly as it did before any of
	// this existed - and nothing about the swatch selection puts a variant
	// entry on what it makes.
	void createModeStillCreatesSectorsWithNoSwatchPicked(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Surface paint create", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture(false, false);
		auto& authoring = fixture.authoring();

		// The strip is there - the author can see the family - but nothing
		// is picked, so the tool is doing its create job.
		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.size() == 4,
			"The Room strip is not the base plus its three variants");
		require(!swatches.hasSelection(), "The strip arrived with a pick made");
		require(paintModeOf(PaintTool::Room, swatches) == PaintMode::Create,
			"A tool with its strip showing but nothing picked is not in create");
		require(!resolveSurfacePaint(fixture.documentWorld(), PaintTool::Room,
				swatches, 0, 2, 3).ok,
			"A paint resolve succeeded with no swatch picked");

		// And create still creates.
		auto const before = fixture.documentWorld().getNumSectors();
		authoring.world().pauseSimulation();
		auto const secondRoom = authoring.addRoom("Second", 0, 0, 6, 3, 1);
		authoring.addCorridor(0, 3, 6, 4, 1);
		authoring.finishBuild();
		require(fixture.documentWorld().getNumSectors() == before + 2,
			"Creating with the strip on screen made the wrong number of Sectors");
		require(fixture.sector(secondRoom)->getName() == "Second",
			"The created Room is not the one the create call made");
		require(fixture.sector(secondRoom)->getSurfaceVariants().empty(),
			"The created Room came with variant entries on it: "
				+ describe(fixture.sector(secondRoom)->getSurfaceVariants()));

		// Picking a swatch now switches into surface-paint, and the create
		// job stops being what a click does - which is the whole point of
		// the mode, and the reason the two cannot be confused. The cell used
		// here is the Corridor created above, so the switch is followed by a
		// refusal of the kind only surface-paint makes.
		require(swatches.toggle(3), "Picking a swatch was refused");
		require(paintModeOf(PaintTool::Room, swatches) == PaintMode::SurfacePaint,
			"Picking a swatch after creating left the tool out of surface-paint");
		require(!resolveSurfacePaint(fixture.documentWorld(), PaintTool::Room,
				swatches, 0, 6, 3).ok,
			"A swatch picked for the Room was accepted on a Corridor cell");
		require(fixture.sector(secondRoom)->getSurfaceVariants().empty(),
			"The mode switch itself put an entry on an existing Sector");
	}
}

namespace editor_smoke
{
	void registerSurfacePaint(std::vector<smoke::Check>& checks)
	{
		checks.push_back({
			"surfacePaint/pickingAnySwatchSwitchesTheToolIntoSurfacePaint",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				pickingAnySwatchSwitchesTheToolIntoSurfacePaint(context);
			} });
		checks.push_back({
			"surfacePaint/deselectingTheSwatchReturnsTheToolToCreate",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				deselectingTheSwatchReturnsTheToolToCreate(context);
			} });
		checks.push_back({
			"surfacePaint/aMatchingCellRecordsTheVariantAndItRendersImmediately",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aMatchingCellRecordsTheVariantAndItRendersImmediately(context);
			} });
		checks.push_back({
			"surfacePaint/theBaseSwatchRemovesTheEntryEntirely",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				theBaseSwatchRemovesTheEntryEntirely(context);
			} });
		checks.push_back({
			"surfacePaint/aCellOfTheWrongSectorTypeIsRefusedAndChangesNothing",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aCellOfTheWrongSectorTypeIsRefusedAndChangesNothing(context);
			} });
		checks.push_back({
			"surfacePaint/aCellOutsideAnySectorOfThatTypeIsRefusedTheSameWay",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aCellOutsideAnySectorOfThatTypeIsRefusedTheSameWay(context);
			} });
		checks.push_back({
			"surfacePaint/oneUndoStepPerClickAndUndoRestoresExactly",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				oneUndoStepPerClickAndUndoRestoresExactly(context);
			} });
		checks.push_back({
			"surfacePaint/clearingACellThatHasNothingToClearIsAcceptedAndChangesNothing",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				clearingACellThatHasNothingToClearIsAcceptedAndChangesNothing(context);
			} });
		checks.push_back({
			"surfacePaint/repaintingTheVariantAlreadyWornIsAcceptedAndChangesNothing",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				repaintingTheVariantAlreadyWornIsAcceptedAndChangesNothing(context);
			} });
		checks.push_back({
			"surfacePaint/refusalAndAcceptedButUnchangedAreNotTheSameAnswer",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				refusalAndAcceptedButUnchangedAreNotTheSameAnswer(context);
			} });
		checks.push_back({
			"surfacePaint/createModeStillCreatesSectorsWithNoSwatchPicked",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				createModeStillCreatesSectorsWithNoSwatchPicked(context);
			} });
	}
}
