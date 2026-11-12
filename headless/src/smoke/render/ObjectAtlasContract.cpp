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
//
// The contract reads both ways. Every required tile has to be a name some
// draw call asks for, and every name a draw call asks for has to be a tile
// the installed atlas carries. The second direction is the one that was
// missing, and its absence is why a deleted tile could leave its call sites
// behind with the suite green: `drawObjectSprite` returning false is a
// designed fallback, and a fallback that quietly draws something else is
// precisely the thing a contract check exists for (#1064).

#include "Checks.h"

#include "ObjectTileset.h"
#include "TileArtwork.h"

#include "core/Defines.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using smoke::require;

	std::string const kNewObjectsTileset =
		"resources/textures/objects-new.tileset.yaml";

	// The version 1 object tileset: the fallback vocabulary. The custom
	// theme sets are authored from it, it declares no render semantics, and
	// so is exempt from that half of the contract.
	std::string const kLegacyObjectsTileset =
		"resources/textures/objects.tileset.yaml";

	// Every source that names an Objects-slot tile to a draw call. The
	// contract is about the names the drawing code asks the slot for, so
	// every file that asks one has to be read against it.
	//
	// This used to be the object renderer alone, which left the editor's
	// palette icons - which name Objects tiles too, and fall back to a font
	// glyph when the tile is not there - invisible to both directions of
	// the contract. #1063 shipped inside that gap. The check further down
	// fails when a source names a tile the installed atlas does not carry,
	// and another fails when a source names tiles without being listed here,
	// so this list cannot quietly fall behind the code it describes.
	std::vector<std::string> const kTileNamingSources{
		"pf-render/src/Render.cpp",
		"editor/src/UI.cpp",
	};

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

	// One source the contract reads. The comment-stripped text is kept whole
	// as well as split, because the reverse direction has to read a draw
	// call's own argument list - which spans lines - rather than whatever
	// line happens to sit near a name.
	struct ScannedSource
	{
		std::string path;
		std::string code;
		std::vector<std::string> lines;
		// The offset each line starts at, so a byte offset can be reported
		// as the line number a reader has to open.
		std::vector<std::size_t> lineStarts;

		std::size_t lineOf(std::size_t offset) const
		{
			auto const after = std::upper_bound(lineStarts.begin(),
				lineStarts.end(), offset);
			auto const line = static_cast<std::ptrdiff_t>(after - lineStarts.begin());
			return line < 1 ? 1 : static_cast<std::size_t>(line);
		}
	};

	ScannedSource scanSource(std::string const& path, std::string code)
	{
		ScannedSource source;
		source.path = path;
		source.code = std::move(code);
		source.lines = splitLines(source.code);
		source.lineStarts.push_back(0);
		for (std::size_t i = 0; i < source.code.size(); ++i)
			if (source.code[i] == '\n') source.lineStarts.push_back(i + 1);
		return source;
	}

	ScannedSource scanSource(smoke::Context const& context, std::string const& path)
	{
		return scanSource(path, withoutComments(readText(context.fixture(path))));
	}

	std::vector<ScannedSource> scanTileNamingSources(smoke::Context const& context)
	{
		require(!kTileNamingSources.empty(),
			"No source is listed as naming Objects tiles, so the Objects contract "
			"reads no drawing code at all");
		std::vector<ScannedSource> sources;
		for (auto const& path : kTileNamingSources)
			sources.push_back(scanSource(context, path));
		return sources;
	}

	std::vector<std::size_t> linesCalling(ScannedSource const& source)
	{
		std::vector<std::size_t> calls;
		for (std::size_t i = 0; i < source.lines.size(); ++i)
			for (auto const& helper : kDrawHelpers)
				if (source.lines[i].find(helper) != std::string::npos)
				{
					calls.push_back(i);
					break;
				}
		return calls;
	}

	std::vector<std::size_t> linesNaming(ScannedSource const& source,
		std::string const& tileName)
	{
		auto const literal = "\"" + tileName + "\"";
		std::vector<std::size_t> found;
		for (std::size_t i = 0; i < source.lines.size(); ++i)
			if (source.lines[i].find(literal) != std::string::npos) found.push_back(i);
		return found;
	}

	bool isIdentChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '_';
	}

	bool isIdentifierStart(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
	}

	bool isSpace(char c) { return isspace(static_cast<unsigned char>(c)) != 0; }

	// A role-or-legacy helper takes the version 2 role name first and the
	// version 1 name to fall back to second. The renderer only reaches that
	// second name when the role tile is absent from the installed set -
	// which is to say when a version 1 set is installed - so the version 2
	// contract does not own it. It is not exempt from existing, though: a
	// fallback that resolves in no shipped set is the same rot wearing an
	// excuse, and the check below asks the version 1 set for it too.
	bool isLegacyFallbackHelper(std::string const& callee)
	{
		static std::string const suffix{ "OrLegacy" };
		return callee.size() >= suffix.size()
			&& callee.compare(callee.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// The index just past the closing quote of the string literal that opens
	// at `open`. An unterminated literal runs to the end of the text, which
	// is the honest answer rather than a wrong one.
	std::size_t literalEnd(std::string const& code, std::size_t open)
	{
		for (std::size_t i = open + 1; i < code.size(); ++i)
		{
			if (code[i] == '\\') { ++i; continue; }
			if (code[i] == '"') return i + 1;
		}
		return code.size();
	}

	// The [start, end) span of each argument of the call whose `(` is at
	// `open`. Nested parens are respected, so a comma inside a nested call
	// does not split the outer call's first argument, and string literals
	// are stepped over whole, so a `(` or `,` inside one is not structure.
	std::vector<std::pair<std::size_t, std::size_t>> callArgumentSpans(
		std::string const& code, std::size_t open)
	{
		std::vector<std::pair<std::size_t, std::size_t>> args;
		if (open >= code.size() || code[open] != '(') return args;
		std::size_t depth = 0;
		std::size_t start = open + 1;
		for (std::size_t i = open + 1; i < code.size(); ++i)
		{
			auto const c = code[i];
			if (c == '"') { i = literalEnd(code, i) - 1; continue; }
			if (c == '(') ++depth;
			else if (c == ')')
			{
				if (depth == 0)
				{
					args.emplace_back(start, i);
					return args;
				}
				--depth;
			}
			else if (c == ',' && depth == 0)
			{
				args.emplace_back(start, i);
				start = i + 1;
			}
		}
		return args;
	}

	// One string literal read out of comment-stripped code, with the line it
	// sits on and whether it is the version 1 arm of a role-or-legacy call.
	struct LiteralUse
	{
		std::string text;
		std::size_t line;
		bool legacyFallback{ false };
	};

	// Every string literal in [from, to), each tagged with whether it sits
	// at argument index 1 or later of a `...OrLegacy` call. Call frames are
	// tracked by paren depth, so the tag follows the argument the literal is
	// really in rather than anything merely near it.
	std::vector<LiteralUse> literalUsesIn(ScannedSource const& source,
		std::size_t from, std::size_t to)
	{
		std::vector<LiteralUse> uses;
		struct Frame { bool legacy; std::size_t depth; std::size_t argIndex; };
		std::vector<Frame> frames;
		std::size_t depth = 0;
		std::string ident;
		to = std::min(to, source.code.size());
		for (std::size_t i = from; i < to; ++i)
		{
			auto const c = source.code[i];
			if (isIdentChar(c)) { ident += c; continue; }
			if (!ident.empty() && isSpace(c)) continue;
			if (!ident.empty())
			{
				if (c == '(')
					frames.push_back({ isLegacyFallbackHelper(ident), depth + 1, 0 });
				ident.clear();
			}
			if (c == '(') ++depth;
			else if (c == ')')
			{
				if (!frames.empty() && frames.back().depth == depth) frames.pop_back();
				if (depth > 0) --depth;
			}
			else if (c == ',' && !frames.empty() && frames.back().depth == depth)
				++frames.back().argIndex;
			else if (c == '"')
			{
				auto const end = literalEnd(source.code, i);
				uses.push_back({ source.code.substr(i + 1,
					end >= i + 2 ? end - i - 2 : 0), source.lineOf(i),
					!frames.empty() && frames.back().legacy
						&& frames.back().argIndex >= 1 });
				i = end - 1;
			}
		}
		return uses;
	}

	std::string trimmed(std::string const& text)
	{
		std::size_t begin = 0, end = text.size();
		while (begin < end && isSpace(text[begin])) ++begin;
		while (end > begin && isSpace(text[end - 1])) --end;
		return text.substr(begin, end - begin);
	}

	bool isBareName(std::string const& text)
	{
		if (text.empty() || !isIdentifierStart(text[0])) return false;
		for (auto const c : text) if (!isIdentChar(c)) return false;
		return true;
	}

	// The offset at which the line `kDrawCallWindow` above `line` starts, or
	// the start of the file if the file is shorter than the window.
	std::size_t windowStart(ScannedSource const& source, std::size_t line)
	{
		if (line <= kDrawCallWindow) return 0;
		return source.lineStarts[line - 1 - kDrawCallWindow];
	}

	// The tile names one draw-call site asks the Objects slot for.
	//
	// A literal in the tile-name argument names it right here. A bare name
	// there means the tile was picked above, so the statement that assigned
	// it is read instead - but only within the same reach the forward check
	// uses, because a name chosen further away is a different decision and
	// not this call's. A parameter's name is no tile name at all: the call
	// that reaches it is read at its own site, which is where its literal is.
	std::vector<LiteralUse> namesAskedBy(ScannedSource const& source,
		std::size_t open)
	{
		auto const args = callArgumentSpans(source.code, open);
		if (args.empty()) return {};
		auto const first = trimmed(source.code.substr(args[0].first,
			args[0].second - args[0].first));
		if (!isBareName(first))
			return literalUsesIn(source, args[0].first, args[0].second);

		auto const from = windowStart(source, source.lineOf(open));
		for (std::size_t i = from; i + first.size() <= open; ++i)
		{
			if (i > 0 && isIdentChar(source.code[i - 1])) continue;
			if (source.code.compare(i, first.size(), first) != 0) continue;
			std::size_t j = i + first.size();
			while (j < open && isSpace(source.code[j])) ++j;
			if (j >= open || source.code[j] != '=') continue;
			if (j + 1 < source.code.size() && source.code[j + 1] == '=') continue;
			auto const terminator = source.code.find(';', j);
			return literalUsesIn(source, j + 1,
				terminator == std::string::npos ? source.code.size() : terminator);
		}
		return {};
	}

	// The offset of the `(` of every call to a sprite draw helper. A
	// definition or declaration of one of the helpers reads as a call here
	// and contributes nothing: its first argument is a parameter
	// declaration, not a tile name.
	std::vector<std::size_t> drawCallOpens(ScannedSource const& source)
	{
		std::vector<std::size_t> opens;
		for (auto const& helper : kDrawHelpers)
			for (std::size_t at = source.code.find(helper);
				at != std::string::npos;
				at = source.code.find(helper, at + 1))
				opens.push_back(at + helper.size() - 1);
		std::sort(opens.begin(), opens.end());
		return opens;
	}

	// The tiles this ticket's contract is about, in the order the renderer
	// names them. Kept here as a list rather than derived so a tile dropped
	// from the contract shows up as a missing name rather than as nothing.
	std::vector<std::string> const kRequiredTiles{
		"door-L0", "door-R0",
		"door-tall-L0", "door-tall-R0",
		"door-lift-L0", "door-lift-R0", "door-shuttle-L0",
		"bulkhead-door", "access-panel",
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

	// Every entry the contract makes has to be a tile some draw call names.
	// #1031 found `booth-window-closed` required here with no draw call
	// naming it anywhere: a hard requirement on artwork nothing consumed,
	// costing an atlas slot, able to refuse an otherwise usable atlas, and
	// guarding nothing with its declared drawn size. The list's own comment
	// used to say the two lists belong in one review; this is that review,
	// run on every check instead of left to whoever edits next. It reads
	// every source that names tiles, not only the renderer, so a name the
	// editor draws alone satisfies it the same way one the renderer draws
	// does.
	void everyRequiredTileIsNamedByADrawCall(smoke::Context const& context)
	{
		auto const sources = scanTileNamingSources(context);
		std::size_t drawCalls = 0;
		for (auto const& source : sources)
			drawCalls += linesCalling(source).size();
		require(drawCalls != 0,
			"The scanned tile-naming sources call no sprite draw helper at all, so "
			"no required tile could be seen being drawn");

		std::vector<std::string> undrawn;
		for (auto const& expected : requiredObjectTiles())
		{
			bool namedSomewhere = false;
			bool namedButUnreachably = false;
			for (auto const& source : sources)
			{
				auto const named = linesNaming(source, expected.tileName);
				if (named.empty()) continue;
				auto const calls = linesCalling(source);
				for (auto const namedLine : named)
					for (auto const drawLine : calls)
						if (drawLine + kDrawCallWindow >= namedLine
							&& drawLine <= namedLine + kDrawCallWindow)
							namedSomewhere = true;
				namedButUnreachably = true;
			}
			if (!namedSomewhere)
				undrawn.push_back(std::string(expected.tileName)
					+ (namedButUnreachably
						? " is named only where no draw call reaches it"
						: " is named nowhere in the scanned tile-naming sources"));
		}
		std::string joined;
		for (auto const& problem : undrawn)
			joined += "\n  - " + problem;
		require(undrawn.empty(),
			"The Objects contract requires " + std::to_string(undrawn.size())
			+ " tile(s) no draw call draws, so an atlas could be refused for "
			"artwork nothing would ever ask for:" + joined);
	}

	// The other direction, which was missing entirely. Every tile name a
	// draw call asks the Objects slot for has to be a tile the installed
	// atlas carries. #1063 is what the gap cost: the bare `door` tile was
	// replaced by a leaf family, the editor's palette icon went on asking
	// for the old name, `drawObjectSprite` returned false exactly as
	// designed, and the icon became a font glyph with the suite green. A
	// designed fallback that quietly draws something else is the class of
	// rot this check exists to make loud, and it names the file and line so
	// the fix is not a search.
	void everyTileNameAskedOfTheAtlasIsCarried(smoke::Context const& context)
	{
		auto const atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		require(atlas.declaresRenderSemantics,
			"objects-new.tileset.yaml reports no declared render semantics, so the "
			"atlas this check resolves drawn names against is not the version 2 "
			"set the contract is written for");
		std::set<std::string> carried;
		for (auto const& entry : atlas.sprites) carried.insert(entry.first);
		require(!carried.empty(),
			"The installed Objects atlas carries no tiles, so no drawn name could "
			"resolve against it");

		// The version 1 set is the fallback vocabulary. The custom theme
		// sets authored from it declare no render semantics and are exempt
		// from that half of the contract - refusing them would refuse every
		// World that selects one - but a name offered as the version 1 arm
		// of a fallback still has to be a name it actually carries.
		auto const legacy = ObjectTileset::load(
			context.fixture(kLegacyObjectsTileset));
		require(!legacy.declaresRenderSemantics,
			"The version 1 object tileset reports declared render semantics, so "
			"the fallback vocabulary read here is not a version 1 set");
		std::set<std::string> legacyCarried;
		for (auto const& entry : legacy.sprites) legacyCarried.insert(entry.first);

		std::vector<std::string> unresolved;
		std::size_t asked = 0;
		for (auto const& source : scanTileNamingSources(context))
			for (auto const open : drawCallOpens(source))
				for (auto const& use : namesAskedBy(source, open))
				{
					++asked;
					auto const where = source.path + ":" + std::to_string(use.line);
					if (use.legacyFallback)
					{
						if (legacyCarried.count(use.text) == 0)
							unresolved.push_back(where + ": '" + use.text
								+ "' is offered as the version 1 arm of a role-or-legacy "
								"call and no version 1 object set carries it");
						continue;
					}
					if (carried.count(use.text) == 0)
						unresolved.push_back(where + ": a sprite draw call names '"
							+ use.text + "' and the installed Objects atlas does not "
							"carry it");
				}
		require(asked != 0,
			"The scanned sources ask no tile name of a sprite draw call, so this "
			"check could not see a single name being resolved");
		std::string joined;
		for (auto const& problem : unresolved)
			joined += "\n  - " + problem;
		require(unresolved.empty(),
			std::to_string(unresolved.size())
			+ " tile name(s) asked of the Objects slot by a draw call are not in "
			"the installed atlas. Each falls back to a primitive fill or a font "
			"glyph, so nothing on screen says the tile is gone:" + joined);
	}

	// The scanned list is a claim about the codebase, and a claim nobody
	// tests is a claim that rots. Every first-party source that asks a tile
	// name of a sprite draw call has to be in it; a source outside the list
	// is invisible to both directions of the contract, which is the hole
	// #1063 fell into. Without this, widening the scan once proves nothing
	// about the next file someone adds a draw call to.
	void theScannedSourcesCoverEveryTileNamingSource(smoke::Context const& context)
	{
		// The repository root, read as the directory the root CMakeLists.txt
		// lives in. The smoke harness bakes the root in for fixtures but does
		// not export it, and this check needs the tree rather than a fixture.
		auto const root = context.fixture("CMakeLists.txt").parent_path();
		std::set<std::string> scanned(kTileNamingSources.begin(),
			kTileNamingSources.end());
		// Vendored third-party code names nothing of ours. The verification
		// tier names tiles from every atlas, including names no Objects slot
		// carries, and is read by its own checks rather than by this one.
		// Build trees, docs, resources and scripts hold no draw calls.
		std::set<std::string> const skipped{
			"ext", "headless", "docs", "resources", "scripts", "cmake", ".git",
		};
		std::vector<std::string> missed;
		std::error_code error;
		std::filesystem::recursive_directory_iterator walk(root,
			std::filesystem::directory_options::skip_permission_denied, error);
		require(!error,
			"Could not start walking the repository for tile-naming sources: "
			+ error.message());
		for (auto const end = std::filesystem::recursive_directory_iterator{};
			walk != end; ++walk)
		{
			if (walk->is_directory())
			{
				auto const name = walk->path().filename().string();
				if (skipped.count(name) != 0 || name.rfind("build", 0) == 0
					|| name.rfind("cmake-build", 0) == 0)
					walk.disable_recursion_pending();
				continue;
			}
			auto const extension = walk->path().extension().string();
			if (extension != ".cpp" && extension != ".h"
				&& extension != ".hpp" && extension != ".inl") continue;
			auto const relative = std::filesystem::relative(walk->path(), root)
				.generic_string();
			if (scanned.count(relative) != 0) continue;
			auto const source = scanSource(relative, readText(walk->path()));
			for (auto const open : drawCallOpens(source))
				for (auto const& use : namesAskedBy(source, open))
					missed.push_back(relative + ":" + std::to_string(use.line)
						+ " asks a sprite draw call for '" + use.text
						+ "' but the file is not in the scanned source list");
		}
		std::string joined;
		for (auto const& problem : missed)
			joined += "\n  - " + problem;
		require(missed.empty(),
			std::to_string(missed.size())
			+ " tile name(s) are drawn from a source the Objects contract never "
			"reads, so neither direction of the contract can see them. Add the "
			"file to kTileNamingSources:" + joined);
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

		// A Door leaf is a full World cell: the clip, not the drawn size,
		// is what keeps it inside the doorway (#1056).
		require(near(find("door-L0").drawnWidth, 1.0f)
				&& near(find("door-L0").drawnHeight, 1.0f),
			"The left Door leaf expectation is not a full cell");
		require(near(find("door-R0").drawnWidth, 1.0f)
				&& near(find("door-R0").drawnHeight, 1.0f),
			"The right Door leaf expectation is not a full cell");
		// The Lift and Shuttle leaves are leaves too: the same full cell at
		// the same zero offset, because the same aperture clip bounds them
		// (#1057). A Shuttle hatch has no right-hand expectation because
		// its family has no right-hand tile.
		require(near(find("door-lift-L0").drawnWidth, 1.0f)
				&& near(find("door-lift-L0").drawnHeight, 1.0f),
			"The left Lift landing leaf expectation is not a full cell");
		require(near(find("door-lift-R0").drawnWidth, 1.0f)
				&& near(find("door-lift-R0").drawnHeight, 1.0f),
			"The right Lift landing leaf expectation is not a full cell");
		require(near(find("door-shuttle-L0").drawnWidth, 1.0f)
				&& near(find("door-shuttle-L0").drawnHeight, 1.0f),
			"The Shuttle hatch leaf expectation is not a full cell");
		// The tall pair is a full cell for the same reason as every other
		// leaf: the taller aperture is a clip fact, not a drawn-size fact,
		// so the tall Door's 0.9 opening never reaches into the tile's
		// declared extent (#1058).
		require(near(find("door-tall-L0").drawnWidth, 1.0f)
				&& near(find("door-tall-L0").drawnHeight, 1.0f),
			"The left tall Door leaf expectation is not a full cell");
		require(near(find("door-tall-R0").drawnWidth, 1.0f)
				&& near(find("door-tall-R0").drawnHeight, 1.0f),
			"The right tall Door leaf expectation is not a full cell");
		for (auto const* absent : { "door-shuttle-R0", "door-lift-M0" })
			for (auto const& expected : requiredObjectTiles())
				require(!(std::string(absent) == expected.tileName),
					std::string("The Objects contract requires '") + absent
					+ "', which no leaf family carries");
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
		atlas.sprites.at("door-L0").drawnSize = { 1.0f, 0.7f };
		auto const diagnostic = checkObjectAtlasContract("WideDoorAtlas",
			atlas.sprites);
		require(diagnostic.find("door-L0") != std::string::npos,
			"The drawn-size refusal does not name the Door leaf tile: " + diagnostic);
		require(diagnostic.find("1 x 0.7") != std::string::npos
				|| diagnostic.find("1.0 x 0.7") != std::string::npos,
			"The drawn-size refusal does not carry the declared extent: "
			+ diagnostic);
		require(diagnostic.find("1 x 1") != std::string::npos
				|| diagnostic.find("1.0 x 1.0") != std::string::npos,
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
		atlas.sprites.at("door-L0").drawnSize = { 0.5f, 0.5f };
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
	checks.push_back({ "objectAtlasContract/everyTileNameAskedIsCarried",
		isolated<everyTileNameAskedOfTheAtlasIsCarried> });
	checks.push_back({ "objectAtlasContract/scannedSourcesCoverEveryTileNamingSource",
		isolated<theScannedSourcesCoverEveryTileNamingSource> });
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
