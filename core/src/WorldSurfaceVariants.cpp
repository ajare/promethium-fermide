// Recording a variant on one live cell of a Sector (#1042).
//
// #1040 put the entries on the Sector and on its construction record, and
// taught the loader to refuse a bad one. This is the other half: the way an
// author puts one there without editing the file by hand.
//
// Two things are written, and both are needed. The live Sector is what the
// renderer reads this frame, so painting it is what makes the cell change
// under the cursor immediately. The creating record is what the World saves
// and what the next rebuild replays - and a rebuild is not some far-off
// event, it is what every structural edit does - so an entry written only
// to the live Sector would be artwork with an expiry date.
//
// The clearing request (`variant == std::nullopt`) removes the entry rather
// than writing a zero. A Sector painted back to its base and a Sector that
// never wore a variant are the same thing, and they save the same way: with
// no field at all. A stored zero would be a third state that means the base
// but is not the base, and every reader would have to know it.
//
// Nothing here touches traversal, capacity or simulation state, which is
// what the domain says a variant is. So the edit needs no pause and no
// rebuild: it is not a topology change and it should not ask for one.

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "core/Sector.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	uint32_t World::rekeySurfaceVariantsForResize(
		std::vector<SectorSurfaceVariant>& entries,
		int64_t oldAnchorX, int64_t oldAnchorY,
		uint32_t oldCellsWide, uint32_t oldLevelsHigh,
		int64_t newAnchorX, int64_t newAnchorY,
		uint32_t cellsWide, uint32_t levelsHigh)
	{
		// A move is not a resize. It translates the whole Sector and the artwork
		// rides along with it, so the local keys that named the Sector's cells
		// before still name them, and there is nothing to re-key and nothing to
		// drop. Asked the other way - keep the artwork on the World cells - a move
		// would drop every entry the moment it left the old footprint, which is
		// the opposite of what an author means by picking the Sector up.
		if (cellsWide == oldCellsWide && levelsHigh == oldLevelsHigh)
			return 0;

		// Walked into a new list rather than filtered in place, because the answer
		// for each entry is a new pair of keys as much as it is a keep-or-drop
		// decision. The order the entries were already in is carried over, so a
		// resize that takes three cells out does not shuffle the forty the Sector
		// still has - and a resize whose anchor never moved leaves the list equal
		// to what it was, which is what lets a saved document say nothing
		// changed.
		std::vector<SectorSurfaceVariant> kept;
		kept.reserve(entries.size());
		for (auto const& entry : entries)
		{
			// Signed throughout. The World cell the entry was painted on is the
			// old anchor plus the local key; the local key that names that same
			// cell in the new footprint is that minus the new anchor. When the
			// edit deleted the World cell the difference is negative, and an
			// unsigned subtraction here would wrap to something huge that the
			// extent check catches by luck on one axis and quietly misplaces on
			// the other.
			auto const column = oldAnchorX + static_cast<int64_t>(entry.column)
				- newAnchorX;
			auto const level = oldAnchorY + static_cast<int64_t>(entry.level)
				- newAnchorY;
			if (column < 0 || level < 0
				|| column >= static_cast<int64_t>(cellsWide)
				|| level >= static_cast<int64_t>(levelsHigh))
				continue;
			kept.push_back({ static_cast<uint32_t>(column),
				static_cast<uint32_t>(level), entry.variant });
		}
		auto const dropped = static_cast<uint32_t>(entries.size() - kept.size());
		entries = std::move(kept);
		return dropped;
	}

	void World::appendSurfaceVariantDropConsequence(
		std::vector<std::string>& consequences,
		std::vector<SectorSurfaceVariant> const& entries,
		int64_t oldAnchorX, int64_t oldAnchorY,
		uint32_t oldCellsWide, uint32_t oldLevelsHigh,
		int64_t newAnchorX, int64_t newAnchorY,
		uint32_t cellsWide, uint32_t levelsHigh)
	{
		// Counted on a copy through the re-key the record rewrite will perform,
		// so one answer is computed one way and every edit path that announces a
		// loss announces exactly what goes (#1044, carried to the vertical
		// Transits by #1045 and to the moving anchor by #1047). A footprint that
		// loses nothing says nothing: a move and a grow cost no artwork and should
		// not ask about any. A resize that leaves none of the old cells behind
		// loses every entry, and the line says all of them rather than letting a
		// total loss look like a clean edit.
		auto planned = entries;
		if (auto const dropped = rekeySurfaceVariantsForResize(planned,
				oldAnchorX, oldAnchorY, oldCellsWide, oldLevelsHigh,
				newAnchorX, newAnchorY, cellsWide, levelsHigh))
			consequences.push_back(format("Drop {} variant {} outside the new footprint",
				dropped, dropped == 1 ? "cell" : "cells"));
	}

	World::ConstructionRecord const* World::findCreatingRecord(uint32_t sectorIndex) const
	{
		uint32_t producer = 0;
		for (auto const& record : mConstructionRecords)
		{
			if (!constructionTypeCreatesSector(record.type)) continue;
			if (producer++ == sectorIndex) return &record;
		}
		return nullptr;
	}

	bool World::canSetSectorSurfaceVariant(uint32_t sectorIndex, uint32_t column,
		uint32_t level, optional<uint32_t> variant, string* diagnostic) const
	{
		auto reject = [&](string text)
			{ if (diagnostic) *diagnostic = std::move(text); return false; };

		if (sectorIndex >= mSectors.size())
			return reject("There is no Sector " + to_string(sectorIndex)
				+ " in this World");
		auto const& sector = mSectors[sectorIndex];

		// A Sector with no creating record has nowhere for the entry to be
		// saved, so accepting it would draw today and be gone tomorrow with no
		// word said. Every Sector a World builds has a producing record, so
		// this refuses a broken wiring rather than an ordinary case.
		auto const* record = findCreatingRecord(sectorIndex);
		if (!record)
			return reject(sector->getName() + " was not created by a construction record, so a variant recorded on it could not be saved");

		// The same rule the loader applies (#1040): a Background or a Facade
		// is authored as a flat colour and never asks the atlas for a tile, so
		// an entry on one is artwork that could never show. Refusing it here
		// keeps a hand-edited document and a painted World agreeing about what
		// a variant means.
		if (!constructionTypeCarriesSurfaceVariants(record->type))
			return reject("A " + constructionTypeName(record->type)
				+ " draws no tiled surface, so it cannot wear a variant");

		// The same bounds the loader refuses, in the same Sector-local frame.
		// An entry outside the grid names a cell that does not exist, and
		// drawing nothing for it is a silent failure where a refusal is a
		// loud one.
		if (column >= sector->getCellsWide() || level >= sector->getLevelsHigh())
			return reject(format("Column {}, level {} is outside the {}'s {} cells wide by {} levels high",
				column, level, sector->getName(), sector->getCellsWide(),
				sector->getLevelsHigh()));

		// Variant 0 is not the base. The base is the absence of an entry, and
		// a caller that means the base asks with nothing rather than with a
		// number that would be saved, replayed and read as a variant the
		// artwork has no row for.
		if (variant && *variant == 0)
			return reject("The base tile is recorded as no entry at all, not as variant 0");

		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setSectorSurfaceVariant(uint32_t sectorIndex, uint32_t column,
		uint32_t level, optional<uint32_t> variant, string* diagnostic)
	{
		string reason;
		if (!canSetSectorSurfaceVariant(sectorIndex, column, level, variant, &reason))
		{
			if (diagnostic) *diagnostic = reason;
			return false;
		}

		auto& record = mConstructionRecords[static_cast<size_t>(
			findCreatingRecord(sectorIndex) - mConstructionRecords.data())];
		auto& sector = mSectors[sectorIndex];

		// Rebuilt rather than patched in place. One cell's entry is replaced or
		// removed and every other entry is carried over untouched, in the order
		// it was already in, so a Sector's saved list does not shuffle between
		// one click and the next.
		auto entries = record.surfaceVariants;
		std::erase_if(entries, [&](auto const& entry)
			{ return entry.column == column && entry.level == level; });
		if (variant) entries.push_back({ column, level, *variant });

		record.surfaceVariants = entries;
		sector->setSurfaceVariants(entries);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
}
