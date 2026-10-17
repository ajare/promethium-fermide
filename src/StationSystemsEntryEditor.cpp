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

// #647: the power and conversion chain entry editors, extracted from the
// Station Systems form tab so the Flow inspector renders the exact same
// fields, labels and deferred-removal behaviour against the same draft.

// One declared material exchange of an equipment load. The exchange rows
// render inside the load entry: they are the load's own fields, not a
// separate list.
bool renderEquipmentLoadEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::EquipmentLoadSettings& load,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char loadIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(loadIdBuffer, load.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(), loadIdBuffer, sizeof(loadIdBuffer)))
		load.id = loadIdBuffer;
	ImGui::InputDouble((label + " electrical demand kW").c_str(),
		&load.electricalDemandKW, 0.0, 0.0, "%.3f");
	bool loadPeriodic = load.operatingProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox((label + " periodic duty").c_str(), &loadPeriodic))
	{
		if (loadPeriodic)
		{
			if (load.operatingProfile.periodSeconds <= 0.0) load.operatingProfile.periodSeconds = 120.0;
			if (load.operatingProfile.segments.empty())
				load.operatingProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			load.operatingProfile.periodSeconds = 0.0;
			load.operatingProfile.segments.clear();
		}
	}
	if (!loadPeriodic)
		ImGui::InputDouble((label + " constant duty").c_str(),
			&load.operatingProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble((label + " duty period seconds").c_str(),
			&load.operatingProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < load.operatingProfile.segments.size(); ++segment)
		{
			auto& stretch = load.operatingProfile.segments[segment];
			auto const segmentRow = label + " duty segment " + std::to_string(segment);
			ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(("Remove " + segmentRow).c_str()))
				load.operatingProfile.segments.erase(
					load.operatingProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (load.operatingProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add duty segment to " + label).c_str()))
			load.operatingProfile.segments.push_back(
				{load.operatingProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::InputDouble((label + " heat output kW").c_str(), &load.heatOutputKW, 0.0, 0.0, "%.3f");
	char thermalClassBuffer[16]{};
	std::strncpy(thermalClassBuffer, load.thermalClass.c_str(), sizeof(thermalClassBuffer) - 1);
	if (ImGui::InputText((label + " thermal class").c_str(),
		thermalClassBuffer, sizeof(thermalClassBuffer)))
		load.thermalClass = thermalClassBuffer;
	bool requiresCooling = load.coolingDisposition == core::StationCoolingDisposition::CoolingService;
	if (ImGui::Checkbox((label + " requires cooling service").c_str(), &requiresCooling))
		load.coolingDisposition = requiresCooling
			? core::StationCoolingDisposition::CoolingService
			: core::StationCoolingDisposition::Exported;
	ImGui::InputDouble((label + " critical floor kW").c_str(),
		&load.criticalFloorKW, 0.0, 0.0, "%.3f");
	ImGui::InputInt((label + " priority").c_str(), &load.priority);
	ImGui::InputDouble((label + " max curtailment").c_str(),
		&load.maxCurtailment, 0.0, 0.0, "%.3f");
	// #417 recovery rules (purely discretionary loads only; a load
	// with a protected floor is restored by the protected pass). All
	// default to zero: no dwell/hysteresis, clean restart.
	ImGui::InputDouble((label + " recovery dwell (s)").c_str(),
		&load.recoveryDwellSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recovery hysteresis fraction").c_str(),
		&load.recoveryHysteresisFraction, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " restart surge power (kW)").c_str(),
		&load.restartSurgePowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " restart surge seconds").c_str(),
		&load.restartSurgeSeconds, 0.0, 0.0, "%.3f");
	char heatSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(heatSinkBuffer, load.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink").c_str(),
		heatSinkBuffer, sizeof(heatSinkBuffer)))
		load.heatSinkId = heatSinkBuffer;
	for (size_t exchange = 0; exchange < load.materialExchanges.size(); )
	{
		auto& flow = load.materialExchanges[exchange];
		auto const exchangeRow = label + " exchange " + std::to_string(exchange);
		char exchangeStoreBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(exchangeStoreBuffer, flow.storeId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText((exchangeRow + " store").c_str(),
			exchangeStoreBuffer, sizeof(exchangeStoreBuffer)))
			flow.storeId = exchangeStoreBuffer;
		ImGui::SameLine();
		ImGui::InputDouble((exchangeRow + " rate").c_str(), &flow.ratePerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		ImGui::Checkbox((exchangeRow + " consumes").c_str(), &flow.consumes);
		ImGui::SameLine();
		bool const removeExchange = ImGui::Button(("Remove " + exchangeRow).c_str());
		if (removeExchange)
			load.materialExchanges.erase(
				load.materialExchanges.begin() + static_cast<std::ptrdiff_t>(exchange));
		else
			++exchange;
	}
	if (load.materialExchanges.size() < core::MaxStationLoadMaterialExchanges
		&& ImGui::Button(("Add exchange to " + label).c_str()))
		load.materialExchanges.push_back({});
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderBatteryEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::BatterySettings& battery,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char batteryIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(batteryIdBuffer, battery.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(), batteryIdBuffer, sizeof(batteryIdBuffer)))
		battery.id = batteryIdBuffer;
	ImGui::InputDouble((label + " nameplate kWh").c_str(),
		&battery.nameplateKWh, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " usable kWh").c_str(),
		&battery.usableKWh, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting stored kWh").c_str(),
		&battery.startingStoredKWh, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max charge kW").c_str(),
		&battery.maxChargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max discharge kW").c_str(),
		&battery.maxDischargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " charge efficiency").c_str(),
		&battery.chargeEfficiency, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " discharge efficiency").c_str(),
		&battery.dischargeEfficiency, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderGeneratorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::GeneratorSettings& generator,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char generatorIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(generatorIdBuffer, generator.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(),
		generatorIdBuffer, sizeof(generatorIdBuffer)))
		generator.id = generatorIdBuffer;
	char fuelStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(fuelStoreBuffer, generator.fuelStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " fuel store").c_str(),
		fuelStoreBuffer, sizeof(fuelStoreBuffer)))
		generator.fuelStoreId = fuelStoreBuffer;
	char generatorClassBuffer[16]{};
	std::strncpy(generatorClassBuffer, generator.thermalClass.c_str(), sizeof(generatorClassBuffer) - 1);
	if (ImGui::InputText((label + " thermal class").c_str(),
		generatorClassBuffer, sizeof(generatorClassBuffer)))
		generator.thermalClass = generatorClassBuffer;
	char generatorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(generatorSinkBuffer, generator.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink").c_str(),
		generatorSinkBuffer, sizeof(generatorSinkBuffer)))
		generator.heatSinkId = generatorSinkBuffer;
	ImGui::InputDouble((label + " rated output kW").c_str(),
		&generator.ratedOutputKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " efficiency").c_str(),
		&generator.efficiency, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " ramp kW per second").c_str(),
		&generator.maxRampKWPerSecond, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " commanded output kW").c_str(),
		&generator.commandedOutputKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " startup power kW").c_str(),
		&generator.startupPowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " startup seconds").c_str(),
		&generator.startupSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " shutdown heat kW").c_str(),
		&generator.shutdownHeatKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " shutdown seconds").c_str(),
		&generator.shutdownSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max sink temperature C").c_str(),
		&generator.maxSinkTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderFuelCellEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::FuelCellSettings& cell,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	char cellIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellIdBuffer, cell.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(),
		cellIdBuffer, sizeof(cellIdBuffer)))
		cell.id = cellIdBuffer;
	char cellHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellHazardBuffer, cell.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		cellHazardBuffer, sizeof(cellHazardBuffer)))
		cell.hazardClass = cellHazardBuffer;
	char cellHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellHydrogenBuffer, cell.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hydrogen reactant store").c_str(),
		cellHydrogenBuffer, sizeof(cellHydrogenBuffer)))
		cell.hydrogenStoreId = cellHydrogenBuffer;
	char cellOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellOxygenBuffer, cell.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " oxygen reactant store").c_str(),
		cellOxygenBuffer, sizeof(cellOxygenBuffer)))
		cell.oxygenStoreId = cellOxygenBuffer;
	char cellWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellWaterBuffer, cell.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " product water store").c_str(),
		cellWaterBuffer, sizeof(cellWaterBuffer)))
		cell.waterStoreId = cellWaterBuffer;
	char cellThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellThermalBuffer, cell.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink thermal class").c_str(),
		cellThermalBuffer, sizeof(cellThermalBuffer)))
		cell.thermalClass = cellThermalBuffer;
	char cellSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellSinkBuffer, cell.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink reservoir").c_str(),
		cellSinkBuffer, sizeof(cellSinkBuffer)))
		cell.heatSinkId = cellSinkBuffer;
	ImGui::InputDouble((label + " rated output kW").c_str(),
		&cell.ratedOutputKW, 0.0, 10.0, "%.3f");
	ImGui::InputDouble((label + " efficiency (0..1]").c_str(),
		&cell.efficiency, 0.0, 0.05, "%.3f");
	ImGui::InputDouble((label + " commanded output kW").c_str(),
		&cell.commandedOutputKW, 0.0, 1.0, "%.3f");
	ImGui::InputDouble((label + " max sink temperature C").c_str(),
		&cell.maxSinkTemperatureC, 0.0, 5.0, "%.1f");
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	return removeRequested;
}

bool renderSabatierReactorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::SabatierReactorSettings& reactor,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	char reactorIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorIdBuffer, reactor.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(),
		reactorIdBuffer, sizeof(reactorIdBuffer)))
		reactor.id = reactorIdBuffer;
	char reactorHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorHazardBuffer, reactor.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		reactorHazardBuffer, sizeof(reactorHazardBuffer)))
		reactor.hazardClass = reactorHazardBuffer;
	char reactorCo2Buffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorCo2Buffer, reactor.co2StoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " CO2 feed store").c_str(),
		reactorCo2Buffer, sizeof(reactorCo2Buffer)))
		reactor.co2StoreId = reactorCo2Buffer;
	char reactorHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorHydrogenBuffer, reactor.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hydrogen feed store").c_str(),
		reactorHydrogenBuffer, sizeof(reactorHydrogenBuffer)))
		reactor.hydrogenStoreId = reactorHydrogenBuffer;
	char reactorMethaneBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorMethaneBuffer, reactor.methaneStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " methane product store").c_str(),
		reactorMethaneBuffer, sizeof(reactorMethaneBuffer)))
		reactor.methaneStoreId = reactorMethaneBuffer;
	char reactorWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorWaterBuffer, reactor.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " product water store").c_str(),
		reactorWaterBuffer, sizeof(reactorWaterBuffer)))
		reactor.waterStoreId = reactorWaterBuffer;
	char reactorThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorThermalBuffer, reactor.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink thermal class").c_str(),
		reactorThermalBuffer, sizeof(reactorThermalBuffer)))
		reactor.thermalClass = reactorThermalBuffer;
	char reactorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorSinkBuffer, reactor.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink reservoir").c_str(),
		reactorSinkBuffer, sizeof(reactorSinkBuffer)))
		reactor.heatSinkId = reactorSinkBuffer;
	ImGui::PushItemWidth(140.0f);
	ImGui::InputDouble((label + " rated CO2 conversion kg/s").c_str(),
		&reactor.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " specific energy kWh/kg CO2").c_str(),
		&reactor.specificEnergyKWhPerKgCO2, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " max sink temperature C").c_str(),
		&reactor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt((label + " priority").c_str(), &reactor.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderMethaneProcessorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::MethaneProcessorSettings& processor,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	char processorIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorIdBuffer, processor.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(),
		processorIdBuffer, sizeof(processorIdBuffer)))
		processor.id = processorIdBuffer;
	char processorHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorHazardBuffer, processor.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		processorHazardBuffer, sizeof(processorHazardBuffer)))
		processor.hazardClass = processorHazardBuffer;
	char processorMethaneBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorMethaneBuffer, processor.methaneStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " methane feed store").c_str(),
		processorMethaneBuffer, sizeof(processorMethaneBuffer)))
		processor.methaneStoreId = processorMethaneBuffer;
	char processorHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorHydrogenBuffer, processor.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " recovered hydrogen store").c_str(),
		processorHydrogenBuffer, sizeof(processorHydrogenBuffer)))
		processor.hydrogenStoreId = processorHydrogenBuffer;
	char processorCarbonBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorCarbonBuffer, processor.carbonStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " solid carbon store").c_str(),
		processorCarbonBuffer, sizeof(processorCarbonBuffer)))
		processor.carbonStoreId = processorCarbonBuffer;
	char processorThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorThermalBuffer, processor.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink thermal class").c_str(),
		processorThermalBuffer, sizeof(processorThermalBuffer)))
		processor.thermalClass = processorThermalBuffer;
	char processorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorSinkBuffer, processor.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " heat sink reservoir").c_str(),
		processorSinkBuffer, sizeof(processorSinkBuffer)))
		processor.heatSinkId = processorSinkBuffer;
	ImGui::PushItemWidth(140.0f);
	ImGui::InputDouble((label + " rated CH4 processing kg/s").c_str(),
		&processor.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " specific energy kWh/kg CH4").c_str(),
		&processor.specificEnergyKWhPerKgCH4, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " max sink temperature C").c_str(),
		&processor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt((label + " priority").c_str(), &processor.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

// #656: the thermal reservoir entry editor, extracted from the Station
// Systems form tab so the Flow inspector renders the exact same fields,
// labels and deferred-removal behaviour against the same draft.
bool renderThermalReservoirEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ThermalReservoirSettings& reservoir,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char reservoirIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reservoirIdBuffer, reservoir.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(),
		reservoirIdBuffer, sizeof(reservoirIdBuffer)))
		reservoir.id = reservoirIdBuffer;
	char reservoirClassBuffer[16]{};
	std::strncpy(reservoirClassBuffer, reservoir.thermalClass.c_str(), sizeof(reservoirClassBuffer) - 1);
	if (ImGui::InputText((label + " thermal class").c_str(),
		reservoirClassBuffer, sizeof(reservoirClassBuffer)))
		reservoir.thermalClass = reservoirClassBuffer;
	ImGui::InputDouble((label + " heat capacity kJ/K").c_str(),
		&reservoir.heatCapacityKJPerK, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting temperature C").c_str(),
		&reservoir.startingTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " min temperature C").c_str(),
		&reservoir.minTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max temperature C").c_str(),
		&reservoir.maxTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " absorbed heat kW").c_str(),
		&reservoir.absorbedHeatKW, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderRadiatorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::RadiatorSettings& radiator,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	char radiatorIdBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(radiatorIdBuffer, radiator.id.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " id").c_str(),
		radiatorIdBuffer, sizeof(radiatorIdBuffer)))
		radiator.id = radiatorIdBuffer;
	char radiatorHostBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(radiatorHostBuffer, radiator.reservoirId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " reservoir").c_str(),
		radiatorHostBuffer, sizeof(radiatorHostBuffer)))
		radiator.reservoirId = radiatorHostBuffer;
	ImGui::InputDouble((label + " area m2").c_str(),
		&radiator.areaM2, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " emissivity").c_str(),
		&radiator.emissivity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " view factor").c_str(),
		&radiator.viewFactor, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " sink temperature C").c_str(),
		&radiator.sinkTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

void renderEquipmentLoadList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.equipmentLoads.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeLoad = renderEquipmentLoadEntry(draft, draft.equipmentLoads[index],
			"Load " + row, "Remove load " + row);
		if (removeLoad)
			draft.equipmentLoads.erase(draft.equipmentLoads.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.equipmentLoads.size() < core::MaxStationEquipmentLoads && ImGui::Button("Add load"))
	{
		core::EquipmentLoadSettings load;
		load.electricalDemandKW = 5.0;
		load.thermalClass = "moderate";
		for (size_t candidate = draft.equipmentLoads.size() + 1;; ++candidate)
		{
			load.id = "load" + std::to_string(candidate);
			auto const taken = std::any_of(draft.equipmentLoads.begin(), draft.equipmentLoads.end(),
				[&](core::EquipmentLoadSettings const& existing) { return existing.id == load.id; });
			if (!taken) break;
		}
		draft.equipmentLoads.push_back(std::move(load));
	}
}

void renderBatteryList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.batteries.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeBattery = renderBatteryEntry(draft, draft.batteries[index],
			"Battery " + row, "Remove battery " + row);
		if (removeBattery)
			draft.batteries.erase(draft.batteries.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.batteries.size() < core::MaxStationBatteries && ImGui::Button("Add battery"))
	{
		core::BatterySettings battery;
		battery.nameplateKWh = 10.0;
		battery.usableKWh = 8.0;
		battery.startingStoredKWh = 8.0;
		battery.maxChargeKW = 2.0;
		battery.maxDischargeKW = 4.0;
		for (size_t candidate = draft.batteries.size() + 1;; ++candidate)
		{
			battery.id = "battery" + std::to_string(candidate);
			auto const taken = std::any_of(draft.batteries.begin(), draft.batteries.end(),
				[&](core::BatterySettings const& existing) { return existing.id == battery.id; });
			if (!taken) break;
		}
		draft.batteries.push_back(std::move(battery));
	}
}

void renderGeneratorList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.generators.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeGenerator = renderGeneratorEntry(draft, draft.generators[index],
			"Generator " + row, "Remove generator " + row);
		if (removeGenerator)
			draft.generators.erase(draft.generators.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.generators.size() < core::MaxStationGenerators && ImGui::Button("Add generator"))
	{
		core::GeneratorSettings generator;
		generator.thermalClass = "generation";
		generator.ratedOutputKW = 10.0;
		generator.efficiency = 0.4;
		generator.maxRampKWPerSecond = 2.0;
		generator.commandedOutputKW = 10.0;
		generator.startupPowerKW = 1.0;
		generator.startupSeconds = 5.0;
		generator.shutdownHeatKW = 0.5;
		generator.shutdownSeconds = 10.0;
		generator.maxSinkTemperatureC = 90.0;
		// Prefer an energy-unit store as the default fuel so a freshly
		// drafted generator is valid against the declared fuel vocabulary.
		for (auto const& store : draft.stores)
			if (store.unit == "kJ" || store.unit == "MJ"
				|| store.unit == "Wh" || store.unit == "kWh")
			{ generator.fuelStoreId = store.id; break; }
		// Prefer a generation-class sink when one exists; otherwise the
		// first reservoir (validation will surface a class mismatch).
		if (!draft.thermalReservoirs.empty())
		{
			generator.heatSinkId = draft.thermalReservoirs.back().id;
			for (auto const& reservoir : draft.thermalReservoirs)
				if (reservoir.thermalClass == generator.thermalClass)
				{ generator.heatSinkId = reservoir.id; break; }
		}
		for (size_t candidate = draft.generators.size() + 1;; ++candidate)
		{
			generator.id = "generator" + std::to_string(candidate);
			auto const taken = std::any_of(draft.generators.begin(), draft.generators.end(),
				[&](core::GeneratorSettings const& existing) { return existing.id == generator.id; });
			if (!taken) break;
		}
		draft.generators.push_back(std::move(generator));
	}
}

void renderFuelCellList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.fuelCells.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeCell = renderFuelCellEntry(draft, draft.fuelCells[index],
			"Fuel cell " + row, "Remove fuel cell " + row);
		if (removeCell)
		{
			draft.fuelCells.erase(draft.fuelCells.begin() + static_cast<std::ptrdiff_t>(index));
			continue;
		}
		++index;
	}
	if (draft.fuelCells.size() < core::MaxStationFuelCells && ImGui::Button("Add fuel cell"))
	{
		core::FuelCellSettings cell;
		cell.hazardClass = "normal";
		cell.thermalClass = "low";
		cell.ratedOutputKW = 10.0;
		cell.efficiency = 0.6;
		cell.maxSinkTemperatureC = 80.0;
		if (!draft.stores.empty())
		{
			cell.hydrogenStoreId = draft.stores.front().id;
			cell.oxygenStoreId = draft.stores.front().id;
			cell.waterStoreId = draft.stores.front().id;
		}
		if (!draft.thermalReservoirs.empty())
			cell.heatSinkId = draft.thermalReservoirs.front().id;
		for (size_t candidate = draft.fuelCells.size() + 1;; ++candidate)
		{
			cell.id = "fuelcell" + std::to_string(candidate);
			auto const taken = std::any_of(draft.fuelCells.begin(), draft.fuelCells.end(),
				[&](core::FuelCellSettings const& existing) { return existing.id == cell.id; });
			if (!taken) break;
		}
		draft.fuelCells.push_back(std::move(cell));
	}
}

void renderSabatierList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.sabatierReactors.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeReactor = renderSabatierReactorEntry(draft, draft.sabatierReactors[index],
			"Sabatier " + row, "Remove Sabatier reactor " + row);
		if (removeReactor)
			draft.sabatierReactors.erase(draft.sabatierReactors.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.sabatierReactors.size() < core::MaxStationSabatierReactors
		&& ImGui::Button("Add Sabatier reactor"))
	{
		core::SabatierReactorSettings reactor;
		reactor.hazardClass = "normal";
		reactor.thermalClass = "low";
		reactor.rateKgPerS = 0.001;
		reactor.specificEnergyKWhPerKgCO2 = 1.0;
		reactor.maxSinkTemperatureC = 60.0;
		if (!draft.stores.empty())
		{
			reactor.co2StoreId = draft.stores.front().id;
			reactor.hydrogenStoreId = draft.stores.front().id;
			reactor.methaneStoreId = draft.stores.front().id;
			reactor.waterStoreId = draft.stores.front().id;
		}
		if (!draft.thermalReservoirs.empty())
			reactor.heatSinkId = draft.thermalReservoirs.front().id;
		for (size_t candidate = draft.sabatierReactors.size() + 1;; ++candidate)
		{
			reactor.id = "sabatier" + std::to_string(candidate);
			auto const taken = std::any_of(draft.sabatierReactors.begin(), draft.sabatierReactors.end(),
				[&](core::SabatierReactorSettings const& existing) { return existing.id == reactor.id; });
			if (!taken) break;
		}
		draft.sabatierReactors.push_back(std::move(reactor));
	}
}

void renderMethaneProcessorList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.methaneProcessors.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeProcessor = renderMethaneProcessorEntry(draft,
			draft.methaneProcessors[index],
			"Methane processor " + row, "Remove methane processor " + row);
		if (removeProcessor)
			draft.methaneProcessors.erase(
				draft.methaneProcessors.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.methaneProcessors.size() < core::MaxStationMethaneProcessors
		&& ImGui::Button("Add methane processor"))
	{
		core::MethaneProcessorSettings processor;
		processor.hazardClass = "normal";
		processor.thermalClass = "low";
		processor.rateKgPerS = 0.001;
		processor.specificEnergyKWhPerKgCH4 = 2.0;
		processor.maxSinkTemperatureC = 60.0;
		if (!draft.stores.empty())
		{
			processor.methaneStoreId = draft.stores.front().id;
			processor.hydrogenStoreId = draft.stores.front().id;
			processor.carbonStoreId = draft.stores.front().id;
		}
		if (!draft.thermalReservoirs.empty())
			processor.heatSinkId = draft.thermalReservoirs.front().id;
		for (size_t candidate = draft.methaneProcessors.size() + 1;; ++candidate)
		{
			processor.id = "methaneprocessor" + std::to_string(candidate);
			auto const taken = std::any_of(draft.methaneProcessors.begin(),
				draft.methaneProcessors.end(),
				[&](core::MethaneProcessorSettings const& existing) { return existing.id == processor.id; });
			if (!taken) break;
		}
		draft.methaneProcessors.push_back(std::move(processor));
	}
}

void renderThermalReservoirList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.thermalReservoirs.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeReservoir = renderThermalReservoirEntry(draft,
			draft.thermalReservoirs[index],
			"Reservoir " + row, "Remove reservoir " + row);
		if (removeReservoir)
			draft.thermalReservoirs.erase(
				draft.thermalReservoirs.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.thermalReservoirs.size() < core::MaxStationThermalReservoirs
		&& ImGui::Button("Add thermal reservoir"))
	{
		core::ThermalReservoirSettings reservoir;
		reservoir.thermalClass = "moderate";
		reservoir.heatCapacityKJPerK = 100.0;
		reservoir.startingTemperatureC = 20.0;
		reservoir.minTemperatureC = 0.0;
		reservoir.maxTemperatureC = 40.0;
		for (size_t candidate = draft.thermalReservoirs.size() + 1;; ++candidate)
		{
			reservoir.id = "reservoir" + std::to_string(candidate);
			auto const taken = std::any_of(draft.thermalReservoirs.begin(),
				draft.thermalReservoirs.end(),
				[&](core::ThermalReservoirSettings const& existing) { return existing.id == reservoir.id; });
			if (!taken) break;
		}
		draft.thermalReservoirs.push_back(std::move(reservoir));
	}
}

void renderRadiatorList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.radiators.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeRadiator = renderRadiatorEntry(draft, draft.radiators[index],
			"Radiator " + row, "Remove radiator " + row);
		if (removeRadiator)
			draft.radiators.erase(
				draft.radiators.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (draft.radiators.size() < core::MaxStationRadiators && ImGui::Button("Add radiator"))
	{
		core::RadiatorSettings radiator;
		radiator.areaM2 = 100.0;
		radiator.emissivity = 0.9;
		radiator.viewFactor = 1.0;
		radiator.sinkTemperatureC = -270.0;
		if (!draft.thermalReservoirs.empty())
			radiator.reservoirId = draft.thermalReservoirs.front().id;
		for (size_t candidate = draft.radiators.size() + 1;; ++candidate)
		{
			radiator.id = "radiator" + std::to_string(candidate);
			auto const taken = std::any_of(draft.radiators.begin(), draft.radiators.end(),
				[&](core::RadiatorSettings const& existing) { return existing.id == radiator.id; });
			if (!taken) break;
		}
		draft.radiators.push_back(std::move(radiator));
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

bool renderEquipmentLoadEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.equipmentLoads.size()) return false;
	return renderEquipmentLoadEntry(draft, draft.equipmentLoads[index],
		"Load " + std::to_string(index), {});
}

bool renderBatteryEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.batteries.size()) return false;
	return renderBatteryEntry(draft, draft.batteries[index],
		"Battery " + std::to_string(index), {});
}

bool renderGeneratorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.generators.size()) return false;
	return renderGeneratorEntry(draft, draft.generators[index],
		"Generator " + std::to_string(index), {});
}

bool renderFuelCellEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.fuelCells.size()) return false;
	return renderFuelCellEntry(draft, draft.fuelCells[index],
		"Fuel cell " + std::to_string(index), {});
}

bool renderSabatierEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.sabatierReactors.size()) return false;
	return renderSabatierReactorEntry(draft, draft.sabatierReactors[index],
		"Sabatier " + std::to_string(index), {});
}

bool renderMethaneProcessorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.methaneProcessors.size()) return false;
	return renderMethaneProcessorEntry(draft, draft.methaneProcessors[index],
		"Methane processor " + std::to_string(index), {});
}

bool renderThermalReservoirEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.thermalReservoirs.size()) return false;
	return renderThermalReservoirEntry(draft, draft.thermalReservoirs[index],
		"Reservoir " + std::to_string(index), {});
}

bool renderRadiatorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.radiators.size()) return false;
	return renderRadiatorEntry(draft, draft.radiators[index],
		"Radiator " + std::to_string(index), {});
}

// Keyed by the Flow node type names in core::stationPortDescriptors().
constexpr std::array<StationEntryEditor, 10> kEntryEditors{{
	{"Store", &renderStoreEntryAt},
	{"Transfer", &renderTransferEntryAt},
	{"Equipment", &renderEquipmentLoadEntryAt},
	{"Battery", &renderBatteryEntryAt},
	{"Generator", &renderGeneratorEntryAt},
	{"Fuel cell", &renderFuelCellEntryAt},
	{"Sabatier", &renderSabatierEntryAt},
	{"Methane processor", &renderMethaneProcessorEntryAt},
	{"Reservoir", &renderThermalReservoirEntryAt},
	{"Radiator", &renderRadiatorEntryAt},
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
