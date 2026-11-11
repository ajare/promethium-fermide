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
