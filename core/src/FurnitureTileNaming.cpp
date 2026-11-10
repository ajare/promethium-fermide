#include "FurnitureTileNaming.h"

#include "core/Furniture.h"
#include "core/SerializationException.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace core
{
	namespace
	{
		// One digit per axis carries the offsets 0 through 9, so the widest
		// and tallest piece the convention can name is ten cells per axis -
		// ten columns and ten rows. An offset past the digit's reach names
		// nothing and is refused below.
		int const kMaxAxisDigit = 9;
		int const kMaxAxisCells = kMaxAxisDigit + 1;

		bool isRoleDigit(char const* suffix, char& role, int& digit)
		{
			if (suffix[0] != 'L' && suffix[0] != 'M' && suffix[0] != 'R') return false;
			if (suffix[1] < '0' || suffix[1] > '9') return false;
			role = suffix[0];
			digit = suffix[1] - '0';
			return true;
		}

		bool isAxisDigit(char c) { return c >= '0' && c <= '9'; }

		char const* vocabularyName(FurnitureTileVocabulary vocabulary)
		{
			switch (vocabulary)
			{
			case FurnitureTileVocabulary::Role: return "role";
			case FurnitureTileVocabulary::Absolute: return "absolute";
			default: return "bare";
			}
		}
	}

	FurnitureTileName parseFurnitureTileName(std::string const& name)
	{
		FurnitureTileName parsed;
		parsed.base = name;
		// The suffix is the last two characters after a `-`, and the base has
		// to survive: `-L0` names no piece at all, so it reads as a bare name
		// rather than as a role on nothing.
		if (name.size() < 4 || name[name.size() - 3] != '-') return parsed;
		auto const suffix = name.c_str() + name.size() - 2;
		char role{};
		int digit{};
		if (isRoleDigit(suffix, role, digit))
		{
			parsed.vocabulary = FurnitureTileVocabulary::Role;
			parsed.role = role;
			parsed.level = digit;
			parsed.base = name.substr(0, name.size() - 3);
			return parsed;
		}
		if (isAxisDigit(suffix[0]) && isAxisDigit(suffix[1]))
		{
			parsed.vocabulary = FurnitureTileVocabulary::Absolute;
			parsed.offsetX = suffix[0] - '0';
			parsed.offsetY = suffix[1] - '0';
			parsed.base = name.substr(0, name.size() - 3);
			return parsed;
		}
		return parsed;
	}

	void validateFurnitureTileNames(std::string const& definitionKey,
		std::vector<FurnitureTile> const& tiles)
	{
		// A piece that speaks no position vocabulary is the old artwork's
		// authoring - `chair`, `sofa-left`, `bed-right` - and stays exactly
		// as it loads. The convention binds the moment a piece speaks it.
		FurnitureTileVocabulary vocabulary{FurnitureTileVocabulary::Bare};
		for (auto const& tile : tiles)
		{
			auto const parsed = parseFurnitureTileName(tile.image);
			if (parsed.vocabulary == FurnitureTileVocabulary::Bare) continue;
			if (vocabulary == FurnitureTileVocabulary::Bare)
			{
				vocabulary = parsed.vocabulary;
				continue;
			}
			if (parsed.vocabulary != vocabulary)
				throw SerializationException("Furniture '" + definitionKey
					+ "' mixes " + std::string(vocabularyName(vocabulary))
					+ " and " + vocabularyName(parsed.vocabulary)
					+ " tile vocabularies; one piece speaks one vocabulary ('"
					+ tile.image + "')");
		}
		if (vocabulary == FurnitureTileVocabulary::Bare) return;

		// A bare name in a suffixed footprint names no position, which is
		// the one thing a piece speaking the convention cannot afford.
		for (auto const& tile : tiles)
			if (parseFurnitureTileName(tile.image).vocabulary
				== FurnitureTileVocabulary::Bare)
				throw SerializationException("Furniture '" + definitionKey
					+ "' leaves tile '" + tile.image
					+ "' unsuffixed inside a footprint that names its tiles by "
					"position");

		int const width = [] (std::vector<FurnitureTile> const& in)
		{
			int max{};
			for (auto const& tile : in) max = std::max(max, tile.x);
			return max + 1;
		}(tiles);
		int const height = [] (std::vector<FurnitureTile> const& in)
		{
			int max{};
			for (auto const& tile : in) max = std::max(max, tile.y);
			return max + 1;
		}(tiles);

		// The anchor is the bottom-left of the footprint, so no offset is
		// negative and one digit per axis is enough to name any of them.
		for (auto const& tile : tiles)
		{
			if (tile.x < 0 || tile.y < 0)
				throw SerializationException("Furniture '" + definitionKey
					+ "' tile '" + tile.image
					+ "' is offset negatively from its anchor; the anchor is the "
					"footprint's bottom-left");
			if (tile.x > kMaxAxisDigit || tile.y > kMaxAxisDigit)
				throw SerializationException("Furniture '" + definitionKey
					+ "' tile '" + tile.image
					+ "' sits past the one digit per axis the naming convention "
					"carries; offsets run 0 through " + std::to_string(kMaxAxisDigit)
					+ ", which caps a piece at " + std::to_string(kMaxAxisCells)
					+ " cells per axis");
		}

		if (vocabulary == FurnitureTileVocabulary::Absolute)
		{
			for (auto const& tile : tiles)
			{
				auto const parsed = parseFurnitureTileName(tile.image);
				if (parsed.offsetX != tile.x || parsed.offsetY != tile.y)
					throw SerializationException("Furniture '" + definitionKey
						+ "' tile '" + tile.image + "' names offset "
						+ std::to_string(parsed.offsetX) + ","
						+ std::to_string(parsed.offsetY)
						+ " but the catalogue places it at "
						+ std::to_string(tile.x) + "," + std::to_string(tile.y));
			}
			return;
		}

		// Role vocabulary. The name says what the tile is for; the footprint's
		// width says where that role can sit.
		std::set<std::pair<char, int>> roles;
		for (auto const& tile : tiles)
		{
			auto const parsed = parseFurnitureTileName(tile.image);
			if (parsed.level != tile.y)
				throw SerializationException("Furniture '" + definitionKey
					+ "' tile '" + tile.image + "' names height level "
					+ std::to_string(parsed.level) + " but the catalogue places it "
					"at level " + std::to_string(tile.y));
			roles.insert({ parsed.role, parsed.level });
		}

		// `L` is always present because it is the anchor, `R` appears when
		// width >= 2, `M` appears when width >= 3 - and for height > 1 every
		// role present must be present at every height level, so a footprint
		// can never be half-specified. This is asked before any position rule
		// so that a piece missing its right cap is reported as missing its
		// right cap rather than as having its middle in the wrong place.
		for (int level = 0; level < height; ++level)
		{
			auto const missing = [&](char role)
			{
				return !roles.count({ role, level });
			};
			if (missing('L'))
				throw SerializationException("Furniture '" + definitionKey
					+ "' has no L tile at height level " + std::to_string(level)
					+ "; L is the anchor and is always present");
			if (width >= 2 && missing('R'))
				throw SerializationException("Furniture '" + definitionKey
					+ "' is " + std::to_string(width) + " cells wide but has no R "
					"tile at height level " + std::to_string(level)
					+ "; a piece at least two cells wide needs its right cap");
			if (width >= 3 && missing('M'))
				throw SerializationException("Furniture '" + definitionKey
					+ "' is " + std::to_string(width) + " cells wide but has no M "
					"tile at height level " + std::to_string(level)
					+ "; a piece at least three cells wide needs its middle");
		}

		// A left cap in the middle of a sofa is the mistake this refuses.
		for (auto const& tile : tiles)
		{
			auto const parsed = parseFurnitureTileName(tile.image);
			switch (parsed.role)
			{
			case 'L':
				if (tile.x != 0)
					throw SerializationException("Furniture '" + definitionKey
						+ "' left cap '" + tile.image
						+ "' is not the anchor: the anchor is the footprint's "
						"bottom-left, so L belongs at x = 0");
				break;
			case 'R':
				if (tile.x != width - 1)
					throw SerializationException("Furniture '" + definitionKey
						+ "' right cap '" + tile.image + "' is at x = "
						+ std::to_string(tile.x) + " in a footprint "
						+ std::to_string(width) + " cells wide, which forbids it");
				break;
			default:
				if (width < 3 || tile.x < 1 || tile.x > width - 2)
					throw SerializationException("Furniture '" + definitionKey
						+ "' middle tile '" + tile.image + "' is at x = "
						+ std::to_string(tile.x) + " in a footprint "
						+ std::to_string(width)
						+ " cells wide, which forbids it");
				break;
			}
		}
	}
}
