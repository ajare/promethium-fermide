#include "StationSystemsEntryEditor.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace station_editor
{
// The draft is carried into the per-entry editors as the shared seam's
// context: the entry is always one of the draft's own elements, so a
// surface that only holds the draft can edit any entry without owning
// the list around it. Today's fields never read across the draft; the
// parameter is the seam the fan-out tickets (#647-#651) extend.
bool renderStoreEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceStoreSettings& store,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char idBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(idBuffer, store.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(), idBuffer, sizeof(idBuffer)))
		store.id = idBuffer;
	char unitBuffer[8]{};
	std::strncpy(unitBuffer, store.unit.c_str(), sizeof(unitBuffer) - 1);
	if (ImGui::InputText((label + " unit").c_str(), unitBuffer, sizeof(unitBuffer)))
		store.unit = unitBuffer;
	char storeHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(storeHazardBuffer, store.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		storeHazardBuffer, sizeof(storeHazardBuffer)))
		store.hazardClass = storeHazardBuffer;
	ImGui::InputDouble((label + " capacity").c_str(), &store.capacity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting usable").c_str(), &store.startingQualified, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting held").c_str(), &store.startingUnqualified, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " protected reserve").c_str(), &store.protectedReserve, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	bool compound = store.composition.has_value();
	if (ImGui::Checkbox((label + " declared composition").c_str(), &compound))
	{
		if (compound && !store.composition) store.composition = core::ResourceCompositionSettings{};
		else if (!compound) store.composition.reset();
	}
	if (store.composition)
	{
		auto& composition = *store.composition;
		ImGui::InputDouble((label + " moisture fraction").c_str(), &composition.moisture, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " nutrient nitrogen fraction").c_str(), &composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " atmospheric nitrogen fraction").c_str(), &composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " phosphorus fraction").c_str(), &composition.phosphorus, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " potassium fraction").c_str(), &composition.potassium, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " other minerals fraction").c_str(), &composition.otherMinerals, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " energy kJ per kg dry").c_str(), &composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	}
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderTransferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceTransferSettings& transfer,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char transferIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(transferIdBuffer, transfer.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(), transferIdBuffer, sizeof(transferIdBuffer)))
		transfer.id = transferIdBuffer;
	char sourceBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(sourceBuffer, transfer.sourceId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " source").c_str(), sourceBuffer, sizeof(sourceBuffer)))
		transfer.sourceId = sourceBuffer;
	char destinationBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(destinationBuffer, transfer.destinationId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " destination").c_str(), destinationBuffer, sizeof(destinationBuffer)))
		transfer.destinationId = destinationBuffer;
	char transferUnitBuffer[8]{};
	std::strncpy(transferUnitBuffer, transfer.unit.c_str(), sizeof(transferUnitBuffer) - 1);
	if (ImGui::InputText((label + " unit").c_str(), transferUnitBuffer, sizeof(transferUnitBuffer)))
		transfer.unit = transferUnitBuffer;
	ImGui::InputDouble((label + " rate per second").c_str(), &transfer.ratePerSecond, 0.0, 0.0, "%.3f");
	ImGui::InputInt((label + " priority").c_str(), &transfer.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

void renderStoreList(core::StationSystemsSettings& draft)
{
	// Removal is deferred until the whole row has rendered: erasing the element
	// mid-row would destroy the bound reference (and, for a compound store, its
	// nested composition) while the rest of the row still reads it (#557).
	// The index only advances on kept rows so a shifted row is never skipped.
	for (size_t index = 0; index < draft.stores.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeStore = renderStoreEntry(draft, draft.stores[index],
			"Store " + row, "Remove store " + row);
		if (index + 1 < draft.stores.size()) ImGui::Separator();
		if (removeStore)
			draft.stores.erase(draft.stores.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (ImGui::Button("Add store"))
	{
		core::ResourceStoreSettings store;
		store.unit = "kg";
		store.capacity = 100.0;
		store.startingQualified = 50.0;
		for (size_t candidate = draft.stores.size() + 1;; ++candidate)
		{
			store.id = "store" + std::to_string(candidate);
			auto const taken = std::any_of(draft.stores.begin(), draft.stores.end(),
				[&](core::ResourceStoreSettings const& existing) { return existing.id == store.id; });
			if (!taken) break;
		}
		draft.stores.push_back(std::move(store));
	}
}

void renderTransferList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.transfers.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeTransfer = renderTransferEntry(draft, draft.transfers[index],
			"Transfer " + row, "Remove transfer " + row);
		if (removeTransfer)
			draft.transfers.erase(draft.transfers.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (ImGui::Button("Add transfer"))
	{
		core::ResourceTransferSettings transfer;
		transfer.unit = "kg";
		transfer.ratePerSecond = 1.0;
		if (!draft.stores.empty())
		{
			transfer.sourceId = draft.stores.front().id;
			transfer.unit = draft.stores.front().unit;
			for (auto const& store : draft.stores)
				if (store.id != transfer.sourceId && store.unit == transfer.unit)
				{ transfer.destinationId = store.id; break; }
		}
		for (size_t candidate = draft.transfers.size() + 1;; ++candidate)
		{
			transfer.id = "transfer" + std::to_string(candidate);
			auto const taken = std::any_of(draft.transfers.begin(), draft.transfers.end(),
				[&](core::ResourceTransferSettings const& existing) { return existing.id == transfer.id; });
			if (!taken) break;
		}
		draft.transfers.push_back(std::move(transfer));
	}
}

namespace
{
// The registry seam renders fields only: removal is a list-surface
// operation, and a caller without the deferred-removal loop (the Flow
// inspector) must not be handed a Remove button it cannot honour.
bool renderStoreEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.stores.size()) return false;
	return renderStoreEntry(draft, draft.stores[index],
		"Store " + std::to_string(index), {});
}

bool renderTransferEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.transfers.size()) return false;
	return renderTransferEntry(draft, draft.transfers[index],
		"Transfer " + std::to_string(index), {});
}

// Keyed by the Flow node type names in core::stationPortDescriptors().
constexpr std::array<StationEntryEditor, 2> kEntryEditors{{
	{"Store", &renderStoreEntryAt},
	{"Transfer", &renderTransferEntryAt},
}};
}

std::span<StationEntryEditor const> stationEntryEditors()
{
	return kEntryEditors;
}

StationEntryEditor const* findStationEntryEditor(std::string_view nodeType)
{
	for (auto const& editor : kEntryEditors)
		if (editor.nodeType == nodeType) return &editor;
	return nullptr;
}
}
