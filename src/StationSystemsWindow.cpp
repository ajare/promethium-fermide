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
	ImGui::TextWrapped("Slice 5: authored finite Resource stores, bounded transfers, compound-resource conversions, solar generation, External equipment load supply, rechargeable batteries, thermal reservoirs with radiator rejection, coolant loops with freeze-protection heating, and chillers supplying class-matched cold services. Transfers move only qualified material above protected reserves, respect destination capacity, and allocate competing requests by priority then declaration order. Compound stores declare constituent compositions on a dry-mass basis; conversions are validated to never create tracked constituents. Solar output follows installed capacity and the illumination/eclipse profile; zero capacity disables an array. External loads declare demand, operating profile, heat, thermal class, material exchanges, a protected critical floor, priority and allowed curtailment; allocated power never exceeds generation plus battery output and cooling-infeasible loads are not admitted. Batteries separate nameplate and usable energy: the reserve floor (nameplate minus usable) is never dischargeable, charge and discharge efficiencies book their losses, discharge covers deficits on the same allocation pool as generation, and charging takes only surplus. Thermal reservoirs integrate temperature from their effective capacity under absorbed and routed equipment heat; radiators reject heat by the Stefan-Boltzmann law at the emitting temperature against the authored sink, emissivity and view factor. A load's emitted heat is booked exactly once: routed into its class-compatible reservoir or exported directly, never both. Declared temperature limits are reported, never clamped. Coolant loops hold a finite, loop-specific coolant inventory segregated by hazard class (coolant is never potable or nutrient water); the pump circulates only the coolant the loop actually holds, so an underfilled loop flows less and a dry loop flows nothing, and the freeze-protection heater thermostats the loop up to its setpoint within its capacity. Pump work and heater heat enter the loop exactly once and reach the reservoir only through the circulation-gated heat-only exchanger; the loop draw rides the same electrical pool as external loads. Chillers supply a cold service of their declared class through a class-matched coolant-loop evaporator and reservoir condenser: the delivered duty is capped by the loop's actual circulation, gated at the chilled setpoint, and refused while the sink is at or above the maximum condensing temperature; work at the configured COP plus auxiliaries rides the same electrical pool, and condenser heat equals extracted heat plus the work and auxiliaries actually supplied, booked once. Fuelled generators convert a finite energy-unit fuel inventory into bus electrical output at the declared efficiency: output is bounded by the installed rating and the ramp limit, starting only completes once the startup energy has actually been drawn from the shared pool while the class-matched sink is below its maximum sink temperature, and after output stops the configured shutdown heat and its cooling requirement persist for the decay period; waste, startup and decay heat route exactly once into the sink and the fuel ledger closes over fuel = electrical + waste. Within a tick, generators produce first, then power allocates, then coolant loops and chillers run, then thermal integrates, then transfers, then conversions. Observation interval is a provisional display/event setting, not a separate simulation clock.");
	auto const authored = world->getStationSystemsSettings();
	ImGui::Text("Authored start conditions: %s, observation interval %.2f s, %zu resource store(s), %zu transfer(s), %zu conversion(s), %zu solar array(s), %zu external load(s), %zu battery(ies), %zu resupply delivery(ies), %zu thermal reservoir(s), %zu radiator(s), %zu coolant loop(s), %zu chiller(s), %zu generator(s)",
		authored.enabled ? "enabled" : "disabled", authored.observationIntervalSeconds,
		authored.stores.size(), authored.transfers.size(), authored.conversions.size(),
		authored.solarArrays.size(), authored.equipmentLoads.size(), authored.batteries.size(),
		authored.deliveries.size(), authored.thermalReservoirs.size(), authored.radiators.size(),
		authored.coolantLoops.size(), authored.chillers.size(), authored.generators.size());
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
		char heatSinkBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(heatSinkBuffer, load.heatSinkId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Load " + row + " heat sink").c_str(),
			heatSinkBuffer, sizeof(heatSinkBuffer)))
			load.heatSinkId = heatSinkBuffer;
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
	ImGui::SeparatorText("Draft resupply deliveries (store units at simulation seconds; recurrence 0 is a one-off)");
	for (size_t index = 0; index < mDraft.deliveries.size(); )
	{
		auto& delivery = mDraft.deliveries[index];
		auto const row = std::to_string(index);
		bool removeDelivery = false;
		ImGui::PushItemWidth(220);
		char deliveryIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(deliveryIdBuffer, delivery.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Delivery " + row + " id").c_str(), deliveryIdBuffer, sizeof(deliveryIdBuffer)))
			delivery.id = deliveryIdBuffer;
		char deliveryStoreBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(deliveryStoreBuffer, delivery.storeId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Delivery " + row + " store").c_str(),
			deliveryStoreBuffer, sizeof(deliveryStoreBuffer)))
			delivery.storeId = deliveryStoreBuffer;
		char deliveryUnitBuffer[8]{};
		std::strncpy(deliveryUnitBuffer, delivery.unit.c_str(), sizeof(deliveryUnitBuffer) - 1);
		if (ImGui::InputText(("Delivery " + row + " unit").c_str(), deliveryUnitBuffer, sizeof(deliveryUnitBuffer)))
			delivery.unit = deliveryUnitBuffer;
		ImGui::InputDouble(("Delivery " + row + " quantity").c_str(), &delivery.quantity, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Delivery " + row + " first occurrence seconds").c_str(),
			&delivery.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Delivery " + row + " recurrence seconds").c_str(),
			&delivery.recurrenceSeconds, 0.0, 0.0, "%.3f");
		ImGui::Checkbox(("Delivery " + row + " arrives usable").c_str(), &delivery.arrivesQualified);
		ImGui::SameLine();
		ImGui::InputInt(("Delivery " + row + " priority").c_str(), &delivery.priority);
		ImGui::SameLine();
		if (ImGui::Button(("Remove delivery " + row).c_str()))
			removeDelivery = true;
		ImGui::PopItemWidth();
		if (removeDelivery)
			mDraft.deliveries.erase(mDraft.deliveries.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.deliveries.size() < core::MaxStationResourceDeliveries && ImGui::Button("Add delivery"))
	{
		core::ResourceDeliverySettings delivery;
		delivery.quantity = 10.0;
		if (!mDraft.stores.empty())
		{
			delivery.storeId = mDraft.stores.front().id;
			delivery.unit = mDraft.stores.front().unit;
		}
		for (size_t candidate = mDraft.deliveries.size() + 1;; ++candidate)
		{
			delivery.id = "delivery" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.deliveries.begin(), mDraft.deliveries.end(),
				[&](core::ResourceDeliverySettings const& existing) { return existing.id == delivery.id; });
			if (!taken) break;
		}
		mDraft.deliveries.push_back(std::move(delivery));
	}
	ImGui::SeparatorText("Draft thermal reservoirs (kJ/K, degrees C; limits are declared service limits, absorbed heat is environmental)");
	for (size_t index = 0; index < mDraft.thermalReservoirs.size(); )
	{
		auto& thermalReservoir = mDraft.thermalReservoirs[index];
		auto const row = std::to_string(index);
		bool removeReservoir = false;
		ImGui::PushItemWidth(220);
		char reservoirIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(reservoirIdBuffer, thermalReservoir.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Reservoir " + row + " id").c_str(),
			reservoirIdBuffer, sizeof(reservoirIdBuffer)))
			thermalReservoir.id = reservoirIdBuffer;
		char reservoirClassBuffer[16]{};
		std::strncpy(reservoirClassBuffer, thermalReservoir.thermalClass.c_str(), sizeof(reservoirClassBuffer) - 1);
		if (ImGui::InputText(("Reservoir " + row + " thermal class").c_str(),
			reservoirClassBuffer, sizeof(reservoirClassBuffer)))
			thermalReservoir.thermalClass = reservoirClassBuffer;
		ImGui::InputDouble(("Reservoir " + row + " heat capacity kJ/K").c_str(),
			&thermalReservoir.heatCapacityKJPerK, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Reservoir " + row + " starting temperature C").c_str(),
			&thermalReservoir.startingTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Reservoir " + row + " min temperature C").c_str(),
			&thermalReservoir.minTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Reservoir " + row + " max temperature C").c_str(),
			&thermalReservoir.maxTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Reservoir " + row + " absorbed heat kW").c_str(),
			&thermalReservoir.absorbedHeatKW, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove reservoir " + row).c_str()))
			removeReservoir = true;
		ImGui::PopItemWidth();
		if (removeReservoir)
			mDraft.thermalReservoirs.erase(mDraft.thermalReservoirs.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.thermalReservoirs.size() < core::MaxStationThermalReservoirs && ImGui::Button("Add thermal reservoir"))
	{
		core::ThermalReservoirSettings thermalReservoir;
		thermalReservoir.thermalClass = "moderate";
		thermalReservoir.heatCapacityKJPerK = 100.0;
		thermalReservoir.startingTemperatureC = 20.0;
		thermalReservoir.minTemperatureC = 0.0;
		thermalReservoir.maxTemperatureC = 40.0;
		for (size_t candidate = mDraft.thermalReservoirs.size() + 1;; ++candidate)
		{
			thermalReservoir.id = "reservoir" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.thermalReservoirs.begin(), mDraft.thermalReservoirs.end(),
				[&](core::ThermalReservoirSettings const& existing) { return existing.id == thermalReservoir.id; });
			if (!taken) break;
		}
		mDraft.thermalReservoirs.push_back(std::move(thermalReservoir));
	}
	ImGui::SeparatorText("Draft radiators (m2; rejection = emissivity x view factor x area x sigma x (T^4 - T_sink^4))");
	for (size_t index = 0; index < mDraft.radiators.size(); )
	{
		auto& radiator = mDraft.radiators[index];
		auto const row = std::to_string(index);
		bool removeRadiator = false;
		ImGui::PushItemWidth(220);
		char radiatorIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(radiatorIdBuffer, radiator.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Radiator " + row + " id").c_str(),
			radiatorIdBuffer, sizeof(radiatorIdBuffer)))
			radiator.id = radiatorIdBuffer;
		char radiatorHostBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(radiatorHostBuffer, radiator.reservoirId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Radiator " + row + " reservoir").c_str(),
			radiatorHostBuffer, sizeof(radiatorHostBuffer)))
			radiator.reservoirId = radiatorHostBuffer;
		ImGui::InputDouble(("Radiator " + row + " area m2").c_str(),
			&radiator.areaM2, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Radiator " + row + " emissivity").c_str(),
			&radiator.emissivity, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Radiator " + row + " view factor").c_str(),
			&radiator.viewFactor, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Radiator " + row + " sink temperature C").c_str(),
			&radiator.sinkTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove radiator " + row).c_str()))
			removeRadiator = true;
		ImGui::PopItemWidth();
		if (removeRadiator)
			mDraft.radiators.erase(mDraft.radiators.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.radiators.size() < core::MaxStationRadiators && ImGui::Button("Add radiator"))
	{
		core::RadiatorSettings radiator;
		radiator.areaM2 = 100.0;
		radiator.emissivity = 0.9;
		radiator.viewFactor = 1.0;
		radiator.sinkTemperatureC = -270.0;
		if (!mDraft.thermalReservoirs.empty())
			radiator.reservoirId = mDraft.thermalReservoirs.front().id;
		for (size_t candidate = mDraft.radiators.size() + 1;; ++candidate)
		{
			radiator.id = "radiator" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.radiators.begin(), mDraft.radiators.end(),
				[&](core::RadiatorSettings const& existing) { return existing.id == radiator.id; });
			if (!taken) break;
		}
		mDraft.radiators.push_back(std::move(radiator));
	}
	ImGui::SeparatorText("Draft coolant loops (kg coolant; finite loop-specific inventory, normal/hazardous segregated, heat-only reservoir coupling; zero starting coolant disables the loop)");
	for (size_t index = 0; index < mDraft.coolantLoops.size(); )
	{
		auto& loop = mDraft.coolantLoops[index];
		auto const row = std::to_string(index);
		bool removeLoop = false;
		ImGui::PushItemWidth(220);
		char loopIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(loopIdBuffer, loop.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Coolant loop " + row + " id").c_str(),
			loopIdBuffer, sizeof(loopIdBuffer)))
			loop.id = loopIdBuffer;
		char hazardBuffer[16]{};
		std::strncpy(hazardBuffer, loop.hazardClass.c_str(), sizeof(hazardBuffer) - 1);
		if (ImGui::InputText(("Coolant loop " + row + " hazard class").c_str(),
			hazardBuffer, sizeof(hazardBuffer)))
			loop.hazardClass = hazardBuffer;
		char loopReservoirBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(loopReservoirBuffer, loop.reservoirId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Coolant loop " + row + " reservoir").c_str(),
			loopReservoirBuffer, sizeof(loopReservoirBuffer)))
			loop.reservoirId = loopReservoirBuffer;
		ImGui::InputDouble(("Coolant loop " + row + " coolant capacity kg").c_str(),
			&loop.coolantCapacityKg, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " starting coolant kg").c_str(),
			&loop.startingCoolantKg, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " specific heat kJ/kg K").c_str(),
			&loop.specificHeatKJPerKgK, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Coolant loop " + row + " starting temperature C").c_str(),
			&loop.startingTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " freeze setpoint C").c_str(),
			&loop.freezeProtectionSetpointC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " pump power kW").c_str(),
			&loop.pumpPowerKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " rated flow kg/s").c_str(),
			&loop.ratedFlowKgPerS, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " exchanger conductance kW/K").c_str(),
			&loop.exchangerConductanceKWPerK, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Coolant loop " + row + " heater capacity kW").c_str(),
			&loop.heaterCapacityKW, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove coolant loop " + row).c_str()))
			removeLoop = true;
		ImGui::PopItemWidth();
		if (removeLoop)
			mDraft.coolantLoops.erase(mDraft.coolantLoops.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.coolantLoops.size() < core::MaxStationCoolantLoops && ImGui::Button("Add coolant loop"))
	{
		core::CoolantLoopSettings loop;
		loop.hazardClass = "normal";
		loop.coolantCapacityKg = 100.0;
		loop.startingCoolantKg = 100.0;
		loop.specificHeatKJPerKgK = 4.0;
		loop.startingTemperatureC = 20.0;
		loop.freezeProtectionSetpointC = 5.0;
		loop.pumpPowerKW = 1.0;
		loop.ratedFlowKgPerS = 1.0;
		loop.exchangerConductanceKWPerK = 2.0;
		loop.heaterCapacityKW = 1.0;
		if (!mDraft.thermalReservoirs.empty())
			loop.reservoirId = mDraft.thermalReservoirs.front().id;
		for (size_t candidate = mDraft.coolantLoops.size() + 1;; ++candidate)
		{
			loop.id = "coolantLoop" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.coolantLoops.begin(), mDraft.coolantLoops.end(),
				[&](core::CoolantLoopSettings const& existing) { return existing.id == loop.id; });
			if (!taken) break;
		}
		mDraft.coolantLoops.push_back(std::move(loop));
	}
	ImGui::SeparatorText("Draft chillers (kW cold service; class-matched loop evaporator and reservoir condenser, COP and auxiliaries; zero rated capacity disables the chiller)");
	for (size_t index = 0; index < mDraft.chillers.size(); )
	{
		auto& chiller = mDraft.chillers[index];
		auto const row = std::to_string(index);
		bool removeChiller = false;
		ImGui::PushItemWidth(220);
		char chillerIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(chillerIdBuffer, chiller.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Chiller " + row + " id").c_str(),
			chillerIdBuffer, sizeof(chillerIdBuffer)))
			chiller.id = chillerIdBuffer;
		char chillerClassBuffer[16]{};
		std::strncpy(chillerClassBuffer, chiller.thermalClass.c_str(), sizeof(chillerClassBuffer) - 1);
		if (ImGui::InputText(("Chiller " + row + " thermal class").c_str(),
			chillerClassBuffer, sizeof(chillerClassBuffer)))
			chiller.thermalClass = chillerClassBuffer;
		char coldLoopBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(coldLoopBuffer, chiller.coldLoopId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Chiller " + row + " cold loop").c_str(),
			coldLoopBuffer, sizeof(coldLoopBuffer)))
			chiller.coldLoopId = coldLoopBuffer;
		char condenserBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(condenserBuffer, chiller.condenserReservoirId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Chiller " + row + " condenser reservoir").c_str(),
			condenserBuffer, sizeof(condenserBuffer)))
			chiller.condenserReservoirId = condenserBuffer;
		ImGui::InputDouble(("Chiller " + row + " rated cooling capacity kW").c_str(),
			&chiller.ratedCoolingCapacityKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Chiller " + row + " COP").c_str(),
			&chiller.cop, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Chiller " + row + " auxiliary power kW").c_str(),
			&chiller.auxiliaryPowerKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Chiller " + row + " chilled setpoint C").c_str(),
			&chiller.chilledSetpointC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Chiller " + row + " max condensing temperature C").c_str(),
			&chiller.maxCondensingTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove chiller " + row).c_str()))
			removeChiller = true;
		ImGui::PopItemWidth();
		if (removeChiller)
			mDraft.chillers.erase(mDraft.chillers.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.chillers.size() < core::MaxStationChillers && ImGui::Button("Add chiller"))
	{
		core::ChillerSettings chiller;
		chiller.thermalClass = "moderate";
		chiller.ratedCoolingCapacityKW = 5.0;
		chiller.cop = 3.0;
		chiller.auxiliaryPowerKW = 0.2;
		chiller.chilledSetpointC = 5.0;
		chiller.maxCondensingTemperatureC = 45.0;
		if (!mDraft.coolantLoops.empty())
			chiller.coldLoopId = mDraft.coolantLoops.front().id;
		// Prefer a condenser sink that is not the cold loop's own host:
		// rejecting into the reservoir the cold side cools is a heater,
		// so the default avoids that refusal when another reservoir exists.
		if (!mDraft.thermalReservoirs.empty())
		{
			std::string coldHost;
			for (auto const& loop : mDraft.coolantLoops)
				if (loop.id == chiller.coldLoopId) coldHost = loop.reservoirId;
			chiller.condenserReservoirId = mDraft.thermalReservoirs.back().id;
			for (auto const& reservoir : mDraft.thermalReservoirs)
				if (reservoir.id != coldHost) chiller.condenserReservoirId = reservoir.id;
		}
		for (size_t candidate = mDraft.chillers.size() + 1;; ++candidate)
		{
			chiller.id = "chiller" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.chillers.begin(), mDraft.chillers.end(),
				[&](core::ChillerSettings const& existing) { return existing.id == chiller.id; });
			if (!taken) break;
		}
		mDraft.chillers.push_back(std::move(chiller));
	}
	ImGui::SeparatorText("Draft generators (kW/kWh; fuel is an energy-unit Resource store; startup energy and a qualified cooling sink gate the start; zero installed capacity disables the source)");
	for (size_t index = 0; index < mDraft.generators.size(); )
	{
		auto& generator = mDraft.generators[index];
		auto const row = std::to_string(index);
		bool removeGenerator = false;
		ImGui::PushItemWidth(220);
		char generatorIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(generatorIdBuffer, generator.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Generator " + row + " id").c_str(),
			generatorIdBuffer, sizeof(generatorIdBuffer)))
			generator.id = generatorIdBuffer;
		char fuelStoreBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(fuelStoreBuffer, generator.fuelStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Generator " + row + " fuel store").c_str(),
			fuelStoreBuffer, sizeof(fuelStoreBuffer)))
			generator.fuelStoreId = fuelStoreBuffer;
		char generatorClassBuffer[16]{};
		std::strncpy(generatorClassBuffer, generator.thermalClass.c_str(), sizeof(generatorClassBuffer) - 1);
		if (ImGui::InputText(("Generator " + row + " thermal class").c_str(),
			generatorClassBuffer, sizeof(generatorClassBuffer)))
			generator.thermalClass = generatorClassBuffer;
		char generatorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(generatorSinkBuffer, generator.heatSinkId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Generator " + row + " heat sink").c_str(),
			generatorSinkBuffer, sizeof(generatorSinkBuffer)))
			generator.heatSinkId = generatorSinkBuffer;
		ImGui::InputDouble(("Generator " + row + " rated output kW").c_str(),
			&generator.ratedOutputKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " efficiency").c_str(),
			&generator.efficiency, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " ramp kW per second").c_str(),
			&generator.maxRampKWPerSecond, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " commanded output kW").c_str(),
			&generator.commandedOutputKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " startup power kW").c_str(),
			&generator.startupPowerKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " startup seconds").c_str(),
			&generator.startupSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " shutdown heat kW").c_str(),
			&generator.shutdownHeatKW, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " shutdown seconds").c_str(),
			&generator.shutdownSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Generator " + row + " max sink temperature C").c_str(),
			&generator.maxSinkTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove generator " + row).c_str()))
			removeGenerator = true;
		ImGui::PopItemWidth();
		if (removeGenerator)
			mDraft.generators.erase(mDraft.generators.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.generators.size() < core::MaxStationGenerators && ImGui::Button("Add generator"))
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
		for (auto const& store : mDraft.stores)
			if (store.unit == "kJ" || store.unit == "MJ"
				|| store.unit == "Wh" || store.unit == "kWh")
			{ generator.fuelStoreId = store.id; break; }
		// Prefer a generation-class sink when one exists; otherwise the
		// first reservoir (validation will surface a class mismatch).
		if (!mDraft.thermalReservoirs.empty())
		{
			generator.heatSinkId = mDraft.thermalReservoirs.back().id;
			for (auto const& reservoir : mDraft.thermalReservoirs)
				if (reservoir.thermalClass == generator.thermalClass)
				{ generator.heatSinkId = reservoir.id; break; }
		}
		for (size_t candidate = mDraft.generators.size() + 1;; ++candidate)
		{
			generator.id = "generator" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.generators.begin(), mDraft.generators.end(),
				[&](core::GeneratorSettings const& existing) { return existing.id == generator.id; });
			if (!taken) break;
		}
		mDraft.generators.push_back(std::move(generator));
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
	ImGui::SeparatorText("Constituent balances (current + lost - delivered must equal starting within tolerance; dry-mass basis)");
	if (snapshot.constituentBalances.empty()) ImGui::TextDisabled("No compound Resource stores authored.");
	for (auto const& balance : snapshot.constituentBalances)
		ImGui::Text("Constituent %s (%s): starting %.6f, current %.6f, lost %.6f, delivered %.6f, residual %.9f (tolerance %.1e)",
			balance.constituent.c_str(), balance.unit.c_str(), balance.starting,
			balance.current, balance.lost, balance.delivered, balance.residual, snapshot.balanceTolerance);
	ImGui::SeparatorText("Boundary balances (current + lost - delivered must stay within tolerance)");
	if (snapshot.balances.empty()) ImGui::TextDisabled("No Resource stores authored.");
	for (auto const& balance : snapshot.balances)
		ImGui::Text("Balance %s: starting %.3f, current %.3f, lost %.3f, delivered %.3f, residual %.9f (tolerance %.1e)",
			balance.unit.c_str(), balance.totalStarting, balance.totalCurrent,
			balance.lost, balance.delivered, balance.residual, snapshot.balanceTolerance);
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
		if (load.heatRoutedKW > 0.0)
			ImGui::Text("  heat routed to reservoir: %.3f kW (not also exported)", load.heatRoutedKW);
		for (auto const& flow : load.materialExchanges)
			ImGui::Text("  exchange %s (%s): requested %.3f, completed %.3f, unmet %.3f",
				flow.storeId.c_str(), flow.unit.c_str(), flow.requested, flow.completed,
				flow.unmet);
	}
	ImGui::Text("Power totals: generation %.3f kW, requested %.3f, supplied %.3f, unmet %.3f, unmet critical %.3f, surplus %.3f, exported heat %.3f kW, battery discharge %.3f kW, battery charge %.3f kW, battery shortfall %.3f kW, coolant pump %.3f kW, freeze heating %.3f kW, chiller power %.3f kW, generator output %.3f kW, generator startup %.3f kW, generator waste %.3f kW, generator decay %.3f kW",
		snapshot.power.availableGenerationKW, snapshot.power.totalRequestedKW,
		snapshot.power.totalSuppliedKW, snapshot.power.totalUnmetKW,
		snapshot.power.unmetCriticalKW, snapshot.power.surplusKW, snapshot.power.exportedHeatKW,
		snapshot.power.batteryDischargeKW, snapshot.power.batteryChargeKW,
		snapshot.power.batteryShortfallKW, snapshot.power.coolantPumpKW,
		snapshot.power.coolantHeaterKW, snapshot.power.chillerPowerKW,
		snapshot.power.generatorOutputKW, snapshot.power.generatorStartupDrawKW,
		snapshot.power.generatorWasteHeatKW, snapshot.power.generatorDecayHeatKW);
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
	ImGui::SeparatorText("Resupply deliveries (live; arrivals land first each tick, the destination accepts only what fits, refusals stay accounted)");
	if (snapshot.deliveries.empty()) ImGui::TextDisabled("No resupply deliveries authored.");
	else
		ImGui::Text("Next delivery due at %.3f s", snapshot.nextDeliverySeconds);
	for (auto const& delivery : snapshot.deliveries)
	{
		char const* status = "idle";
		switch (delivery.status)
		{
		case core::StationTransferStatus::Full: status = "accepted"; break;
		case core::StationTransferStatus::Partial: status = "partially-accepted"; break;
		case core::StationTransferStatus::Rejected: status = "refused"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s -> %s (%s): %s | last at %.3f s: %.3f of %.3f accepted, %.3f rejected | total accepted %.3f, rejected %.3f | %llu occurrence(s) | priority %d | %s",
			delivery.id.c_str(), delivery.storeId.c_str(), delivery.unit.c_str(), status,
			delivery.lastOccurrenceSeconds, delivery.lastAccepted, delivery.lastScheduled,
			delivery.lastRejected, delivery.acceptedTotal, delivery.rejectedTotal,
			static_cast<unsigned long long>(delivery.occurrencesCompleted), delivery.priority,
			delivery.arrivesQualified ? "arrives usable" : "arrives held awaiting clearance");
		if (delivery.exhausted)
			ImGui::Text("  one-off schedule spent: no further arrivals");
		else
			ImGui::Text("  next arrival at %.3f s (%s)", delivery.nextOccurrenceSeconds,
				delivery.recurrenceSeconds > 0.0
					? ("recurring every " + std::to_string(delivery.recurrenceSeconds) + " s").c_str()
					: "one-off");
	}
	if (snapshot.enabled && !snapshot.healthy)
		ImGui::TextWrapped("Station software failure: the World is paused. The failed tick was not committed. Reset the World to reconstruct Station systems; resume cannot retry failed scripts.");
	ImGui::SeparatorText("Thermal reservoirs (live; temperature integrates heat in minus radiator rejection; limits are reported, never clamped)");
	if (snapshot.thermal.reservoirs.empty()) ImGui::TextDisabled("No thermal reservoirs authored.");
	for (auto const& thermalReservoir : snapshot.thermal.reservoirs)
	{
		ImGui::Text("%s (%s): %.3f C | limits %.3f..%.3f C | capacity %.3f kJ/K | heat in %.3f kW (routed %.3f, absorbed %.3f), rejected %.3f kW, net %.3f kW",
			thermalReservoir.id.c_str(), thermalReservoir.thermalClass.c_str(),
			thermalReservoir.temperatureC, thermalReservoir.minTemperatureC,
			thermalReservoir.maxTemperatureC, thermalReservoir.heatCapacityKJPerK,
			thermalReservoir.lastHeatInKW, thermalReservoir.lastHeatRoutedKW,
			thermalReservoir.absorbedHeatKW, thermalReservoir.lastRejectedKW,
			thermalReservoir.lastNetKW);
		ImGui::Text("  energy ledger (kJ): starting %.3f, current %.3f, absorbed %.3f, routed %.3f, coolant exchange %.3f, condenser %.3f, rejected %.3f, residual %.9f (tolerance %.1e)",
			thermalReservoir.startingEnergyKJ, thermalReservoir.currentEnergyKJ,
			thermalReservoir.absorbedEnergyKJ, thermalReservoir.routedEnergyKJ,
			thermalReservoir.loopEnergyKJ, thermalReservoir.condenserEnergyKJ,
			thermalReservoir.rejectedEnergyKJ, thermalReservoir.residualKJ, snapshot.balanceTolerance);
		if (thermalReservoir.aboveLimit)
			ImGui::Text("  ABOVE the declared maximum service temperature");
		if (thermalReservoir.belowLimit)
			ImGui::Text("  BELOW the declared minimum service temperature");
	}
	ImGui::SeparatorText("Radiators (live; rejection follows the emitting temperature and the authored sink)");
	if (snapshot.thermal.radiators.empty()) ImGui::TextDisabled("No radiators authored.");
	for (auto const& radiator : snapshot.thermal.radiators)
		ImGui::Text("%s -> %s: area %.3f m2, emissivity %.3f, view %.3f, sink %.3f C | rejected %.3f kW",
			radiator.id.c_str(), radiator.reservoirId.c_str(), radiator.areaM2,
			radiator.emissivity, radiator.viewFactor, radiator.sinkTemperatureC,
			radiator.lastRejectedKW);
	ImGui::Text("Thermal totals: absorbed %.3f kW, routed %.3f kW, rejected %.3f kW, circulation %.3f kg/s, freeze heating %.3f kW, cold provided %.3f kW, cold unmet %.3f kW, condenser heat %.3f kW",
		snapshot.thermal.totalAbsorbedKW, snapshot.thermal.totalRoutedKW,
		snapshot.thermal.totalRejectedKW, snapshot.thermal.totalCirculationKgPerS,
		snapshot.thermal.totalFreezeHeatingKW, snapshot.thermal.totalCoolingProvidedKW,
		snapshot.thermal.totalCoolingUnmetKW, snapshot.thermal.totalCondenserHeatKW);
	ImGui::SeparatorText("Coolant loops (live; finite loop-specific coolant, circulation capped by inventory and power, freeze exposure reported never clamped)");
	if (snapshot.coolantLoops.empty()) ImGui::TextDisabled("No coolant loops authored.");
	for (auto const& loop : snapshot.coolantLoops)
	{
		char const* status = "idle";
		switch (loop.status)
		{
		case core::StationTransferStatus::Full: status = "full service"; break;
		case core::StationTransferStatus::Partial: status = "partial service"; break;
		case core::StationTransferStatus::Rejected: status = "service refused"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] -> %s: %s | coolant %.3f of %.3f kg (%.0f%% full) | flow %.3f of %.3f kg/s (circulation %.1f%%) | pump %.3f of %.3f kW | heater %.3f of %.3f kW",
			loop.id.c_str(), loop.hazardClass.c_str(), loop.reservoirId.c_str(), status,
			loop.coolantKg, loop.coolantCapacityKg, loop.fillFraction * 100.0,
			loop.flowKgPerS, loop.ratedFlowKgPerS, loop.circulationFraction * 100.0,
			loop.pumpSuppliedKW, loop.pumpDemandKW,
			loop.heaterSuppliedKW, loop.heaterDemandKW);
		ImGui::Text("  temperature %.3f C (setpoint %.3f C, exchange %+.3f kW) | energy ledger (kJ): starting %.3f, current %.3f, pump work %.3f, heater %.3f, exchanged %.3f, chiller extracted %.3f, residual %.9f (tolerance %.1e)",
			loop.loopTemperatureC, loop.freezeProtectionSetpointC, loop.exchangeIntoLoopKW,
			loop.startingEnergyKJ, loop.currentEnergyKJ, loop.pumpWorkEnergyKJ,
			loop.heaterEnergyKJ, loop.exchangedEnergyKJ, loop.extractedEnergyKJ,
			loop.residualKJ, snapshot.balanceTolerance);
		if (loop.disabled)
			ImGui::Text("  DISABLED: the loop holds no coolant, so it pumps no flow and heats nothing");
		if (loop.belowFreezeSetpoint)
			ImGui::Text("  BELOW the freeze-protection setpoint: freeze exposure with %.3f kW of heating demand unmet",
				loop.heaterUnmetKW);
	}
	ImGui::SeparatorText("Chillers (live; cold service capped by cold-loop circulation, gated at the chilled setpoint, refused while the condenser sink is over the maximum condensing temperature)");
	if (snapshot.chillers.empty()) ImGui::TextDisabled("No chillers authored.");
	for (auto const& chiller : snapshot.chillers)
	{
		char const* status = "idle";
		switch (chiller.status)
		{
		case core::StationTransferStatus::Full: status = "full service"; break;
		case core::StationTransferStatus::Partial: status = "partial service"; break;
		case core::StationTransferStatus::Rejected: status = "service refused"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s (%s) %s -> condenser %s: %s | cold duty %.3f of %.3f kW (unmet %.3f) | deliverable %.3f kW at %.1f%% circulation | power %.3f of %.3f kW (COP %.2f, aux %.3f kW) | condenser heat %.3f kW",
			chiller.id.c_str(), chiller.thermalClass.c_str(), chiller.coldLoopId.c_str(),
			chiller.condenserReservoirId.c_str(), status,
			chiller.coolingKW, chiller.coolingDemandKW, chiller.coolingUnmetKW,
			chiller.deliverableKW, chiller.coldLoopCirculationFraction * 100.0,
			chiller.powerSuppliedKW, chiller.powerDemandKW, chiller.cop,
			chiller.auxiliaryPowerKW, chiller.condenserHeatKW);
		ImGui::Text("  cold side %.3f C (chilled setpoint %.3f C) | sink %.3f C (max condensing %.3f C) | energy ledger (kJ): extracted %.3f, work %.3f, condenser %.3f, residual %.9f (tolerance %.1e)",
			chiller.coldLoopTemperatureC, chiller.chilledSetpointC,
			chiller.sinkTemperatureC, chiller.maxCondensingTemperatureC,
			chiller.extractedEnergyKJ, chiller.workEnergyKJ, chiller.condenserEnergyKJ,
			chiller.residualKJ, snapshot.balanceTolerance);
		if (chiller.disabled)
			ImGui::Text("  DISABLED: zero rated cooling capacity supplies no cold service");
		if (chiller.atChilledSetpoint)
			ImGui::Text("  cold side at its chilled setpoint: no extraction demanded");
		if (chiller.sinkOverMaxCondensing)
			ImGui::Text("  SINK OVER the maximum condensing temperature: cold service refused");
		if (!chiller.admitted && !chiller.disabled && !chiller.sinkOverMaxCondensing
			&& chiller.coolingDemandKW > 0.0)
			ImGui::Text("  not admitted: %s", chiller.blockReason.c_str());
	}
	ImGui::Text("Cold service by class:");
	for (auto const& cooling : snapshot.thermal.coolingByClass)
		ImGui::Text("  %s: capacity %.3f kW, provided %.3f, unmet %.3f, work %.3f, condenser %.3f kW",
			cooling.thermalClass.c_str(), cooling.capacityKW, cooling.providedKW,
			cooling.unmetKW, cooling.workKW, cooling.condenserKW);
	ImGui::SeparatorText("Generators (live; fuelled output gated by startup energy, cooling qualification, ramp limits and the finite fuel inventory; shutdown heat persists after output stops)");
	if (snapshot.power.generators.empty()) ImGui::TextDisabled("No generators authored.");
	for (auto const& generator : snapshot.power.generators)
	{
		char const* phase = "stopped";
		switch (generator.phase)
		{
		case core::StationGeneratorPhase::Starting: phase = "starting"; break;
		case core::StationGeneratorPhase::Running: phase = "running"; break;
		case core::StationGeneratorPhase::Shutdown: phase = "shutdown"; break;
		case core::StationGeneratorPhase::Stopped: break;
		}
		char const* status = "idle";
		switch (generator.status)
		{
		case core::StationTransferStatus::Full: status = "full"; break;
		case core::StationTransferStatus::Partial: status = "partial"; break;
		case core::StationTransferStatus::Rejected: status = "rejected"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s (%s) fuel %s -> sink %s: %s / %s | output %.3f of commanded %.3f kW (rated %.3f, ramp %.3f kW/s, eff %.2f) | fuel %.4f kWh, waste %.3f kW | startup %.3f of %.3f kW (%.1f of %.1f kJ) | decay %.3f kW",
			generator.id.c_str(), generator.thermalClass.c_str(), generator.fuelStoreId.c_str(),
			generator.heatSinkId.c_str(), phase, status,
			generator.outputKW, generator.commandedOutputKW, generator.ratedOutputKW,
			generator.maxRampKWPerSecond, generator.efficiency,
			generator.fuelDrawKWh, generator.wasteHeatKW,
			generator.startupDrawKW, generator.startupPowerKW,
			generator.startupReceivedKJ, generator.startupRequiredKJ,
			generator.decayHeatKW);
		ImGui::Text("  sink %.3f C (max %.3f C, cooling %s) | fuel ledger (kJ): fuel %.3f, electrical %.3f, waste %.3f, startup %.3f, decay %.3f, residual %.9f (tolerance %.1e)",
			generator.sinkTemperatureC, generator.maxSinkTemperatureC,
			generator.coolingAvailable ? "qualified" : "NOT qualified",
			generator.fuelEnergyKJ, generator.electricalKJ, generator.wasteHeatKJ,
			generator.startupHeatKJ, generator.decayHeatKJ,
			generator.residualKJ, snapshot.balanceTolerance);
		if (generator.disabled)
			ImGui::Text("  DISABLED: zero installed capacity supplies no electrical source");
		if (generator.coolingRequired && !generator.coolingAvailable)
			ImGui::Text("  cooling REQUIRED but not qualified: shutdown heat persists without a qualified sink");
		if (!generator.blockReason.empty())
			ImGui::Text("  blocked: %s", generator.blockReason.c_str());
	}
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
