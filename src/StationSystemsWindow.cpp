#include "StationSystemsWindow.h"
#include "core/World.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cstring>

void StationSystemsWindow::render(std::shared_ptr<core::World> const& world, DocumentHistory& history)
{
	if (!visible) return;
	if (!ImGui::Begin("Station Systems", &visible)) { ImGui::End(); return; }
	if (!world)
	{
		mWorld.reset(); mConfirmReset = false;
		ImGui::TextDisabled("No World is open."); ImGui::End(); return;
	}
	if (mWorld.lock() != world->getLifetimeToken().lock())
	{
		mWorld = world->getLifetimeToken(); mDraft = world->getStationSystemsSettings();
		mDiagnostic.clear(); mConfirmReset = false;
	}
	ImGui::Text("Active World: %s", world->getName().c_str());
	ImGui::Text("World status: %s", world->isSimulationPaused() ? "Paused" : "Running");
	ImGui::Text("World elapsed: %.3f seconds (tick %llu)",
		world->getSimulationTick() * static_cast<double>(core::World::getFixedTimestep()),
		static_cast<unsigned long long>(world->getSimulationTick()));
	ImGui::Text("Document: %s", world->isModified() ? "unsaved changes" : "saved");
	ImGui::TextWrapped("Slice 5: authored finite Resource stores, bounded transfers, compound-resource conversions, solar generation, External equipment load supply and rechargeable batteries. Transfers move only qualified material above protected reserves, respect destination capacity, and allocate competing requests by priority then declaration order. Compound stores declare constituent compositions on a dry-mass basis; conversions are validated to never create tracked constituents. Solar output follows installed capacity and the illumination/eclipse profile; zero capacity disables an array. External loads declare demand, operating profile, heat, thermal class, material exchanges, a protected critical floor, priority and allowed curtailment; allocated power never exceeds generation plus battery output and cooling-infeasible loads are not admitted. Batteries separate nameplate and usable energy: the reserve floor (nameplate minus usable) is never dischargeable, charge and discharge efficiencies book their losses, discharge covers deficits on the same allocation pool as generation, and charging takes only surplus. Within a tick, power allocates first, then transfers, then conversions. Observation interval is a provisional display/event setting, not a separate simulation clock.");
	auto const authored = world->getStationSystemsSettings();
	ImGui::Text("Authored start conditions: %s, observation interval %.2f s, %zu resource store(s), %zu transfer(s), %zu conversion(s), %zu solar array(s), %zu external load(s), %zu battery(ies)",
		authored.enabled ? "enabled" : "disabled", authored.observationIntervalSeconds,
		authored.stores.size(), authored.transfers.size(), authored.conversions.size(),
		authored.solarArrays.size(), authored.equipmentLoads.size(), authored.batteries.size());
	ImGui::SeparatorText("Draft settings (seconds; provisional model assumptions)");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	ImGui::Checkbox("Enable Station systems", &mDraft.enabled);
	ImGui::InputFloat("Observation interval (seconds)", &mDraft.observationIntervalSeconds, 0.1f, 1.0f, "%.2f");
	// Removal is deferred until the whole row has rendered: erasing the element
	// mid-row would destroy the bound reference (and, for a compound store, its
	// nested composition) while the rest of the row still reads it (#557).
	// The index only advances on kept rows so a shifted row is never skipped.
	for (size_t index = 0; index < mDraft.stores.size(); )
	{
		auto& store = mDraft.stores[index];
		auto const row = std::to_string(index);
		bool removeStore = false;
		ImGui::PushItemWidth(220);
		char idBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(idBuffer, store.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Store " + row + " id").c_str(), idBuffer, sizeof(idBuffer)))
			store.id = idBuffer;
		char unitBuffer[8]{};
		std::strncpy(unitBuffer, store.unit.c_str(), sizeof(unitBuffer) - 1);
		if (ImGui::InputText(("Store " + row + " unit").c_str(), unitBuffer, sizeof(unitBuffer)))
			store.unit = unitBuffer;
		ImGui::InputDouble(("Store " + row + " capacity").c_str(), &store.capacity, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Store " + row + " starting usable").c_str(), &store.startingQualified, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Store " + row + " starting held").c_str(), &store.startingUnqualified, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Store " + row + " protected reserve").c_str(), &store.protectedReserve, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove store " + row).c_str()))
			removeStore = true;
		bool compound = store.composition.has_value();
		if (ImGui::Checkbox(("Store " + row + " declared composition").c_str(), &compound))
		{
			if (compound && !store.composition) store.composition = core::ResourceCompositionSettings{};
			else if (!compound) store.composition.reset();
		}
		if (store.composition)
		{
			auto& composition = *store.composition;
			ImGui::InputDouble(("Store " + row + " moisture fraction").c_str(), &composition.moisture, 0.0, 0.0, "%.4f");
			ImGui::InputDouble(("Store " + row + " nutrient nitrogen fraction").c_str(), &composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
			ImGui::InputDouble(("Store " + row + " atmospheric nitrogen fraction").c_str(), &composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
			ImGui::InputDouble(("Store " + row + " phosphorus fraction").c_str(), &composition.phosphorus, 0.0, 0.0, "%.4f");
			ImGui::InputDouble(("Store " + row + " potassium fraction").c_str(), &composition.potassium, 0.0, 0.0, "%.4f");
			ImGui::InputDouble(("Store " + row + " other minerals fraction").c_str(), &composition.otherMinerals, 0.0, 0.0, "%.4f");
			ImGui::InputDouble(("Store " + row + " energy kJ per kg dry").c_str(), &composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
		}
		ImGui::PopItemWidth();
		if (removeStore)
			mDraft.stores.erase(mDraft.stores.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (ImGui::Button("Add store"))
	{
		core::ResourceStoreSettings store;
		store.unit = "kg";
		store.capacity = 100.0;
		store.startingQualified = 50.0;
		for (size_t candidate = mDraft.stores.size() + 1;; ++candidate)
		{
			store.id = "store" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.stores.begin(), mDraft.stores.end(),
				[&](core::ResourceStoreSettings const& existing) { return existing.id == store.id; });
			if (!taken) break;
		}
		mDraft.stores.push_back(std::move(store));
	}
	ImGui::SeparatorText("Draft transfers (qualified material, same-unit stores)");
	for (size_t index = 0; index < mDraft.transfers.size(); )
	{
		auto& transfer = mDraft.transfers[index];
		auto const row = std::to_string(index);
		bool removeTransfer = false;
		ImGui::PushItemWidth(220);
		char transferIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(transferIdBuffer, transfer.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Transfer " + row + " id").c_str(), transferIdBuffer, sizeof(transferIdBuffer)))
			transfer.id = transferIdBuffer;
		char sourceBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(sourceBuffer, transfer.sourceId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Transfer " + row + " source").c_str(), sourceBuffer, sizeof(sourceBuffer)))
			transfer.sourceId = sourceBuffer;
		char destinationBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(destinationBuffer, transfer.destinationId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Transfer " + row + " destination").c_str(), destinationBuffer, sizeof(destinationBuffer)))
			transfer.destinationId = destinationBuffer;
		char transferUnitBuffer[8]{};
		std::strncpy(transferUnitBuffer, transfer.unit.c_str(), sizeof(transferUnitBuffer) - 1);
		if (ImGui::InputText(("Transfer " + row + " unit").c_str(), transferUnitBuffer, sizeof(transferUnitBuffer)))
			transfer.unit = transferUnitBuffer;
		ImGui::InputDouble(("Transfer " + row + " rate per second").c_str(), &transfer.ratePerSecond, 0.0, 0.0, "%.3f");
		ImGui::InputInt(("Transfer " + row + " priority").c_str(), &transfer.priority);
		ImGui::SameLine();
		if (ImGui::Button(("Remove transfer " + row).c_str()))
			removeTransfer = true;
		ImGui::PopItemWidth();
		if (removeTransfer)
			mDraft.transfers.erase(mDraft.transfers.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (ImGui::Button("Add transfer"))
	{
		core::ResourceTransferSettings transfer;
		transfer.unit = "kg";
		transfer.ratePerSecond = 1.0;
		if (!mDraft.stores.empty())
		{
			transfer.sourceId = mDraft.stores.front().id;
			transfer.unit = mDraft.stores.front().unit;
			for (auto const& store : mDraft.stores)
				if (store.id != transfer.sourceId && store.unit == transfer.unit)
				{ transfer.destinationId = store.id; break; }
		}
		for (size_t candidate = mDraft.transfers.size() + 1;; ++candidate)
		{
			transfer.id = "transfer" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.transfers.begin(), mDraft.transfers.end(),
				[&](core::ResourceTransferSettings const& existing) { return existing.id == transfer.id; });
			if (!taken) break;
		}
		mDraft.transfers.push_back(std::move(transfer));
	}
	ImGui::SeparatorText("Draft conversions (compound material; declared yields may lose but never create constituents)");
	for (size_t index = 0; index < mDraft.conversions.size(); )
	{
		auto& conversion = mDraft.conversions[index];
		auto const row = std::to_string(index);
		bool removeConversion = false;
		ImGui::PushItemWidth(220);
		char conversionIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(conversionIdBuffer, conversion.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Conversion " + row + " id").c_str(), conversionIdBuffer, sizeof(conversionIdBuffer)))
			conversion.id = conversionIdBuffer;
		ImGui::InputInt(("Conversion " + row + " priority").c_str(), &conversion.priority);
		ImGui::SameLine();
		if (ImGui::Button(("Remove conversion " + row).c_str()))
			removeConversion = true;
		for (size_t flow = 0; flow < conversion.inputs.size(); )
		{
			auto& input = conversion.inputs[flow];
			auto const flowRow = "Conversion " + row + " input " + std::to_string(flow);
			char storeBuffer[core::MaxStationStoreIdLength + 1]{};
			std::strncpy(storeBuffer, input.storeId.c_str(), core::MaxStationStoreIdLength);
			if (ImGui::InputText((flowRow + " store").c_str(), storeBuffer, sizeof(storeBuffer)))
				input.storeId = storeBuffer;
			ImGui::SameLine();
			ImGui::InputDouble((flowRow + " rate").c_str(), &input.ratePerSecond, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			bool const removeInput = ImGui::Button(("Remove " + flowRow).c_str());
			if (removeInput)
				conversion.inputs.erase(conversion.inputs.begin() + static_cast<std::ptrdiff_t>(flow));
			else
				++flow;
		}
		if (conversion.inputs.size() < core::MaxStationConversionFlows
			&& ImGui::Button(("Add input to conversion " + row).c_str()))
			conversion.inputs.push_back({});
		for (size_t flow = 0; flow < conversion.outputs.size(); )
		{
			auto& output = conversion.outputs[flow];
			auto const flowRow = "Conversion " + row + " output " + std::to_string(flow);
			char storeBuffer[core::MaxStationStoreIdLength + 1]{};
			std::strncpy(storeBuffer, output.storeId.c_str(), core::MaxStationStoreIdLength);
			if (ImGui::InputText((flowRow + " store").c_str(), storeBuffer, sizeof(storeBuffer)))
				output.storeId = storeBuffer;
			ImGui::SameLine();
			ImGui::InputDouble((flowRow + " yield").c_str(), &output.yieldPerSecond, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			bool const removeOutput = ImGui::Button(("Remove " + flowRow).c_str());
			if (removeOutput)
				conversion.outputs.erase(conversion.outputs.begin() + static_cast<std::ptrdiff_t>(flow));
			else
				++flow;
		}
		if (conversion.outputs.size() < core::MaxStationConversionFlows
			&& ImGui::Button(("Add output to conversion " + row).c_str()))
			conversion.outputs.push_back({});
		ImGui::PopItemWidth();
		if (removeConversion)
			mDraft.conversions.erase(mDraft.conversions.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.conversions.size() < core::MaxStationResourceConversions)
	{
		if (ImGui::Button("Add conversion"))
		{
			core::ResourceConversionSettings conversion;
			for (size_t candidate = mDraft.conversions.size() + 1;; ++candidate)
			{
				conversion.id = "conversion" + std::to_string(candidate);
				auto const taken = std::any_of(mDraft.conversions.begin(), mDraft.conversions.end(),
					[&](core::ResourceConversionSettings const& existing) { return existing.id == conversion.id; });
				if (!taken) break;
			}
			conversion.inputs.push_back({});
			conversion.outputs.push_back({});
			mDraft.conversions.push_back(std::move(conversion));
		}
	}
	ImGui::SeparatorText("Draft solar arrays (kW; illumination is a fraction of full sun, 0..1)");
	for (size_t index = 0; index < mDraft.solarArrays.size(); )
	{
		auto& array = mDraft.solarArrays[index];
		auto const row = std::to_string(index);
		bool removeArray = false;
		ImGui::PushItemWidth(220);
		char solarIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(solarIdBuffer, array.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Solar " + row + " id").c_str(), solarIdBuffer, sizeof(solarIdBuffer)))
			array.id = solarIdBuffer;
		ImGui::InputDouble(("Solar " + row + " capacity kW").c_str(), &array.capacityKW, 0.0, 0.0, "%.3f");
		bool solarPeriodic = array.illumination.periodSeconds > 0.0;
		if (ImGui::Checkbox(("Solar " + row + " periodic illumination").c_str(), &solarPeriodic))
		{
			if (solarPeriodic)
			{
				if (array.illumination.periodSeconds <= 0.0) array.illumination.periodSeconds = 120.0;
				if (array.illumination.segments.empty())
					array.illumination.segments.push_back({0.0, 1.0});
			}
			else
			{
				array.illumination.periodSeconds = 0.0;
				array.illumination.segments.clear();
			}
		}
		if (!solarPeriodic)
			ImGui::InputDouble(("Solar " + row + " constant illumination").c_str(),
				&array.illumination.constantLevel, 0.0, 0.0, "%.3f");
		else
		{
			ImGui::InputDouble(("Solar " + row + " period seconds").c_str(),
				&array.illumination.periodSeconds, 0.0, 0.0, "%.1f");
			for (size_t segment = 0; segment < array.illumination.segments.size(); )
			{
				auto& stretch = array.illumination.segments[segment];
				auto const segmentRow = "Solar " + row + " segment " + std::to_string(segment);
				ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
				ImGui::SameLine();
				ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
				ImGui::SameLine();
				bool const removeSegment = ImGui::Button(("Remove " + segmentRow).c_str());
				if (removeSegment)
					array.illumination.segments.erase(
						array.illumination.segments.begin() + static_cast<std::ptrdiff_t>(segment));
				else
					++segment;
				}
			if (array.illumination.segments.size() < core::MaxStationProfileSegments
				&& ImGui::Button(("Add segment to solar " + row).c_str()))
				array.illumination.segments.push_back(
					{array.illumination.periodSeconds * 0.5, 0.0});
			}
		ImGui::SameLine();
		if (ImGui::Button(("Remove solar array " + row).c_str()))
			removeArray = true;
		ImGui::PopItemWidth();
		if (removeArray)
			mDraft.solarArrays.erase(mDraft.solarArrays.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.solarArrays.size() < core::MaxStationSolarArrays && ImGui::Button("Add solar array"))
	{
		core::SolarArraySettings array;
		array.capacityKW = 10.0;
		for (size_t candidate = mDraft.solarArrays.size() + 1;; ++candidate)
		{
			array.id = "solar" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.solarArrays.begin(), mDraft.solarArrays.end(),
				[&](core::SolarArraySettings const& existing) { return existing.id == array.id; });
			if (!taken) break;
		}
		mDraft.solarArrays.push_back(std::move(array));
	}
	ImGui::SeparatorText("Draft external equipment loads (kW; heat and material exchanges scale with delivered power)");
	for (size_t index = 0; index < mDraft.equipmentLoads.size(); )
	{
		auto& load = mDraft.equipmentLoads[index];
		auto const row = std::to_string(index);
		bool removeLoad = false;
		ImGui::PushItemWidth(220);
		char loadIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(loadIdBuffer, load.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Load " + row + " id").c_str(), loadIdBuffer, sizeof(loadIdBuffer)))
			load.id = loadIdBuffer;
		ImGui::InputDouble(("Load " + row + " electrical demand kW").c_str(),
			&load.electricalDemandKW, 0.0, 0.0, "%.3f");
		bool loadPeriodic = load.operatingProfile.periodSeconds > 0.0;
		if (ImGui::Checkbox(("Load " + row + " periodic duty").c_str(), &loadPeriodic))
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
			ImGui::InputDouble(("Load " + row + " constant duty").c_str(),
				&load.operatingProfile.constantLevel, 0.0, 0.0, "%.3f");
		else
		{
			ImGui::InputDouble(("Load " + row + " duty period seconds").c_str(),
				&load.operatingProfile.periodSeconds, 0.0, 0.0, "%.1f");
			for (size_t segment = 0; segment < load.operatingProfile.segments.size(); ++segment)
			{
				auto& stretch = load.operatingProfile.segments[segment];
				auto const segmentRow = "Load " + row + " duty segment " + std::to_string(segment);
				ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
				ImGui::SameLine();
				ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
				ImGui::SameLine();
				if (ImGui::Button(("Remove " + segmentRow).c_str()))
					load.operatingProfile.segments.erase(
						load.operatingProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
				}
			if (load.operatingProfile.segments.size() < core::MaxStationProfileSegments
				&& ImGui::Button(("Add duty segment to load " + row).c_str()))
				load.operatingProfile.segments.push_back(
					{load.operatingProfile.periodSeconds * 0.5, 0.0});
			}
		ImGui::InputDouble(("Load " + row + " heat output kW").c_str(), &load.heatOutputKW, 0.0, 0.0, "%.3f");
		char thermalClassBuffer[16]{};
		std::strncpy(thermalClassBuffer, load.thermalClass.c_str(), sizeof(thermalClassBuffer) - 1);
		if (ImGui::InputText(("Load " + row + " thermal class").c_str(),
			thermalClassBuffer, sizeof(thermalClassBuffer)))
			load.thermalClass = thermalClassBuffer;
		bool requiresCooling = load.coolingDisposition == core::StationCoolingDisposition::CoolingService;
		if (ImGui::Checkbox(("Load " + row + " requires cooling service").c_str(), &requiresCooling))
			load.coolingDisposition = requiresCooling
				? core::StationCoolingDisposition::CoolingService
				: core::StationCoolingDisposition::Exported;
		ImGui::InputDouble(("Load " + row + " critical floor kW").c_str(),
			&load.criticalFloorKW, 0.0, 0.0, "%.3f");
		ImGui::InputInt(("Load " + row + " priority").c_str(), &load.priority);
		ImGui::InputDouble(("Load " + row + " max curtailment").c_str(),
			&load.maxCurtailment, 0.0, 0.0, "%.3f");
		for (size_t exchange = 0; exchange < load.materialExchanges.size(); )
		{
			auto& flow = load.materialExchanges[exchange];
			auto const exchangeRow = "Load " + row + " exchange " + std::to_string(exchange);
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
			&& ImGui::Button(("Add exchange to load " + row).c_str()))
			load.materialExchanges.push_back({});
		ImGui::SameLine();
		if (ImGui::Button(("Remove load " + row).c_str()))
			removeLoad = true;
		ImGui::PopItemWidth();
		if (removeLoad)
			mDraft.equipmentLoads.erase(mDraft.equipmentLoads.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.equipmentLoads.size() < core::MaxStationEquipmentLoads && ImGui::Button("Add load"))
	{
		core::EquipmentLoadSettings load;
		load.electricalDemandKW = 5.0;
		load.thermalClass = "moderate";
		for (size_t candidate = mDraft.equipmentLoads.size() + 1;; ++candidate)
		{
			load.id = "load" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.equipmentLoads.begin(), mDraft.equipmentLoads.end(),
				[&](core::EquipmentLoadSettings const& existing) { return existing.id == load.id; });
			if (!taken) break;
		}
		mDraft.equipmentLoads.push_back(std::move(load));
	}
	ImGui::SeparatorText("Draft batteries (kWh; usable energy is the cyclable slice above the reserve floor = nameplate - usable)");
	for (size_t index = 0; index < mDraft.batteries.size(); )
	{
		auto& battery = mDraft.batteries[index];
		auto const row = std::to_string(index);
		bool removeBattery = false;
		ImGui::PushItemWidth(220);
		char batteryIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(batteryIdBuffer, battery.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Battery " + row + " id").c_str(), batteryIdBuffer, sizeof(batteryIdBuffer)))
			battery.id = batteryIdBuffer;
		ImGui::InputDouble(("Battery " + row + " nameplate kWh").c_str(),
			&battery.nameplateKWh, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Battery " + row + " usable kWh").c_str(),
			&battery.usableKWh, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Battery " + row + " starting stored kWh").c_str(),
			&battery.startingStoredKWh, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Battery " + row + " max charge kW").c_str(),
			&battery.maxChargeKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Battery " + row + " max discharge kW").c_str(),
			&battery.maxDischargeKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Battery " + row + " charge efficiency").c_str(),
			&battery.chargeEfficiency, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Battery " + row + " discharge efficiency").c_str(),
			&battery.dischargeEfficiency, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove battery " + row).c_str()))
			removeBattery = true;
		ImGui::PopItemWidth();
		if (removeBattery)
			mDraft.batteries.erase(mDraft.batteries.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.batteries.size() < core::MaxStationBatteries && ImGui::Button("Add battery"))
	{
		core::BatterySettings battery;
		battery.nameplateKWh = 10.0;
		battery.usableKWh = 8.0;
		battery.startingStoredKWh = 8.0;
		battery.maxChargeKW = 2.0;
		battery.maxDischargeKW = 4.0;
		for (size_t candidate = mDraft.batteries.size() + 1;; ++candidate)
		{
			battery.id = "battery" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.batteries.begin(), mDraft.batteries.end(),
				[&](core::BatterySettings const& existing) { return existing.id == battery.id; });
			if (!taken) break;
		}
		mDraft.batteries.push_back(std::move(battery));
	}
	bool const draftChanged = mDraft != authored;
	ImGui::TextUnformatted(draftChanged
		? "Draft differs from the authored start conditions."
		: "Draft matches the authored start conditions; there is nothing to apply.");
	auto apply = [&](bool confirm)
	{
		auto before = captureDocumentSnapshot(world, history);
		auto const changed = mDraft != world->getStationSystemsSettings();
		if (world->configureStationSystems(mDraft, confirm, &mDiagnostic))
		{
			if (changed) commitDocumentEdit(std::move(before), history);
			mConfirmReset = false;
		}
	};
	ImGui::BeginDisabled(!draftChanged);
	if (ImGui::Button("Apply settings"))
	{
		if (world->getSimulationTick() && mDraft != world->getStationSystemsSettings()) mConfirmReset = true;
		else apply(false);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel edits"))
	{
		mDraft = world->getStationSystemsSettings(); mDiagnostic.clear(); mConfirmReset = false;
	}
	if (mConfirmReset)
	{
		ImGui::TextWrapped("Applying these settings resets the complete World simulation, including Agent movement. Confirm?");
		if (ImGui::Button("Confirm World reset")) apply(true);
		ImGui::SameLine();
		if (ImGui::Button("Cancel reset")) mConfirmReset = false;
	}
	ImGui::EndDisabled();
	if (!mDiagnostic.empty()) ImGui::TextWrapped("%s", mDiagnostic.c_str());
	auto const snapshot = world->getStationSystemsSnapshot();
	ImGui::Separator();
	ImGui::Text("Live station state: %s", snapshot.enabled ? (snapshot.healthy ? "Enabled" : "Failed") : "Disabled");
	ImGui::Text("Station elapsed: %.3f seconds", snapshot.elapsedSeconds);
	for (size_t i = 0; i < core::StationSystemNames.size(); ++i)
		ImGui::Text("%s: %llu completed ticks", core::StationSystemNames[i],
			static_cast<unsigned long long>(snapshot.completedTicks[i]));
	ImGui::SeparatorText("Resource stores (live; current inventory is runtime state, not authored)");
	if (snapshot.stores.empty()) ImGui::TextDisabled("No Resource stores authored.");
	for (auto const& store : snapshot.stores)
	{
		ImGui::Text("%s (%s): usable %.3f, held %.3f of capacity %.3f | initial usable %.3f, initial held %.3f | protected %.3f | last tick in %.3f, out %.3f",
			store.id.c_str(), store.unit.c_str(), store.currentQualified, store.currentUnqualified,
			store.capacity, store.startingQualified, store.startingUnqualified,
			store.protectedReserve, store.lastTickIn, store.lastTickOut);
		if (store.composition)
			ImGui::Text("  compound (dry-mass basis): moisture %.4f | nutrient N %.4f, atmospheric N %.4f, P %.4f, K %.4f, other minerals %.4f | %.1f kJ/kg dry",
				store.composition->moisture, store.composition->nutrientNitrogen,
				store.composition->atmosphericNitrogen, store.composition->phosphorus,
				store.composition->potassium, store.composition->otherMinerals,
				store.composition->energyKJPerKgDry);
	}
	ImGui::SeparatorText("Transfers (live; competing requests allocate by priority, then declaration order)");
	if (snapshot.transfers.empty()) ImGui::TextDisabled("No Resource transfers authored.");
	for (auto const& transfer : snapshot.transfers)
	{
		char const* status = "idle";
		switch (transfer.status)
		{
		case core::StationTransferStatus::Full: status = "full"; break;
		case core::StationTransferStatus::Partial: status = "partial"; break;
		case core::StationTransferStatus::Rejected: status = "rejected"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s: %s -> %s (%s) %s | requested %.3f, completed %.3f, unmet %.3f (rate %.3f/s, priority %d)",
			transfer.id.c_str(), transfer.sourceId.c_str(), transfer.destinationId.c_str(),
			transfer.unit.c_str(), status, transfer.lastRequested, transfer.lastCompleted,
			transfer.lastUnmet, transfer.ratePerSecond, transfer.priority);
	}
	ImGui::SeparatorText("Conversions (live; transfers apply first, then conversions, each by priority then declaration order)");
	if (snapshot.conversions.empty()) ImGui::TextDisabled("No Resource conversions authored.");
	for (auto const& conversion : snapshot.conversions)
	{
		char const* status = "idle";
		switch (conversion.status)
		{
		case core::StationTransferStatus::Full: status = "full"; break;
		case core::StationTransferStatus::Partial: status = "partial"; break;
		case core::StationTransferStatus::Rejected: status = "rejected"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s: %s (scale %.3f, priority %d)",
			conversion.id.c_str(), status, conversion.lastScale, conversion.priority);
		for (auto const& flow : conversion.inputs)
			ImGui::Text("  consumes %s (%s): requested %.3f, completed %.3f",
				flow.storeId.c_str(), flow.unit.c_str(), flow.requested, flow.completed);
		for (auto const& flow : conversion.outputs)
			ImGui::Text("  produces %s (%s): requested %.3f, completed %.3f",
				flow.storeId.c_str(), flow.unit.c_str(), flow.requested, flow.completed);
	}
	ImGui::SeparatorText("Constituent balances (current + lost must equal starting within tolerance; dry-mass basis)");
	if (snapshot.constituentBalances.empty()) ImGui::TextDisabled("No compound Resource stores authored.");
	for (auto const& balance : snapshot.constituentBalances)
		ImGui::Text("Constituent %s (%s): starting %.6f, current %.6f, lost %.6f, residual %.9f (tolerance %.1e)",
			balance.constituent.c_str(), balance.unit.c_str(), balance.starting,
			balance.current, balance.lost, balance.residual, snapshot.balanceTolerance);
	ImGui::SeparatorText("Boundary balances (residual must stay within tolerance)");
	if (snapshot.balances.empty()) ImGui::TextDisabled("No Resource stores authored.");
	for (auto const& balance : snapshot.balances)
		ImGui::Text("Balance %s: starting %.3f, current %.3f, lost %.3f, residual %.9f (tolerance %.1e)",
			balance.unit.c_str(), balance.totalStarting, balance.totalCurrent,
			balance.lost, balance.residual, snapshot.balanceTolerance);
	ImGui::SeparatorText("Solar generation (live; output = installed capacity x illumination)");
	if (snapshot.power.sources.empty()) ImGui::TextDisabled("No solar arrays authored.");
	for (auto const& source : snapshot.power.sources)
		ImGui::Text("%s: capacity %.3f kW, illumination %.3f, available %.3f kW%s",
			source.id.c_str(), source.capacityKW, source.illumination, source.availableKW,
			source.disabled ? " (disabled: zero installed capacity)" : "");
	ImGui::SeparatorText("External equipment loads (live; protected critical floors allocate first, then discretionary demand by priority)");
	if (snapshot.power.loads.empty()) ImGui::TextDisabled("No external equipment loads authored.");
	for (auto const& load : snapshot.power.loads)
	{
		char const* status = "idle";
		switch (load.status)
		{
		case core::StationTransferStatus::Full: status = "full"; break;
		case core::StationTransferStatus::Partial: status = "curtailed"; break;
		case core::StationTransferStatus::Rejected: status = "rejected"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s: %s | requested %.3f kW, supplied %.3f kW, unmet %.3f kW | floor %.3f, unmet critical %.3f | delivered %.1f%% | heat exported %.3f kW (priority %d, class %s)",
			load.id.c_str(), status, load.requestedKW, load.suppliedKW, load.unmetKW,
			load.criticalFloorKW, load.unmetCriticalKW, load.deliveredFraction * 100.0,
			load.heatExportedKW, load.priority, load.thermalClass.c_str());
		if (!load.admitted)
			ImGui::Text("  not admitted: %s (unmet cooling %.3f kW)",
				load.blockReason.c_str(), load.unmetCoolingKW);
		for (auto const& flow : load.materialExchanges)
			ImGui::Text("  exchange %s (%s): requested %.3f, completed %.3f, unmet %.3f",
				flow.storeId.c_str(), flow.unit.c_str(), flow.requested, flow.completed,
				flow.unmet);
	}
	ImGui::Text("Power totals: generation %.3f kW, requested %.3f, supplied %.3f, unmet %.3f, unmet critical %.3f, surplus %.3f, exported heat %.3f kW, battery discharge %.3f kW, battery charge %.3f kW, battery shortfall %.3f kW",
		snapshot.power.availableGenerationKW, snapshot.power.totalRequestedKW,
		snapshot.power.totalSuppliedKW, snapshot.power.totalUnmetKW,
		snapshot.power.unmetCriticalKW, snapshot.power.surplusKW, snapshot.power.exportedHeatKW,
		snapshot.power.batteryDischargeKW, snapshot.power.batteryChargeKW,
		snapshot.power.batteryShortfallKW);
	ImGui::SeparatorText("Batteries (live; reserve = nameplate - usable is never dischargeable; discharge covers deficits on the same pool, charging takes only surplus)");
	if (snapshot.power.batteries.empty()) ImGui::TextDisabled("No batteries authored.");
	for (auto const& battery : snapshot.power.batteries)
	{
		char const* status = "idle";
		switch (battery.status)
		{
		case core::StationTransferStatus::Full: status = "discharging"; break;
		case core::StationTransferStatus::Partial: status = "discharge-limited"; break;
		case core::StationTransferStatus::Rejected: status = "cannot-discharge"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s: %s | stored %.3f of nameplate %.3f kWh (usable %.3f, reserve %.3f never dischargeable) | SoC %.1f%% | charge %.3f kW, discharge %.3f kW (eff %.2f/%.2f) | cumulative losses %.3f kWh",
			battery.id.c_str(), status, battery.storedKWh, battery.nameplateKWh,
			battery.usableKWh, battery.reserveKWh, battery.stateOfCharge * 100.0,
			battery.lastChargeKW, battery.lastDischargeKW,
			battery.chargeEfficiency, battery.dischargeEfficiency, battery.lossesKWh);
		if (battery.disabled)
			ImGui::Text("  disabled: zero usable energy cannot store or deliver anything");
		else if (battery.atReserve)
			ImGui::Text("  at reserve floor: no usable energy remains above the reserve");
		else if (battery.saturated)
			ImGui::Text("  saturated: stored energy is at nameplate capacity");
	}
	if (snapshot.enabled && !snapshot.healthy)
		ImGui::TextWrapped("Station software failure: the World is paused. The failed tick was not committed. Reset the World to reconstruct Station systems; resume cannot retry failed scripts.");
	for (auto const& diagnostic : world->getStationSystemsDiagnostics())
	{
		char const* kind = "Lua error";
		switch (diagnostic.failure)
		{
		case core::ScriptExecutionFailure::MemoryBudgetExceeded: kind = "Memory budget exceeded"; break;
		case core::ScriptExecutionFailure::InstructionBudgetExceeded: kind = "Instruction budget exceeded"; break;
		case core::ScriptExecutionFailure::ConversionError: kind = "Invalid output/accounting"; break;
		case core::ScriptExecutionFailure::None: kind = "Unspecified failure"; break;
		case core::ScriptExecutionFailure::LuaError: break;
		}
		ImGui::TextWrapped("Software failure at tick %llu in %s [%s]: %s",
			static_cast<unsigned long long>(diagnostic.worldTick), diagnostic.system.c_str(), kind, diagnostic.message.c_str());
	}
	if (ImGui::CollapsingHeader("Event history (last 256)"))
		for (auto const& event : world->getStationSystemsEvents())
			ImGui::TextWrapped("Tick %llu: %s", static_cast<unsigned long long>(event.worldTick), event.message.c_str());
	ImGui::End();
}
