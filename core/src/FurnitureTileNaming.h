#pragma once

#include <string>
#include <vector>

namespace core
{
	struct FurnitureTile;

	// The tile naming convention (#1016).
	//
	// Two position vocabularies exist, and a Furniture piece speaks one of
	// them throughout:
	//
	//   * `-XY` is an absolute offset from the anchor, retained as the
	//     escape hatch for irregular footprints.
	//   * `-Ly` / `-My` / `-Ry` is a relative role plus a height level,
	//     which is what makes a middle tile reusable at any width: the name
	//     says what the tile is for, not where one particular width put it.
	//
	// The anchor is the bottom-left of the piece's footprint, so every
	// offset is non-negative and no sign has to be encoded in a name. One
	// digit per axis carries the offsets 0 through 9, which caps a piece at
	// ten cells per axis - ten columns and ten rows. An eleventh column or
	// row cannot be named, and the loader refuses it.
	//
	// The catalogue's per-tile `{x, y}` stays authoritative for placement.
	// The suffix earns its place by making the art sheet self-describing,
	// and the loader asserting agreement is what stops that becoming a lie.
	enum class FurnitureTileVocabulary
	{
		Bare,     // No position suffix: a single-cell object's name.
		Role,     // `-Ly`, `-My`, `-Ry`.
		Absolute, // `-XY`.
	};

	struct FurnitureTileName
	{
		FurnitureTileVocabulary vocabulary{FurnitureTileVocabulary::Bare};
		std::string base;
		char role{};                 // 'L', 'M' or 'R' for the Role vocabulary
		int level{};                 // the height level the role digit names
		int offsetX{}, offsetY{};    // the `-XY` digits
	};

	// Splits a tile name into its base and its position suffix. Never
	// refuses: a name carrying no recognized suffix reads as Bare, which is
	// how the old artwork's names - `chair`, `sofa-left`, `button-enabled`
	// - keep loading while the convention is introduced.
	FurnitureTileName parseFurnitureTileName(std::string const& name);

	// Validates one definition's whole footprint against its tile names,
	// and throws SerializationException when the piece speaks a position
	// vocabulary and anything disagrees: the two vocabularies mixed, a bare
	// name in a suffixed footprint, a negative or two-digit offset, a role
	// at a position its width forbids, a level the name does not match, or
	// a role missing at a level the footprint reaches.
	void validateFurnitureTileNames(std::string const& definitionKey,
		std::vector<FurnitureTile> const& tiles);
}
