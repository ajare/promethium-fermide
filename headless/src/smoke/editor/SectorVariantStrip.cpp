// The palette's variant sub-strip (#1041).
//
// Selecting a Sector paint tool reveals, beneath it, one swatch per tile
// that type declares. The strip is the author's first sight of the variant
// families #1038 put into the tileset, and two things have to be true of it
// before anything can be painted with it: it must show exactly what the
// tileset declares - no empty slot that has no artwork behind it, and no
// missing swatch for a variant that exists - and the pick has to be a fact
// the tray can be trusted about, rather than a highlight that outlives the
// strip it was made on.
//
// The strip is driven through the authoring seam rather than the ImGui
// widgets, which have no state of their own to be wrong: the swatch list,
// the selection and the swatch's own drawing are all production code that
// the tray calls, and the geometry is the same layout arithmetic the tray
// lays the buttons out with.
//
// What is asserted here:
//
//   * the strip is the base plus exactly the declared variants, in that
//     order, and every swatch's tile is present in the installed atlas;
//   * the count comes from the tileset's per-kind query, so a set that
//     declares two gives three swatches and a set that declares five gives
//     six, with nothing in the editor changed between the two;
//   * a tool that paints no tiled surface - no tool at all, or one of the
//     two flat-coloured types - gets no strip;
//   * picking highlights, the next pick moves, a pick outside the strip is
//     refused, and a strip that is not the one the pick was made on retires
//     the pick instead of leaving it pointing at a different tile;
//   * a swatch is atlas artwork with its tile name over it, which is what
//     makes the strip readable on a placeholder sheet;
//   * the strip is a row of the tray, inside the tray, below every row of
//     buttons, and starts under the tool it belongs to.

#include "Checks.h"
#include "State.h"

#include "PaletteLayout.h"
#include "PaintTool.h"
#include "SectorTileset.h"
#include "SectorVariantStrip.h"
#include "WorldDrawList.h"

#include <algorithm>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace
{
	using smoke::require;

	std::string const kTileset = "resources/textures/sectors-new.tileset.yaml";

	// The ten surface-bearing Sector types, by the tile name each one's
	// paint tool asks the atlas for.
	std::vector<std::string> const kSurfaces{ "room", "corridor", "ladder",
		"lift", "shuttle", "stairwell", "staircase", "airlock", "chamber",
		"dumbwaiter" };

	// The paint tools that paint a tiled surface, paired with the kind each
	// one asks the atlas for. Recovered from the mapping itself rather than
	// written out by hand, so a tool added to the enum is a tool these checks
	// cover without being added to them again - and a tool that maps to a
	// kind nobody authored is caught rather than quietly skipped.
	std::vector<std::pair<PaintTool, std::string>> surfaceTools()
	{
		std::vector<std::pair<PaintTool, std::string>> tools;
		int withKind = 0;
		for (int index = 0; index < static_cast<int>(PaintTool::Count); ++index)
		{
			auto const tool = static_cast<PaintTool>(index);
			auto const* mapped = sectorKindForPaintTool(tool);
			if (!mapped) continue;
			++withKind;
			require(std::find(kSurfaces.begin(), kSurfaces.end(),
				std::string{ mapped }) != kSurfaces.end(),
				std::string{ "A paint tool maps to the Sector kind '" } + mapped
					+ "', which is not one of the ten authored surface tiles");
			tools.emplace_back(tool, mapped);
		}
		require(withKind == static_cast<int>(kSurfaces.size()),
			"The tool-to-kind mapping covers " + std::to_string(withKind)
				+ " tools for " + std::to_string(kSurfaces.size())
				+ " surface tiles, so a tool is either missing or doubled");
		return tools;
	}

	// Installs the authored new-family set, the one the editor ships with,
	// so the strip is read against the counts a real file declares.
	SectorTileset installAuthoredAtlas(smoke::Context const& context)
	{
		auto const atlas = SectorTileset::load(context.fixture(kTileset));
		require(atlas.family == SectorTilesetFamily::New,
			"The authored new set did not install as the New family");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorSurfaceVariantCount("corridor") == 3,
			"The installed atlas does not report the family the file declared");
		return atlas;
	}

	// Writes and installs a version 2 set in which one named surface
	// declares `variants` of its own and every other surface declares none.
	// The declared number is the only thing that differs between the sets
	// the checks below install, which is what makes the strip's size a fact
	// about the tileset rather than about anything the editor knows.
	SectorTileset installSetDeclaring(smoke::Context const& context,
		std::string const& tag, std::string const& kind, int variants)
	{
		std::ostringstream yaml;
		yaml << "version: 2\n"
			<< "image: invented.png\n"
			<< "size: [1280, " << ((variants + 1) * 320) << "]\n"
			<< "tile_size: [128, 320]\n"
			<< "sprites:\n";
		int column = 0;
		for (auto const& name : kSurfaces)
		{
			yaml << "  " << name << ":\n"
				<< "    cell: [" << column++ << ", 0]\n"
				<< "    variants: " << (name == kind ? variants : 0) << "\n"
				<< "    content: [0, 0, 128, 320]\n"
				<< "    offset: [0.0, 0.0]\n"
				<< "    drawn_size: [1.0, 1.0]\n"
				<< "    tintable: true\n";
		}
		auto const path = context.temporaryRoot() / (tag + ".tileset.yaml");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << yaml.str();
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		auto const loaded = SectorTileset::load(path);
		require(loaded.variantCount(kind) == variants,
			"The fixture set does not declare the count the check asked for");
		setSectorTileset(loaded, static_cast<TextureId>(1));
		return loaded;
	}

	// Every check runs on the Editor's own state boundary, which restores UI
	// settings and discards pending edits on the way out, and holds a CPU
	// ImGui context for the swatch drawing to record into.
	using editor_smoke::State;

	template<auto Function>
	void withState(smoke::Context const& context)
	{
		State state;
		Function(context);
	}

	std::string describe(std::vector<SectorVariantSwatch> const& strip)
	{
		std::ostringstream out;
		out << "[";
		for (std::size_t index = 0; index < strip.size(); ++index)
		{
			if (index) out << ", ";
			out << strip[index].tileName;
		}
		out << "]";
		return out.str();
	}

	// ------------------------------------------------------------------
	// What the strip shows.
	// ------------------------------------------------------------------

	// Base first, then V1..VN in the order the family derives them, and
	// nothing after that.
	void stripIsBasePlusEveryDeclaredVariant(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		auto const tools = surfaceTools();
		require(tools.size() == kSurfaces.size(),
			"Only " + std::to_string(tools.size())
				+ " of the ten surface tools map to a Sector kind");
		for (auto const& [tool, kind] : tools)
		{
			auto const strip = sectorVariantSwatches(tool);
			auto const declared = sectorSurfaceVariantCount(kind);
			require(strip.size() == static_cast<std::size_t>(declared) + 1u,
				kind + "'s strip holds " + std::to_string(strip.size())
					+ " swatches for a family of " + std::to_string(declared));
			require(strip.front().variant == 0
					&& strip.front().tileName == kind,
				kind + "'s strip does not start with the base tile");
			for (int variant = 1; variant <= declared; ++variant)
			{
				auto const& swatch = strip[static_cast<std::size_t>(variant)];
				require(swatch.variant == variant,
					kind + "'s swatch " + std::to_string(variant)
						+ " is not variant " + std::to_string(variant));
				require(swatch.tileName == kind + "-V" + std::to_string(variant),
					kind + "'s swatch " + std::to_string(variant) + " is named '"
						+ swatch.tileName + "' instead of the tile the family derived");
			}
		}
	}

	// The count is read from the tileset's per-kind query, so changing what
	// a type declares changes the strip and nothing in the editor is touched
	// between the two installs. Two variants gives three swatches and never
	// a fourth; five gives six.
	void theCountComesFromTheTilesetQuery(smoke::Context const& context)
	{
		installSetDeclaring(context, "two-variants", "corridor", 2);
		auto const small = sectorVariantSwatches(PaintTool::Corridor);
		require(small.size() == 3,
			"A Corridor declaring two variants showed " + describe(small)
				+ " instead of base plus two");
		// A type that declares nothing keeps its base and gains no empty
		// slot beside it.
		auto const other = sectorVariantSwatches(PaintTool::Room);
		require(other.size() == 1,
			"A Room declaring no variants showed " + describe(other)
				+ " instead of its base alone");

		installSetDeclaring(context, "five-variants", "corridor", 5);
		auto const large = sectorVariantSwatches(PaintTool::Corridor);
		require(large.size() == 6,
			"A Corridor declaring five variants showed " + describe(large)
				+ " instead of base plus five");
		require(large.back().variant == 5
				&& large.back().tileName == "corridor-V5",
			"The five-variant strip does not end at the variant the file declared");
	}

	// No swatch is offered without the artwork behind it. This is the
	// promise the acceptance criteria care about most: an author can never
	// pick a slot that has nothing in it.
	void everySwatchHasItsTileInTheInstalledAtlas(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		for (auto const& [tool, kind] : surfaceTools())
		{
			for (auto const& swatch : sectorVariantSwatches(tool))
				require(hasSectorTileSurface(swatch.tileName),
					kind + "'s swatch '" + swatch.tileName
						+ "' has no tile in the installed atlas");
		}
		// And the same against a set whose families are a different size,
		// where the swatch names are derived rather than read off the file.
		installSetDeclaring(context, "three-variants-checked", "lift", 3);
		for (auto const& swatch : sectorVariantSwatches(PaintTool::Lift))
			require(hasSectorTileSurface(swatch.tileName),
				"A derived swatch '" + swatch.tileName
					+ "' was offered with no tile behind it");
	}

	// A doorframe is never offered as a swatch (#1054).
	//
	// A frame is derived from the Door standing in a wall; there is no
	// authoring act that puts one down. An author who could pick a
	// `doorframe-*` swatch would be painting a moulding onto a cell no
	// Door frames - a picture no rule produced, and one the next Door to
	// arrive would cover anyway. The strip cannot offer one because it is
	// built from the surface query and the per-kind variant count, and a
	// frame is in neither. That is a property of how the strip is built,
	// and worth pinning rather than rediscovering the day the strip is
	// rebuilt.
	void theStripNeverOffersADoorframeTile(smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		require(!atlas.overlays.empty(),
			"The authored atlas installed with no doorframes, so this check has "
			"nothing to keep out of the strip");
		for (auto const& [tool, kind] : surfaceTools())
		{
			for (auto const& swatch : sectorVariantSwatches(tool))
			{
				require(atlas.overlays.count(swatch.tileName) == 0,
					kind + "'s swatch strip offers the doorframe '"
					+ swatch.tileName + "'; a frame is derived from the Door in "
					"the wall and is not a thing an author can paint");
				require(swatch.tileName.rfind("doorframe-", 0) != 0,
					kind + "'s swatch strip offers '" + swatch.tileName
					+ "', which is named as a doorframe");
				require(hasSectorTileSurface(swatch.tileName),
					kind + "'s swatch '" + swatch.tileName
						+ " is not a Sector surface, so the strip is offering a "
						+ "tile the surface query does not know");
			}
		}
		// And the frames the atlas does carry are reachable only through the
		// overlay query, never the surface one the strip reads.
		for (auto const& entry : atlas.overlays)
			require(!hasSectorTileSurface(entry.first),
				"The doorframe '" + entry.first
					+ "' answers the Sector-surface query yes, so a strip built "
					+ "from that query could offer it");
	}

	// A tool that paints no tiled surface has no strip. Background and
	// Facade are Sector types authored as flat colour - they never ask the
	// atlas for a tile, and a variant entry on one is refused at load
	// (#1040) - so a swatch under either would be a choice that could
	// never be drawn. With no tool selected there is no type whose family
	// to show, and so no strip either.
	void aToolThatPaintsNoSurfaceShowsNoStrip(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		require(!sectorKindForPaintTool(PaintTool::None),
			"No tool selected still maps to a Sector kind");
		require(sectorVariantSwatches(PaintTool::None).empty(),
			"A strip appeared with no paint tool selected");
		for (auto const tool : { PaintTool::Background, PaintTool::Facade })
		{
			require(!sectorKindForPaintTool(tool),
				"A flat-coloured tool still maps to a Sector kind");
			require(sectorVariantSwatches(tool).empty(),
				"A strip appeared under a tool that draws no tiled surface");
		}
	}

	// With nothing installed there is no artwork to show, so no strip shows
	// either rather than a row of holes.
	void noAtlasInstalledShowsNoStrip(smoke::Context const&)
	{
		clearSectorTileset();
		for (auto const& [tool, kind] : surfaceTools())
			require(sectorVariantSwatches(tool).empty(), kind
				+ "'s strip appeared with no atlas installed to draw it from");
	}

	// ------------------------------------------------------------------
	// What the strip highlights.
	// ------------------------------------------------------------------

	// Picking a swatch highlights it; the next pick moves the highlight.
	void pickingHighlightsAndTheNextPickMoves(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		SectorSwatchSelection selection{ sectorVariantSwatches(PaintTool::Corridor) };
		require(selection.size() == 4, "The Corridor strip is not base plus three");
		require(!selection.hasSelection(),
			"A fresh strip arrived with something already picked");
		require(selection.pick(2), "Picking a swatch the strip holds was refused");
		require(selection.isSelected(2), "The picked swatch is not lit");
		require(!selection.isSelected(0) && !selection.isSelected(1)
				&& !selection.isSelected(3),
			"Picking one swatch lit more than one of them");
		require(selection.selected() == 2
				&& selection.selectedVariant().value_or(-1) == 2,
			"The selection does not report the swatch that was picked");

		require(selection.pick(0), "Picking the base swatch was refused");
		require(selection.isSelected(0) && !selection.isSelected(2),
			"The second pick did not move the highlight off the first");
		require(selection.selectedVariant().value_or(-1) == 0,
			"The base swatch does not report variant 0");
	}

	// A pick the strip cannot honour is refused and leaves the highlight
	// where it was, rather than landing on whatever the nearest swatch was.
	// The strip here is a Room's, which the fixture set leaves at its base
	// alone, so the only index it holds is 0.
	void aPickOutsideTheStripIsRefused(smoke::Context const& context)
	{
		installSetDeclaring(context, "pick-outside", "corridor", 2);
		SectorSwatchSelection selection{ sectorVariantSwatches(PaintTool::Room) };
		require(selection.size() == 1, "The Room strip is not its base alone");
		require(!selection.pick(1),
			"Picking past the end of the strip was accepted");
		require(!selection.pick(-1), "Picking before the strip was accepted");
		require(!selection.hasSelection(),
			"A refused pick left a highlight behind");
		require(!selection.selectedVariant().has_value(),
			"A refused pick reported a variant");
		require(selection.pick(0), "Picking the swatch that is there was refused");
		require(!selection.pick(7),
			"Picking far past the strip was accepted");
		require(selection.isSelected(0),
			"A refused pick moved the highlight off the swatch that was picked");
		// And the same against a strip that does hold more than its base:
		// the index just past the last swatch is refused, the last one is not.
		SectorSwatchSelection wide{ sectorVariantSwatches(PaintTool::Corridor) };
		require(wide.size() == 3, "The two-variant Corridor strip is not three swatches");
		require(wide.pick(2), "Picking the last swatch of the strip was refused");
		require(!wide.pick(3),
			"The index just past the last swatch was accepted as a pick");
		require(wide.isSelected(2),
			"A refused pick moved the highlight off the last swatch");
	}

	// The selection owns the strip it was made on. The palette puts a fresh
	// strip under it every frame, so the same strip must leave the pick
	// alone - a pick that evaporated each frame would never be seen to be
	// made - and any other strip must clear it, because a highlight that
	// survived from Corridor's V2 onto Room's V2 would be pointing at a
	// tile nobody chose.
	void aDifferentStripRetiresThePickAndTheSameOneKeepsIt(
		smoke::Context const& context)
	{
		// A set where only the Corridor has a family, so the two strips in
		// play are visibly different sizes and a pick that crossed from one
		// to the other could not be excused by them happening to match.
		installSetDeclaring(context, "retire-the-pick", "corridor", 2);
		auto const corridor = sectorVariantSwatches(PaintTool::Corridor);
		auto const room = sectorVariantSwatches(PaintTool::Room);
		require(corridor.size() == 3 && room.size() == 1,
			"The fixture strips are not the sizes this check is written against");

		SectorSwatchSelection selection{ corridor };
		require(selection.pick(2), "Picking a swatch the strip holds was refused");

		selection.setSwatches(sectorVariantSwatches(PaintTool::Corridor));
		require(selection.isSelected(2),
			"Re-putting the same strip under the pick cleared it, so no pick "
			"would survive the frame it was made in");

		selection.setSwatches(room);
		require(!selection.hasSelection(),
			"Switching to another tool's strip left the old pick lit on the new one");
		require(selection.size() == 1,
			"The new strip did not replace the old one");

		// A strip that shrinks below the picked index cannot hold it either.
		selection.setSwatches(corridor);
		require(selection.pick(2), "Picking the last swatch was refused");
		selection.setSwatches({ corridor[0], corridor[1] });
		require(!selection.hasSelection(),
			"A strip too short to hold the picked swatch kept the pick anyway");
	}

	// Clearing takes the highlight and leaves the swatches on screen: the
	// strip belongs to the tool, not to the pick.
	void clearingTakesTheHighlightAndKeepsTheStrip(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		SectorSwatchSelection selection{ sectorVariantSwatches(PaintTool::Lift) };
		require(selection.pick(1), "Picking a swatch the strip holds was refused");
		selection.clear();
		require(!selection.hasSelection(), "Clearing left the highlight lit");
		require(!selection.selectedVariant().has_value(),
			"Clearing still reported a variant");
		require(selection.size() == 4,
			"Clearing the pick took the strip with it");
		require(selection.pick(1),
			"After clearing, the strip could not be picked again");
	}

	// ------------------------------------------------------------------
	// What a swatch is drawn from.
	// ------------------------------------------------------------------

	// A swatch is the atlas tile framed into the swatch's box with its tile
	// name over it - not a colour chip. The name is what makes the strip
	// readable on a placeholder sheet, and the artwork is what makes it a
	// swatch rather than a label.
	void aSwatchIsAtlasArtworkWithItsTileNameOverlaid(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		auto const strip = sectorVariantSwatches(PaintTool::Corridor);
		for (auto const& swatch : strip)
		{
			WorldDrawList commands{ WorldDrawList::ClipRectangle{
				{ 0.0f, 0.0f }, { 1024.0f, 1024.0f } } };
			require(drawSectorVariantSwatch(swatch, &commands,
					{ 20.0f, 400.0f }, { 148.0f, 436.0f }),
				"The swatch refused to draw its own tile: " + swatch.tileName);
			int artworkTriangles = 0;
			std::vector<std::string> text;
			for (auto const& command : commands.commands())
			{
				if (auto const* triangle
					= std::get_if<WorldDrawList::Triangle>(&command))
				{
					if (triangle->texture == WorldDrawList::Texture::SectorAtlas)
						++artworkTriangles;
				}
				else if (auto const* run = std::get_if<WorldDrawList::Text>(&command))
					text.push_back(run->value);
			}
			// One framed tile is the pair of triangles one image makes.
			require(artworkTriangles == 2,
				swatch.tileName + "'s swatch drew "
					+ std::to_string(artworkTriangles)
					+ " SectorAtlas triangles instead of the two one thumbnail makes");
			require(std::find(text.begin(), text.end(), swatch.tileName)
					!= text.end(),
				swatch.tileName + "'s swatch does not overlay its tile name");
		}
	}

	// ------------------------------------------------------------------
	// Where the strip sits.
	// ------------------------------------------------------------------

	// The strip is a row of the tray: inside the tray's padding, below
	// every row of buttons the tray already has, and the tray grows by
	// exactly the rows it needs. A swatch drawn outside the tray would be
	// a swatch the tray's own hit-testing does not know about.
	void theStripIsARowOfTheTrayAndFitsInsideIt(smoke::Context const&)
	{
		ImVec2 const trayTopLeft{ 140.0f, 90.0f };
		for (bool furnitureRow : { false, true })
		{
			auto const closed = paletteTraySize(furnitureRow, 0);
			auto const rows = paletteRowCount() + (furnitureRow ? 1 : 0);
			for (int count = 1; count <= 12; ++count)
			{
				auto const stripRows = paletteVariantStripRows(count);
				require(stripRows >= 1,
					"A non-empty strip was given no row to sit in");
				auto const size = paletteTraySize(furnitureRow, stripRows);
				require(size.x == closed.x,
					"The strip changed the tray's width, so the tray would jump "
					"when a tool was selected");
				require(size.y == closed.y
						+ float(stripRows) * (PaletteSlotSize + PaletteGap),
					"The tray did not grow by exactly the strip's rows");
				auto const origin = paletteVariantStripOriginX(trayTopLeft, 0,
					count);
				for (int index = 0; index < count; ++index)
				{
					auto const min = paletteVariantSwatchMin(trayTopLeft, index,
						origin, rows);
					auto const max = paletteVariantSwatchMax(trayTopLeft, index,
						origin, rows);
					require(min.x >= trayTopLeft.x + PalettePadding
							&& max.y <= trayTopLeft.y + size.y - PalettePadding,
						"A swatch draws outside the tray that is sized to hold it");
					require(min.y >= trayTopLeft.y + PaletteTopMargin
							+ float(rows) * (PaletteSlotSize + PaletteGap),
						"A swatch sits on top of the tool rows instead of below them");
					// Every swatch is inside the tray's right edge, wrapped or not.
					require(max.x <= trayTopLeft.x + size.x - PalettePadding,
						"A swatch wrapped past the tray's right edge");
				}
			}
		}
	}

	// The strip starts under the tool it belongs to, and is pulled back only
	// as far as its own first row must be to stay inside the tray - never
	// past the tray's own inner left edge, and never left inside the tray
	// while its right edge still had room to reach under the tool.
	void theStripStartsUnderTheToolItBelongsTo(smoke::Context const&)
	{
		ImVec2 const trayTopLeft{ 140.0f, 90.0f };
		auto const innerLeft = trayTopLeft.x + PalettePadding;
		auto const innerRight = trayTopLeft.x + paletteTraySize().x
			- PalettePadding;
		auto const perRow = paletteVariantSwatchesPerRow();
		for (auto const& [tool, kind] : surfaceTools())
		{
			auto const slot = paletteSlotForPaintTool(tool);
			require(slot.has_value(),
				kind + "'s paint tool has no tray slot to sit under");
			auto const toolMin = paletteSlotMin(trayTopLeft, *slot).x;
			// A family of three, which is what the shipped set declares, so
			// the strip's first row is four swatches wide.
			int const count = 4;
			int const firstRow = count < perRow ? count : perRow;
			auto const rowWidth = float(firstRow) * (PaletteSwatchWidth + PaletteGap)
				- PaletteGap;
			auto const origin = paletteVariantStripOriginX(trayTopLeft,
				paletteSlotColumn(*slot), count);
			require(origin >= innerLeft - 0.01f,
				kind + "'s strip starts outside the tray's left padding");
			require(origin + rowWidth <= innerRight + 0.01f,
				kind + "'s strip runs past the tray's inner right edge");
			if (toolMin + rowWidth <= innerRight + 0.01f)
				require(std::abs(origin - toolMin) < 0.01f,
					kind + "'s strip could have started under its tool and did not");
			else
				require(origin + rowWidth >= innerRight - 0.01f,
					kind + "'s strip was pulled back further than it had to be, "
					"leaving room between it and the tray edge");
		}
	}
}

namespace editor_smoke
{
	void registerSectorVariantStrip(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "sectorVariantStrip/stripIsBasePlusEveryDeclaredVariant",
			withState<stripIsBasePlusEveryDeclaredVariant> });
		checks.push_back({ "sectorVariantStrip/theCountComesFromTheTilesetQuery",
			withState<theCountComesFromTheTilesetQuery> });
		checks.push_back({ "sectorVariantStrip/everySwatchHasItsTileInTheInstalledAtlas",
			withState<everySwatchHasItsTileInTheInstalledAtlas> });
		checks.push_back({ "sectorVariantStrip/theStripNeverOffersADoorframeTile",
			withState<theStripNeverOffersADoorframeTile> });
		checks.push_back({ "sectorVariantStrip/aToolThatPaintsNoSurfaceShowsNoStrip",
			withState<aToolThatPaintsNoSurfaceShowsNoStrip> });
		checks.push_back({ "sectorVariantStrip/noAtlasInstalledShowsNoStrip",
			withState<noAtlasInstalledShowsNoStrip> });
		checks.push_back({ "sectorVariantStrip/pickingHighlightsAndTheNextPickMoves",
			withState<pickingHighlightsAndTheNextPickMoves> });
		checks.push_back({ "sectorVariantStrip/aPickOutsideTheStripIsRefused",
			withState<aPickOutsideTheStripIsRefused> });
		checks.push_back({ "sectorVariantStrip/aDifferentStripRetiresThePickAndTheSameOneKeepsIt",
			withState<aDifferentStripRetiresThePickAndTheSameOneKeepsIt> });
		checks.push_back({ "sectorVariantStrip/clearingTakesTheHighlightAndKeepsTheStrip",
			withState<clearingTakesTheHighlightAndKeepsTheStrip> });
		checks.push_back({ "sectorVariantStrip/aSwatchIsAtlasArtworkWithItsTileNameOverlaid",
			withState<aSwatchIsAtlasArtworkWithItsTileNameOverlaid> });
		checks.push_back({ "sectorVariantStrip/theStripIsARowOfTheTrayAndFitsInsideIt",
			withState<theStripIsARowOfTheTrayAndFitsInsideIt> });
		checks.push_back({ "sectorVariantStrip/theStripStartsUnderTheToolItBelongsTo",
			withState<theStripStartsUnderTheToolItBelongsTo> });
	}
}
