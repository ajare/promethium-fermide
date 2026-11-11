// A World selects which atlas fills its Objects slot, and the object
// renderer draws its Doors, Windows, Lifts and Shuttles at the extents
// that atlas declares (#1022). That makes the selection a load-time
// question rather than a build-time one: nobody reviews the art beside the
// choice, so the check has to be the review.
//
// These checks drive the contract directly. The shipped set has to pass it;
// a set missing a required tile, declaring a drawn size the renderer does
// not draw at, or declaring a render offset that would move a tile off the
// thing it belongs to, has to be refused with a message that names the
// tile and both values. A refusal the reader cannot act on is only half a
// refusal.
//
// The version 1 ObjectAtlas is exempt, and that exemption is itself
// asserted: it derives its semantics from each tile's content rect rather
// than declaring them, and it is the artwork every existing World already
// draws. Refusing it would refuse the default.

#include "Checks.h"

#include "ObjectTileset.h"
#include "TileArtwork.h"

#include "core/Defines.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	using smoke::require;

	std::string const kNewObjectsTileset =
		"resources/textures/objects-new.tileset.yaml";

	// The object renderer itself. The contract is about what it draws, so
	// this is the file the contract has to be read against.
	std::string const kRendererSource = "pf-render/src/Render.cpp";

	// The calls that take an Objects tile name and put it on the screen.
	// `drawPart` is the Shuttle carriage's own wrapper: it names the tile
	// to the sprite call and falls back to the carriage fill when the
	// installed set carries no tile for the cell.
	std::vector<std::string> const kDrawHelpers{
		"drawObjectSprite(", "drawOffsetTile(", "drawRetractedTile(",
		"drawPart(",
	};

	// A tile name reaches a draw call by being an argument to one, or by
	// being the arm of the conditional that picks the name for one. The
	// widest shape that does so in the renderer today is the Shuttle's
	// three-way role choice, four lines from the call that consumes it; the
	// window leaves room for one more line of conditional without letting a
	// name two statements away pass for a drawn one.
	std::size_t const kDrawCallWindow = 6;

	std::string readText(std::filesystem::path const& path)
	{
		std::ifstream file(path, std::ios::binary);
		require(file.is_open(), "Could not open " + path.string());
		std::ostringstream text;
		text << file.rdbuf();
		return text.str();
	}

	std::vector<std::string> splitLines(std::string const& text)
	{
		std::vector<std::string> lines;
		std::istringstream stream(text);
		for (std::string line; std::getline(stream, line);)
			lines.push_back(line);
		return lines;
	}

	// Comments stripped, string and character literals kept. A tile named
	// only in prose is not a tile that gets drawn - that is the whole
	// finding this check exists to make impossible - and a `//` or `/*`
	// inside a literal must not be read as the start of a comment.
	std::string withoutComments(std::string const& source)
	{
		std::string code;
		code.reserve(source.size());
		enum class State { Code, Line, Block, Text, Char } state{State::Code};
		for (std::size_t i = 0; i < source.size(); ++i)
		{
			auto const c = source[i];
			auto const next = i + 1 < source.size() ? source[i + 1] : '\0';
			switch (state)
			{
			case State::Code:
				if (c == '/' && next == '/') { state = State::Line; ++i; }
				else if (c == '/' && next == '*') { state = State::Block; ++i; code += ' '; }
				else if (c == '"') { state = State::Text; code += c; }
				else if (c == '\'') { state = State::Char; code += c; }
				else code += c;
				break;
			case State::Line:
				if (c == '\n') { state = State::Code; code += c; }
				break;
			case State::Block:
				if (c == '*' && next == '/') { state = State::Code; ++i; code += ' '; }
				else if (c == '\n') code += '\n';
				else code += ' ';
				break;
			case State::Text:
			case State::Char:
				if (c == '\\' && i + 1 < source.size()) { code += c; code += source[++i]; }
				else if ((c == '"' && state == State::Text)
					|| (c == '\'' && state == State::Char)) { state = State::Code; code += c; }
				else if (c == '\n') { state = State::Code; code += c; }
				else code += c;
				break;
			}
		}
		return code;
	}

	std::vector<std::size_t> linesCalling(std::vector<std::string> const& lines)
	{
		std::vector<std::size_t> calls;
		for (std::size_t i = 0; i < lines.size(); ++i)
			for (auto const& helper : kDrawHelpers)
				if (lines[i].find(helper) != std::string::npos)
				{
					calls.push_back(i);
					break;
				}
		return calls;
	}

	std::vector<std::size_t> linesNaming(std::vector<std::string> const& lines,
		std::string const& tileName)
	{
		auto const literal = "\"" + tileName + "\"";
		std::vector<std::size_t> found;
		for (std::size_t i = 0; i < lines.size(); ++i)
			if (lines[i].find(literal) != std::string::npos) found.push_back(i);
		return found;
	}

	// The tiles this ticket's contract is about, in the order the renderer
	// names them. Kept here as a list rather than derived so a tile dropped
	// from the contract shows up as a missing name rather than as nothing.
	std::vector<std::string> const kRequiredTiles{
		"door", "bulkhead-door", "access-panel",
		"window-clear", "window-frosted", "window-tinted",
		"booth-window-open", "booth-window-shutter",
		"button-enabled", "button-disabled", "marker",
		"walkway", "lift-car", "platform-lift",
		"shuttle-L0", "shuttle-M0", "shuttle-R0", "shuttle-connector",
	};

	// The shipped set passes, and passes with no diagnostic at all: an
	// empty string is the whole signal that a World may select this atlas.
	void shippedObjectsAtlasSatisfiesTheContract(smoke::Context const& context)
	{
		auto const atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		require(atlas.declaresRenderSemantics,
			"objects-new.tileset.yaml is a version 2 set but reports no declared "
			"render semantics, so the contract would skip it");
		auto const diagnostic = checkObjectAtlasContract("ObjectAtlas",
			atlas.sprites);
		require(diagnostic.empty(),
			"The shipped Objects atlas does not satisfy its own contract: "
			+ diagnostic);
	}

	// The contract covers every tile the object renderer asks the Objects
	// slot for. A tile left out of the list goes unchecked, so the list is
	// asserted against the names rather than trusted.
	void theContractCoversEveryDrawnTile(smoke::Context const& context)
	{
		auto const required = requiredObjectTiles();
		require(required.size() == kRequiredTiles.size(),
			"The Objects contract requires " + std::to_string(required.size())
			+ " tiles, expected " + std::to_string(kRequiredTiles.size()));
		for (auto const& name : kRequiredTiles)
		{
			auto const found = std::find_if(required.begin(), required.end(),
				[&](ObjectTileExpectation const& e) { return name == e.tileName; });
			require(found != required.end(),
				"The Objects contract does not require '" + name
				+ "', so a World could select an atlas with no artwork for it");
		}
	}

	// Every entry the contract makes has to be a tile the renderer draws.
	// #1031 found `booth-window-closed` required here with no draw call
	// naming it anywhere: a hard requirement on artwork nothing consumed,
	// costing an atlas slot, able to refuse an otherwise usable atlas, and
	// guarding nothing with its declared drawn size. The list's own comment
	// used to say the two lists belong in one review; this is that review,
	// run on every check instead of left to whoever edits next.
	void everyRequiredTileIsNamedByADrawCall(smoke::Context const& context)
	{
		using smoke::require;
		auto const lines = splitLines(withoutComments(
			readText(context.fixture(kRendererSource))));
		require(!lines.empty(), "The object renderer source read no lines");
		auto const drawCalls = linesCalling(lines);
		require(!drawCalls.empty(),
			"The object renderer source calls no sprite draw helper at all, so no "
			"required tile could be seen being drawn");

		std::vector<std::string> undrawn;
		for (auto const& expected : requiredObjectTiles())
		{
			auto const named = linesNaming(lines, expected.tileName);
			bool reachesADrawCall = false;
			for (auto const namedLine : named)
				for (auto const drawLine : drawCalls)
					if (drawLine + kDrawCallWindow >= namedLine
						&& drawLine <= namedLine + kDrawCallWindow)
						reachesADrawCall = true;
			if (!reachesADrawCall)
				undrawn.push_back(std::string(expected.tileName)
					+ (named.empty()
						? " is named nowhere in the renderer"
						: " is named only where no draw call reaches it"));
		}
		std::string joined;
		for (auto const& problem : undrawn)
			joined += "\n  - " + problem;
		require(undrawn.empty(),
			"The Objects contract requires " + std::to_string(undrawn.size())
			+ " tile(s) the object renderer never draws, so an atlas could be "
			"refused for artwork nothing would ever ask for:" + joined);
	}

	// Each expectation is composed from the domain's own constants rather
	// than remembered numbers, so a change to the geometry moves the
	// contract with it. These are the ones a wrong value would put artwork
	// outside the collision it belongs to.
	void theExpectedExtentsFollowTheDomainGeometry(smoke::Context const&)
	{
		auto const find = [](std::string const& name) -> ObjectTileExpectation {
			for (auto const& e : requiredObjectTiles())
				if (name == e.tileName) return e;
			throw std::runtime_error("no expectation for " + name);
		};
		auto const near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };

		require(near(find("door").drawnWidth, 1.0f - 2.0f * CORE_DOOR_X_INSET)
				&& near(find("door").drawnHeight, CORE_DOOR_HEIGHT),
			"The Door expectation is not the doorway less its x insets at the "
			"Door height");
		require(near(find("bulkhead-door").drawnWidth, CORE_BULKHEAD_DOOR_WIDTH)
				&& near(find("bulkhead-door").drawnHeight, CORE_CORRIDOR_HEIGHT),
			"The Bulkhead Door expectation is not its slab width at the corridor "
			"height");
		require(near(find("window-clear").drawnWidth,
				1.0f - 2.0f * CORE_WINDOW_X_INSET)
				&& near(find("window-clear").drawnHeight, CORE_WINDOW_HEIGHT),
			"The Window expectation is not the cell less its x insets at the "
			"Window height");
		require(near(find("button-enabled").drawnWidth, CORE_BUTTON_SIZE)
				&& near(find("button-enabled").drawnHeight,
					CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO),
			"The Button expectation is not the control size at the cell's y:x "
			"render ratio");
		require(near(find("lift-car").drawnWidth, 1.0f - 2.0f * CORE_LIFT_CAR_BORDER)
				&& near(find("lift-car").drawnHeight, CORE_LIFT_CAR_HEIGHT),
			"The Lift car expectation is not the cell less the car border at the "
			"car height");
		require(near(find("shuttle-L0").drawnHeight, CORE_SHUTTLE_CAR_HEIGHT),
			"The Shuttle cell expectation is not the carriage height");
		// The Walkway is the one tile whose placement is entirely its
		// declared offset: its physical shape is a zero-height line, so the
		// offset is the only thing hanging its slab on the floor.
		require(near(find("walkway").renderOffset.y, -0.075f)
				&& near(find("walkway").drawnHeight, 0.075f),
			"The Walkway slab does not hang below its anchor by its own thickness");
	}

	// A required tile absent from the atlas is refused, by name.
	void aMissingRequiredTileIsRefused(smoke::Context const& context)
	{
		auto atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		atlas.sprites.erase("lift-car");
		auto const diagnostic = checkObjectAtlasContract("BrokenAtlas",
			atlas.sprites);
		require(!diagnostic.empty(),
			"An Objects atlas with no 'lift-car' tile was accepted");
		require(diagnostic.find("lift-car") != std::string::npos,
			"The refusal does not name the missing tile: " + diagnostic);
		require(diagnostic.find("BrokenAtlas") != std::string::npos,
			"The refusal does not name the atlas it refused: " + diagnostic);
	}

	// A drawn size that disagrees with the domain geometry is refused, and
	// the message carries both the declared and the expected extent so the
	// fix is obvious rather than a search.
	void aMismatchedDrawnSizeIsRefused(smoke::Context const& context)
	{
		auto atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		atlas.sprites.at("door").drawnSize = { 1.0f, 0.7f };
		auto const diagnostic = checkObjectAtlasContract("WideDoorAtlas",
			atlas.sprites);
		require(diagnostic.find("door") != std::string::npos,
			"The drawn-size refusal does not name the Door tile: " + diagnostic);
		require(diagnostic.find("1 x 0.7") != std::string::npos
				|| diagnostic.find("1.0 x 0.7") != std::string::npos,
			"The drawn-size refusal does not carry the declared extent: "
			+ diagnostic);
		require(diagnostic.find("0.8 x 0.5") != std::string::npos,
			"The drawn-size refusal does not carry the expected extent: "
			+ diagnostic);
	}

	// A render offset that disagrees is refused the same way. This is the
	// case that would otherwise show up as a Walkway slab drawn through the
	// floor it supports.
	void aMismatchedRenderOffsetIsRefused(smoke::Context const& context)
	{
		auto atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		atlas.sprites.at("walkway").offset = { 0.0f, 0.0f };
		auto const diagnostic = checkObjectAtlasContract("FloatingWalkwayAtlas",
			atlas.sprites);
		require(diagnostic.find("walkway") != std::string::npos,
			"The offset refusal does not name the Walkway tile: " + diagnostic);
		require(diagnostic.find("render offset") != std::string::npos,
			"The offset refusal does not say which semantic is wrong: "
			+ diagnostic);
		require(diagnostic.find("-0.075") != std::string::npos,
			"The offset refusal does not carry the expected offset: "
			+ diagnostic);
	}

	// Every problem in a bad atlas is reported at once. Refusing on the
	// first one would make someone fix a tile, reload, and hit the next.
	void everyProblemIsReportedAtOnce(smoke::Context const& context)
	{
		auto atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		atlas.sprites.erase("marker");
		atlas.sprites.at("door").drawnSize = { 0.5f, 0.5f };
		atlas.sprites.at("walkway").offset = { 0.1f, 0.0f };
		auto const diagnostic = checkObjectAtlasContract("ManyWaysAtlas",
			atlas.sprites);
		require(diagnostic.find("marker") != std::string::npos
				&& diagnostic.find("door") != std::string::npos
				&& diagnostic.find("walkway") != std::string::npos,
			"A bad atlas reported only some of its problems: " + diagnostic);
		require(diagnostic.find("3 tile(s)") != std::string::npos,
			"The refusal does not count the problems: " + diagnostic);
	}

	// The version 1 ObjectAtlas derives its semantics from each tile's
	// content rect and declares none of its own, so it is exempt. This is
	// the atlas every World drew from before the Objects slot could be
	// selected, and the default selection has to keep working.
	void theLegacyObjectAtlasIsExempt(smoke::Context const& context)
	{
		auto const atlas = ObjectTileset::load(
			context.fixture("resources/textures/objects.tileset.yaml"));
		require(!atlas.declaresRenderSemantics,
			"The version 1 object tileset reports declared render semantics, so "
			"the contract would now be applied to the default atlas");
	}
}

void render_smoke::registerObjectAtlasContract(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "objectAtlasContract/shippedAtlasSatisfiesTheContract",
		isolated<shippedObjectsAtlasSatisfiesTheContract> });
	checks.push_back({ "objectAtlasContract/coversEveryDrawnTile",
		isolated<theContractCoversEveryDrawnTile> });
	checks.push_back({ "objectAtlasContract/everyRequiredTileIsNamedByADrawCall",
		isolated<everyRequiredTileIsNamedByADrawCall> });
	checks.push_back({ "objectAtlasContract/expectedExtentsFollowTheDomainGeometry",
		isolated<theExpectedExtentsFollowTheDomainGeometry> });
	checks.push_back({ "objectAtlasContract/aMissingRequiredTileIsRefused",
		isolated<aMissingRequiredTileIsRefused> });
	checks.push_back({ "objectAtlasContract/aMismatchedDrawnSizeIsRefused",
		isolated<aMismatchedDrawnSizeIsRefused> });
	checks.push_back({ "objectAtlasContract/aMismatchedRenderOffsetIsRefused",
		isolated<aMismatchedRenderOffsetIsRefused> });
	checks.push_back({ "objectAtlasContract/everyProblemIsReportedAtOnce",
		isolated<everyProblemIsReportedAtOnce> });
	checks.push_back({ "objectAtlasContract/theLegacyObjectAtlasIsExempt",
		isolated<theLegacyObjectAtlasIsExempt> });
}
