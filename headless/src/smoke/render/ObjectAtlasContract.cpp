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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
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

	// The editor's palette lives here, and its Door slot is the one Objects
	// tile consumer outside the renderer (#1063).
	std::string const kEditorPaletteSource{ "editor/src/UI.cpp" };

	// The calls that take an Objects tile name and put it on the screen.
	// `drawPart` is the Shuttle carriage's own wrapper: it names the tile
	// to the sprite call and falls back to the carriage fill when the
	// installed set carries no tile for the cell. `drawGlassRepeating` is
	// the Window pane's wrapper, and it is a draw call in the sense this
	// check means: it is the call the glass Style's tile name is handed to,
	// and a name it were handed that the atlas did not carry would leave the
	// pane unglazed with nothing saying so (#1079). `drawGlassOrLegacy` is
	// the same kind of wrapper one level up - the call the pane's glass is
	// resolved into and drawn by - and it is listed for the same reason: a
	// name handed to it that no installed set carried would leave the pane
	// bare, and the version 1 arm of its resolution is exactly the name the
	// role-or-legacy tagging below exists to catch (#1083).
	std::vector<std::string> const kDrawHelpers{
		"drawObjectSprite(", "drawOffsetTile(", "drawRetractedTile(",
		"drawPart(", "drawGlassRepeating(", "drawGlassOrLegacy(",
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

	// The matching `)` of the call whose `(` is at `open`, stepping over
	// string literals so a paren inside one is not structure.
	std::size_t callClose(std::string const& code, std::size_t open)
	{
		std::size_t depth = 0;
		for (std::size_t i = open; i < code.size(); ++i)
		{
			auto const c = code[i];
			if (c == '"') { i = literalEnd(code, i) - 1; continue; }
			if (c == '(') ++depth;
			else if (c == ')')
			{
				if (depth == 1) return i;
				if (depth > 0) --depth;
			}
		}
		return std::string::npos;
	}

	// The last non-space character before `at`, or npos when everything
	// before it is space. Used to read what sits immediately in front of a
	// call, which is where a guard lives.
	std::size_t previousNonSpace(std::string const& code, std::size_t at)
	{
		while (at > 0)
		{
			--at;
			if (!isSpace(code[at])) return at;
		}
		return std::string::npos;
	}

	// The index at which the callee name of the call whose `(` is at `open`
	// begins: the identifier run immediately before the paren.
	std::size_t calleeStart(std::string const& code, std::size_t open)
	{
		std::size_t at = open;
		while (at > 0 && isIdentChar(code[at - 1])) --at;
		return at;
	}

	// The character in front of a call's callee, stepping over any `::`
	// qualifier so `ns::drawObjectSprite(` reads the same as the bare call.
	// A guard sits in front of the callee rather than in front of the paren,
	// so this is where a reader looking back for one starts.
	std::size_t beforeCallee(std::string const& code, std::size_t open)
	{
		auto cursor = calleeStart(code, open);
		for (;;)
		{
			auto const c = previousNonSpace(code, cursor);
			if (c == std::string::npos) return std::string::npos;
			if (code[c] != ':' || c < 1 || code[c - 1] != ':') return c;
			auto const qualifier = calleeStart(code, c - 1);
			if (qualifier == c - 1) return c;
			cursor = qualifier;
		}
	}

	// The [start, end) span between the outermost braces of the function
	// defined as `void <name>(`. A declaration of the same shape - the
	// signature closing on a `;` rather than opening a body - is stepped
	// over, so only a definition answers. A file that defines none reads as
	// no span, and every caller turns that into a failure naming the
	// function, so a rename retires none of these checks quietly.
	std::pair<std::size_t, std::size_t> functionBody(ScannedSource const& source,
		std::string const& name)
	{
		auto const signature = "void " + name + "(";
		for (std::size_t at = source.code.find(signature);
			at != std::string::npos;
			at = source.code.find(signature, at + 1))
		{
			// The signature's own `(` is the last character of it, so the
			// parameter list closes where that call's parenthesis does - nested
			// parens and literals inside a default argument and all.
			auto const params = callClose(source.code, at + signature.size() - 1);
			if (params == std::string::npos) break;
			auto const opener = source.code.find_first_not_of(" \t\r\n", params + 1);
			if (opener == std::string::npos || source.code[opener] != '{') continue;
			std::size_t depth = 0;
			for (std::size_t i = opener; i < source.code.size(); ++i)
			{
				auto const c = source.code[i];
				if (c == '"') { i = literalEnd(source.code, i) - 1; continue; }
				if (c == '{') ++depth;
				else if (c == '}')
				{
					--depth;
					if (depth == 0) return { opener + 1, i };
				}
			}
			break;
		}
		return { std::string::npos, std::string::npos };
	}

	// The one sprite draw call inside a span, reported by its `(`. None or
	// several is reported rather than guessed at: the shape of the call site
	// is what the checks below are about, so a shape that moved is a
	// finding, not something to average over.
	std::size_t onlySpriteCall(ScannedSource const& source,
		std::pair<std::size_t, std::size_t> const& span, std::string const& what)
	{
		std::vector<std::size_t> calls;
		for (auto const open : drawCallOpens(source))
			if (open >= span.first && open < span.second) calls.push_back(open);
		require(calls.size() == 1,
			what + " makes " + std::to_string(calls.size())
			+ " sprite draw calls inside itself; this check reads exactly one");
		return calls.front();
	}

	// One palette tray slot that draws an Objects tile as its icon. The slot
	// checks are written once against this and run per slot, so the Window
	// slot getting the coverage the Door slot has had since #1063 (#1082) is
	// a row here rather than a second copy of every check.
	struct PaletteSlot
	{
		std::string function;  // the icon's function in the palette source
		std::string label;     // how a failure names the slot
		std::string noun;      // the tile's kind in prose: "door", "glass"
		std::string glyph;     // the font glyph the slot falls back to
	};

	PaletteSlot const kDoorSlot{ "drawDoorIcon", "The palette's Door icon",
		"door", "ICON_FA_DOOR_OPED" };
	PaletteSlot const kWindowSlot{ "drawWindowIcon", "The palette's Window icon",
		"glass", "ICON_FA_WINDOW_MAXIMIZE" };

	// A label reads mid-sentence as often as at the start of one, so the
	// sentence keeps its own capitalisation rather than the label's.
	std::string lowerFirst(std::string const& text)
	{
		if (text.empty()) return text;
		auto out = text;
		out[0] = static_cast<char>(tolower(static_cast<unsigned char>(out[0])));
		return out;
	}

	// The tile one palette slot draws, read out of the slot's own draw call
	// rather than from a constant the slot could stop using. Returns the
	// literal use, so the caller can name the line it came from.
	LiteralUse paletteIconTile(ScannedSource const& source, PaletteSlot const& slot)
	{
		auto const body = functionBody(source, slot.function);
		require(body.first != std::string::npos,
			kEditorPaletteSource + " no longer defines " + slot.function
			+ ", so " + lowerFirst(slot.label) + " is drawn by code this check "
			"does not read");
		auto const open = onlySpriteCall(source, body, slot.label);
		auto const asked = namesAskedBy(source, open);
		require(asked.size() == 1, slot.label
			+ "'s sprite call does not name exactly one tile, so there is no "
			"single tile to hold the slot's artwork to");
		return asked.front();
	}

	LiteralUse paletteIconTile(smoke::Context const& context, PaletteSlot const& slot)
	{
		return paletteIconTile(scanSource(context, kEditorPaletteSource), slot);
	}

	// One `<var> = objectSpriteSize("tile")` read: the variable the region
	// lands in and the tile it was asked for.
	struct RegionRead
	{
		std::string variable;
		LiteralUse tile;
	};

	// The `<var> = objectSpriteSize("tile")` reads an icon takes from the
	// installed atlas, within [from, to). The variable name comes back with
	// each tile literal so the caller can hold the region read and the draw
	// call to the same tile. An empty result is the shape that lets a
	// remembered aspect constant back in (#1082); several is reported rather
	// than guessed at, because the check cannot tell which one sizes the icon.
	std::vector<RegionRead> regionReadsIn(ScannedSource const& source,
		std::size_t from, std::size_t to)
	{
		std::vector<RegionRead> reads;
		std::string const call{ "objectSpriteSize(" };
		to = std::min(to, source.code.size());
		for (std::size_t at = from; at + call.size() <= to; ++at)
		{
			if (source.code.compare(at, call.size(), call) != 0) continue;
			if (at > from && isIdentChar(source.code[at - 1])) continue;
			auto const args = callArgumentSpans(source.code, at + call.size() - 1);
			require(!args.empty(),
				"an objectSpriteSize call takes no argument to read");
			auto const asked = trimmed(source.code.substr(args[0].first,
				args[0].second - args[0].first));
			require(asked.size() >= 2 && asked.front() == '"'
					&& asked.back() == '"',
				"objectSpriteSize is not asked for a literal tile name, so the "
				"region it returns cannot be traced to one tile");
			RegionRead read;
			read.tile = { asked.substr(1, asked.size() - 2),
				source.lineOf(args[0].first), false };
			// The variable the region lands in: the identifier on the far side
			// of the `=` that introduces the call. A comparison rather than an
			// assignment says the result was tested and thrown away, which sizes
			// nothing.
			std::size_t i = at;
			while (i > from && isSpace(source.code[i - 1])) --i;
			require(i > from && source.code[i - 1] == '=',
				"the objectSpriteSize result is not assigned, so nothing "
				"downstream is sized from the tile's region");
			require(i < 2 || (source.code[i - 2] != '=' && source.code[i - 2] != '!'
					&& source.code[i - 2] != '<' && source.code[i - 2] != '>'),
				"the objectSpriteSize result is compared rather than assigned, so "
				"nothing downstream is sized from the tile's region");
			std::size_t end = i - 1;
			while (end > from && isSpace(source.code[end - 1])) --end;
			std::size_t start = end;
			while (start > from && isIdentChar(source.code[start - 1])) --start;
			require(start < end,
				"no variable name sits on the left of the objectSpriteSize read");
			read.variable = source.code.substr(start, end - start);
			reads.push_back(std::move(read));
		}
		return reads;
	}

	// `<var>` followed by `->` or `.` and an axis letter, starting at `at`.
	// Returns the axis and the offset just past it.
	std::optional<std::pair<char, std::size_t>> axisAt(ScannedSource const& source,
		std::string const& var, std::size_t at, std::size_t to)
	{
		if (at + var.size() > to) return std::nullopt;
		if (source.code.compare(at, var.size(), var) != 0) return std::nullopt;
		std::size_t i = at + var.size();
		if (i + 1 < to && source.code[i] == '-' && source.code[i + 1] == '>') i += 2;
		else if (i < to && source.code[i] == '.') ++i;
		else return std::nullopt;
		if (i >= to) return std::nullopt;
		auto const axis = source.code[i];
		if (axis != 'x' && axis != 'y') return std::nullopt;
		return std::make_pair(axis, i + 1);
	}

	// One `<var>-><axis> / <var>-><axis>` ratio read out of an icon's body.
	struct AxisRatio
	{
		char numerator{ 0 };
		char denominator{ 0 };
		std::size_t line{ 0 };
	};

	// The ratios an icon builds its drawn size from. Either axis order is
	// read; the check that owns this decides whether the ratio is the tile's
	// aspect or its inverse, and reports several rather than guessing at one.
	std::vector<AxisRatio> axisRatiosOf(ScannedSource const& source,
		std::string const& var, std::size_t from, std::size_t to)
	{
		std::vector<AxisRatio> ratios;
		to = std::min(to, source.code.size());
		for (std::size_t at = from; at < to; ++at)
		{
			if (at > from && isIdentChar(source.code[at - 1])) continue;
			auto const numerator = axisAt(source, var, at, to);
			if (!numerator) continue;
			std::size_t j = numerator->second;
			while (j < to && isSpace(source.code[j])) ++j;
			if (j >= to || source.code[j] != '/') continue;
			++j;
			while (j < to && isSpace(source.code[j])) ++j;
			auto const denominator = axisAt(source, var, j, to);
			if (!denominator) continue;
			ratios.push_back(AxisRatio{ numerator->first, denominator->first,
				source.lineOf(at) });
			// A ratio's two operands cannot start another ratio inside
			// themselves, so resume past the denominator.
			at = denominator->second - 1;
		}
		return ratios;
	}

	// The tiles this ticket's contract is about, in the order the renderer
	// names them. Kept here as a list rather than derived so a tile dropped
	// from the contract shows up as a missing name rather than as nothing.
	std::vector<std::string> const kRequiredTiles{
		"door-L0", "door-R0",
		"door-tall-L0", "door-tall-R0",
		"door-lift-L0", "door-lift-R0", "door-shuttle-L0",
		"bulkhead-door", "access-panel",
		"glass-clear", "glass-frosted", "glass-tinted",
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

	// A palette slot draws a tile the shipped atlas carries and one the
	// Objects contract covers. #1063 is what the gap cost the Door slot: the
	// bare `door` tile the icon asked for was deleted by #1056, the sprite
	// call returned false exactly as designed, and the slot became a font
	// glyph with the suite green. The reverse check above refuses a drawn
	// name the installed atlas does not carry; this pins the slot's own
	// shape - one tile named, that tile required - so a slot cannot drift
	// onto an artwork name nobody sized, and reports it as the palette slot
	// it is rather than as a line number in a ten-thousand-line file.
	void paletteIconNamesACarriedRequiredTile(smoke::Context const& context,
		PaletteSlot const& slot)
	{
		auto const icon = paletteIconTile(context, slot);
		auto const where = kEditorPaletteSource + ":" + std::to_string(icon.line);

		auto const atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		require(atlas.sprites.count(icon.text) != 0,
			lowerFirst(slot.label) + " asks the Objects slot for '" + icon.text
			+ "' (" + where + ") and the shipped Objects atlas does not carry it, "
			"so the slot draws its font fallback rather than tile artwork (#1063)");

		bool required = false;
		for (auto const& expected : requiredObjectTiles())
			if (icon.text == expected.tileName) required = true;
		require(required,
			slot.label + " draws '" + icon.text + "' (" + where
			+ "), which is not in requiredObjectTiles(), so no atlas contract "
			"covers the tile the slot is drawn from and an atlas could be "
			"selected that carries it at an extent nothing checked");
	}

	void doorPaletteIconNamesACarriedRequiredTile(smoke::Context const& context)
	{
		paletteIconNamesACarriedRequiredTile(context, kDoorSlot);
	}

	void windowPaletteIconNamesACarriedRequiredTile(smoke::Context const& context)
	{
		paletteIconNamesACarriedRequiredTile(context, kWindowSlot);
	}

	// The slot keeps its font-glyph fallback, and keeps it where a miss
	// reaches it: the sprite call tested for failure, and the glyph drawn as
	// the very next statement. That fallback is what the custom version 1
	// theme sets draw for these slots - neither carries the tile - so a slot
	// that lost it would draw an empty tray cell under those themes instead
	// of the glyph it drew before the tile existed at all (#1063).
	void paletteIconFallsBackToTheGlyphOnAMiss(smoke::Context const& context,
		PaletteSlot const& slot)
	{
		auto const source = scanSource(context, kEditorPaletteSource);
		auto const body = functionBody(source, slot.function);
		require(body.first != std::string::npos,
			kEditorPaletteSource + " no longer defines " + slot.function
			+ ", so " + lowerFirst(slot.label) + " is drawn by code this check "
			"does not read");
		auto const open = onlySpriteCall(source, body, slot.label);

		auto const bang = beforeCallee(source.code, open);
		require(bang != std::string::npos && source.code[bang] == '!',
			slot.label + " does not test its sprite call's answer, so a set with "
			"no " + slot.noun + " tile leaves the slot blank instead of falling "
			"back to the glyph (#1063)");
		auto const guard = previousNonSpace(source.code, bang);
		require(guard != std::string::npos && source.code[guard] == '(',
			slot.label + "'s test of its sprite call is not an `if`, so a miss "
			"is routed nowhere (#1063)");

		auto const guardClose = callClose(source.code, guard);
		require(guardClose != std::string::npos && guardClose < body.second,
			slot.label + "'s guard does not close inside its own body, so its "
			"fallback cannot be read (#1063)");
		std::string const fallbackCall{ "drawObjectIcon" };
		// The statement the guard routes a miss to is the one the guard's own
		// parenthesis closes over. Reading past the sprite call instead would
		// land on the guard's closing paren and call it a statement.
		auto const next = source.code.find_first_not_of(" \t\r\n", guardClose + 1);
		require(next != std::string::npos && next < body.second,
			slot.label + "'s guard has no statement to route a miss to, so a set "
			"with no " + slot.noun + " tile leaves the slot blank (#1063)");
		// A braced body around the fallback says the same thing with two more
		// characters, so read past it rather than failing on the shape.
		auto const statement = source.code[next] == '{'
			? source.code.find_first_not_of(" \t\r\n", next + 1) : next;
		require(statement != std::string::npos && statement < body.second
				&& source.code.compare(statement, fallbackCall.size(), fallbackCall) == 0,
			"The statement after " + lowerFirst(slot.label) + "'s sprite call is "
			"not a drawObjectIcon fallback, so a miss draws nothing at all (#1063)");
		auto const fallbackArgs = callArgumentSpans(source.code,
			statement + fallbackCall.size());
		require(!fallbackArgs.empty(),
			slot.label + "'s fallback call takes no arguments to read (#1063)");
		auto const last = trimmed(source.code.substr(fallbackArgs.back().first,
			fallbackArgs.back().second - fallbackArgs.back().first));
		require(last == slot.glyph,
			slot.label + "'s fallback names '" + last + "' rather than the "
			+ slot.noun + " glyph, so the slot's fallback picture is not the "
			+ slot.noun + " it fell back to before the tile existed (#1063)");
	}

	void doorPaletteIconFallsBackToTheGlyphOnAMiss(smoke::Context const& context)
	{
		paletteIconFallsBackToTheGlyphOnAMiss(context, kDoorSlot);
	}

	void windowPaletteIconFallsBackToTheGlyphOnAMiss(smoke::Context const& context)
	{
		paletteIconFallsBackToTheGlyphOnAMiss(context, kWindowSlot);
	}

	// The mechanism the slot's guard tests, run against real atlases rather
	// than read off the source: the tile the slot names resolves to Object
	// Atlas geometry while the shipped set is installed, and stops resolving
	// the moment a set without it is. That second state is the one the
	// custom version 1 theme sets leave these slots in, and the check above
	// is what makes them draw the glyph rather than nothing (#1063).
	void paletteIconTileResolvesOnlyWhenTheAtlasCarriesIt(
		smoke::Context const& context, PaletteSlot const& slot)
	{
		auto const icon = paletteIconTile(context, slot);
		auto const shipped = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		setObjectTileset(shipped, static_cast<TextureId>(1));
		WorldDrawList drawn(WorldDrawList::ClipRectangle{{-1000, -1000}, {1000, 1000}});
		require(drawObjectSprite(icon.text.c_str(), &drawn, {10.0f, 10.0f},
			{62.0f, 90.0f}),
			"The sprite call refused '" + icon.text + "' from the shipped Objects "
			"atlas, so " + lowerFirst(slot.label) + " takes its font fallback "
			"under the very set the slot names (#1063)");
		std::size_t quads = 0;
		for (auto const& command : drawn.commands())
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas)
				++quads;
		require(quads == 2,
			lowerFirst(slot.label) + "'s tile drew " + std::to_string(quads)
			+ " Object Atlas triangles rather than one quad, so the slot is not "
			"drawing tile artwork (#1063)");

		auto without = shipped;
		without.sprites.erase(icon.text);
		require(without.sprites.count(icon.text) == 0,
			"Erasing '" + icon.text + "' left it in the set, so this check would "
			"not be testing a miss (#1063)");
		setObjectTileset(std::move(without), static_cast<TextureId>(1));
		WorldDrawList missing(WorldDrawList::ClipRectangle{{-1000, -1000}, {1000, 1000}});
		require(!drawObjectSprite(icon.text.c_str(), &missing, {10.0f, 10.0f},
			{62.0f, 90.0f}),
			"The sprite call reported a tile drawn from a set the tile was "
			"erased from, so " + lowerFirst(slot.label) + " would never reach "
			"its fallback (#1063)");
		clearObjectTileset();
	}

	void doorPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt(
		smoke::Context const& context)
	{
		paletteIconTileResolvesOnlyWhenTheAtlasCarriesIt(context, kDoorSlot);
	}

	void windowPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt(
		smoke::Context const& context)
	{
		paletteIconTileResolvesOnlyWhenTheAtlasCarriesIt(context, kWindowSlot);
	}

	// The Window palette slot draws its glass at the glass tile's own aspect.
	// What the slot hardcoded after #1079 retired the pane-sized
	// `window-clear` tile was that tile's 52/48, which drew the 32 x 80
	// glass tile about 2.7x wider than it was authored - invisible under the
	// flat placeholder slab, which has nothing in it to distort, and wrong
	// the moment an artist puts real glass on the tile, in the one place
	// they look to see what their tile reads as (#1082).
	//
	// What is refused here: a remembered number, because the aspect has to be
	// read from the installed tile's region so a re-cut of the art moves the
	// icon with it instead of leaving it stretched; a region read that names
	// a tile other than the one drawn, which fits the icon to somebody else's
	// shape; a drawn size not built from that region at all, or built from
	// more than one candidate this check could not choose between; and a
	// ratio that does not resolve to the region's width over its height,
	// which is the same defect turned on its side.
	//
	// The Door slot is deliberately not read here. Its constant is a smaller
	// mismatch against a full-cell leaf, and holding it to the same rule is a
	// change of its own to make rather than a free rider on this one.
	void windowPaletteIconDrawsAtTheTileOwnAspect(smoke::Context const& context)
	{
		auto const source = scanSource(context, kEditorPaletteSource);
		auto const body = functionBody(source, kWindowSlot.function);
		require(body.first != std::string::npos,
			kEditorPaletteSource + " no longer defines " + kWindowSlot.function
			+ ", so " + lowerFirst(kWindowSlot.label) + " is drawn by code this "
			"check does not read");
		auto const icon = paletteIconTile(source, kWindowSlot);
		auto const where = kEditorPaletteSource + ":" + std::to_string(icon.line);

		auto const reads = regionReadsIn(source, body.first, body.second);
		require(!reads.empty(),
			lowerFirst(kWindowSlot.label) + " reads no tile region from the "
			"installed atlas, so its aspect is a remembered number rather than "
			"the tile's own, and a re-cut of the glass would leave the icon "
			"stretched again (" + where + ") (#1082)");
		require(reads.size() == 1,
			kWindowSlot.label + " reads " + std::to_string(reads.size())
			+ " tile regions from the installed atlas, so this check cannot tell "
			"which one the drawn aspect is fitted to (#1082)");
		auto const& read = reads.front();
		require(read.tile.text == icon.text,
			kWindowSlot.label + " sizes itself from '" + read.tile.text
			+ "' but draws '" + icon.text + "', so the aspect it fits to "
			"describes a different tile than the one it draws (" + where
			+ ") (#1082)");

		auto const ratios = axisRatiosOf(source, read.variable, body.first,
			body.second);
		require(!ratios.empty(),
			kWindowSlot.label + " does not build its drawn size from a ratio of "
			"the region it read ('" + read.variable + "' over an axis of it), so "
			"the drawn aspect cannot be read against the tile's region (#1082)");
		require(ratios.size() == 1,
			kWindowSlot.label + " builds " + std::to_string(ratios.size())
			+ " ratios from the region it read, so this check cannot tell which "
			"one the drawn size is fitted to (#1082)");
		auto const& ratio = ratios.front();

		// Resolve the ratio against the shipped atlas's region for the tile
		// the slot draws, so the numbers compared are the artwork's own
		// rather than ones this check remembered.
		auto const atlas = ObjectTileset::load(context.fixture(kNewObjectsTileset));
		setObjectTileset(atlas, static_cast<TextureId>(1));
		auto const region = objectSpriteSize(icon.text.c_str());
		clearObjectTileset();
		require(region && region->y > 0.0f,
			"objectSpriteSize('" + icon.text + "') reports no usable region "
			"under the shipped Objects atlas, so there is no tile aspect to "
			"hold " + lowerFirst(kWindowSlot.label) + " to (#1082)");
		auto const component = [region](char axis) {
			return axis == 'x' ? region->x : region->y;
		};
		auto const tileAspect = region->x / region->y;
		auto const drawnAspect = component(ratio.numerator)
			/ component(ratio.denominator);
		require(std::abs(drawnAspect - tileAspect) <= 1e-4f,
			kWindowSlot.label + " draws '" + icon.text + "' at aspect "
			+ std::to_string(drawnAspect) + " (" + ratio.numerator + " over "
			+ ratio.denominator + ") while the installed tile's region is "
			+ std::to_string(static_cast<int>(region->x)) + " x "
			+ std::to_string(static_cast<int>(region->y)) + ", aspect "
			+ std::to_string(tileAspect) + ", so the tile is stretched on its "
			"way to the palette tray (" + kEditorPaletteSource + ":"
			+ std::to_string(ratio.line) + ") (#1082)");
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
		// The glass tile is its own 0.25 x 0.25 extent and not the pane's:
		// the pane is filled by repetition, so what the contract sizes is the
		// tile that repeats. Being square in World units on a 128 x 320 grid
		// is the whole reason the artwork is 32 x 80 px rather than square in
		// pixels - the sheet draws at 128 px per World unit across and 320 up,
		// so only those dimensions land the pattern undistorted at one artwork
		// pixel to one screen pixel (#1079).
		for (auto const* glass : { "glass-clear", "glass-frosted",
			"glass-tinted" })
			require(near(find(glass).drawnWidth, 0.25f)
					&& near(find(glass).drawnHeight, 0.25f),
				std::string("The '") + glass + "' glass expectation is not the "
					"0.25 x 0.25 World-unit tile that repeats across a pane");
		// The pane-sized Window tiles are retired. Leaving them required would
		// hold the atlas to art sized to one pane and stretched to fit it -
		// the defect this ticket is about - and would refuse the set that
		// replaced them.
		for (auto const* retired : { "window-clear", "window-frosted",
			"window-tinted" })
			for (auto const& expected : requiredObjectTiles())
				require(!(std::string(retired) == expected.tileName),
					std::string("The Objects contract still requires '") + retired
					+ "', the pane-sized glass tile #1079 retired");
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
	checks.push_back({ "objectAtlasContract/doorPaletteIconNamesACarriedRequiredTile",
		isolated<doorPaletteIconNamesACarriedRequiredTile> });
	checks.push_back({ "objectAtlasContract/doorPaletteIconFallsBackToTheGlyphOnAMiss",
		isolated<doorPaletteIconFallsBackToTheGlyphOnAMiss> });
	checks.push_back({ "objectAtlasContract/doorPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt",
		isolated<doorPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt> });
	checks.push_back({ "objectAtlasContract/windowPaletteIconNamesACarriedRequiredTile",
		isolated<windowPaletteIconNamesACarriedRequiredTile> });
	checks.push_back({ "objectAtlasContract/windowPaletteIconFallsBackToTheGlyphOnAMiss",
		isolated<windowPaletteIconFallsBackToTheGlyphOnAMiss> });
	checks.push_back({ "objectAtlasContract/windowPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt",
		isolated<windowPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt> });
	checks.push_back({ "objectAtlasContract/windowPaletteIconDrawsAtTheTileOwnAspect",
		isolated<windowPaletteIconDrawsAtTheTileOwnAspect> });
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
