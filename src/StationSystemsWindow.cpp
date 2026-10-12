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
	ImGui::TextWrapped("Slice 5: authored finite Resource stores, bounded transfers, compound-resource conversions, solar generation, External equipment load supply, rechargeable batteries, thermal reservoirs with radiator rejection, coolant loops with freeze-protection heating, and chillers supplying class-matched cold services. Transfers move only qualified material above protected reserves, respect destination capacity, and allocate competing requests by priority then declaration order. Compound stores declare constituent compositions on a dry-mass basis; conversions are validated to never create tracked constituents. Solar output follows installed capacity and the illumination/eclipse profile; zero capacity disables an array. External loads declare demand, operating profile, heat, thermal class, material exchanges, a protected critical floor, priority and allowed curtailment; allocated power never exceeds generation plus battery output and cooling-infeasible loads are not admitted. Batteries separate nameplate and usable energy: the reserve floor (nameplate minus usable) is never dischargeable, charge and discharge efficiencies book their losses, discharge covers deficits on the same allocation pool as generation, and charging takes only surplus. Thermal reservoirs integrate temperature from their effective capacity under absorbed and routed equipment heat; radiators reject heat by the Stefan-Boltzmann law at the emitting temperature against the authored sink, emissivity and view factor. A load's emitted heat is booked exactly once: routed into its class-compatible reservoir or exported directly, never both. Declared temperature limits are reported, never clamped. Coolant loops hold a finite, loop-specific coolant inventory segregated by hazard class (coolant is never potable or nutrient water); the pump circulates only the coolant the loop actually holds, so an underfilled loop flows less and a dry loop flows nothing, and the freeze-protection heater thermostats the loop up to its setpoint within its capacity. Pump work and heater heat enter the loop exactly once and reach the reservoir only through the circulation-gated heat-only exchanger; the loop draw rides the same electrical pool as external loads. Chillers supply a cold service of their declared class through a class-matched coolant-loop evaporator and reservoir condenser: the delivered duty is capped by the loop's actual circulation, gated at the chilled setpoint, and refused while the sink is at or above the maximum condensing temperature; work at the configured COP plus auxiliaries rides the same electrical pool, and condenser heat equals extracted heat plus the work and auxiliaries actually supplied, booked once. Fuelled generators convert a finite energy-unit fuel inventory into bus electrical output at the declared efficiency: output is bounded by the installed rating and the ramp limit, starting only completes once the startup energy has actually been drawn from the shared pool while the class-matched sink is below its maximum sink temperature, and after output stops the configured shutdown heat and its cooling requirement persist for the decay period; waste, startup and decay heat route exactly once into the sink and the fuel ledger closes over fuel = electrical + waste. Within a tick, generators and fuel-cell backup units produce first, then power allocates, then coolant loops and chillers run, then compartment atmospheres cycle, crews metabolise and compartments exchange, then thermal integrates, then transfers, then conversions. Compartment atmospheres model an ideal-gas mixture in a fixed volume: absolute and partial pressures derive from the gas inventory and temperature, humidity caps at Magnus saturation with the excess condensing and releasing its latent heat into the compartment, occupancy is an authored load assignment whose oxygen draw is capped by the inventory (CO2 and metabolic heat scale with what was actually consumed), leakage is a constant total-mass rate capped by the inventory, and scheduled airlock cycles remove authored mass with an explicit recovered-versus-lost split; every gram leaving a compartment is booked as leaked, recovered or lost, compartments never mix automatically, and declared pressure and gas-quality limits are reported, never clamped. Crews are an authored population with an activity profile, never derived from World Agents: metabolism draws food and water from compound stores and oxygen from the compartment, and returns CO2, moisture and metabolic heat to the compartment and urine and organic waste to their own finite compound stores. One uniform metabolic scale per tick - the scarcest supply or tightest waste headroom - throttles every pathway together, so absent material never generates nominal waste; requested versus actual is explicit per pathway, the authored coefficients must close over mass, constituents and energy, and the consumed mass the returns do not carry away is reported as explicit retained body mass. Observation interval is a provisional display/event setting, not a separate simulation clock.");
	auto const authored = world->getStationSystemsSettings();
	ImGui::Text("Authored start conditions: %s, observation interval %.2f s, %zu resource store(s), %zu transfer(s), %zu conversion(s), %zu solar array(s), %zu external load(s), %zu battery(ies), %zu resupply delivery(ies), %zu thermal reservoir(s), %zu radiator(s), %zu coolant loop(s), %zu chiller(s), %zu generator(s), %zu atmosphere compartment(s), %zu airlock cycle(s), %zu crew(s), %zu water collector(s), %zu water treatment(s), %zu electrolyzer(s), %zu CO2 capture machine(s) and %zu fuel-cell backup unit(s)",
		authored.enabled ? "enabled" : "disabled", authored.observationIntervalSeconds,
		authored.stores.size(), authored.transfers.size(), authored.conversions.size(),
		authored.solarArrays.size(), authored.equipmentLoads.size(), authored.batteries.size(),
		authored.deliveries.size(), authored.thermalReservoirs.size(), authored.radiators.size(),
		authored.coolantLoops.size(), authored.chillers.size(), authored.generators.size(),
		authored.atmospheres.size(), authored.airlockCycles.size(), authored.crews.size(),
		authored.waterCollectors.size(), authored.waterTreatments.size(),
		authored.electrolyzers.size(), authored.co2Captures.size(),
		authored.fuelCells.size());
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
	ImGui::SeparatorText("Draft atmospheres (m3/kg/C/kPa; ideal-gas partial pressures; occupancy is an authored load assignment; vapour caps at saturation and condenses with latent heat; leak is a constant total-mass rate; limits are reported, never clamped)");
	for (size_t index = 0; index < mDraft.atmospheres.size(); )
	{
		auto& atmosphere = mDraft.atmospheres[index];
		auto const row = std::to_string(index);
		bool removeAtmosphere = false;
		ImGui::PushItemWidth(220);
		char atmosphereIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(atmosphereIdBuffer, atmosphere.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Atmosphere " + row + " id").c_str(),
			atmosphereIdBuffer, sizeof(atmosphereIdBuffer)))
			atmosphere.id = atmosphereIdBuffer;
		char hazardBuffer[16]{};
		std::strncpy(hazardBuffer, atmosphere.hazardClass.c_str(), sizeof(hazardBuffer) - 1);
		if (ImGui::InputText(("Atmosphere " + row + " hazard class").c_str(),
			hazardBuffer, sizeof(hazardBuffer)))
			atmosphere.hazardClass = hazardBuffer;
		ImGui::InputDouble(("Atmosphere " + row + " volume m3").c_str(),
			&atmosphere.volumeM3, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Atmosphere " + row + " starting oxygen kg").c_str(),
			&atmosphere.startingOxygenKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Atmosphere " + row + " starting nitrogen kg").c_str(),
			&atmosphere.startingNitrogenKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Atmosphere " + row + " starting CO2 kg").c_str(),
			&atmosphere.startingCarbonDioxideKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Atmosphere " + row + " starting water vapour kg").c_str(),
			&atmosphere.startingWaterVapourKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Atmosphere " + row + " starting temperature C").c_str(),
			&atmosphere.startingTemperatureC, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Atmosphere " + row + " thermal capacity kJ/K").c_str(),
			&atmosphere.thermalCapacityKJPerK, 0.0, 0.0, "%.3f");
		char atmosphereReservoirBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(atmosphereReservoirBuffer, atmosphere.reservoirId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Atmosphere " + row + " reservoir (empty = adiabatic)").c_str(),
			atmosphereReservoirBuffer, sizeof(atmosphereReservoirBuffer)))
			atmosphere.reservoirId = atmosphereReservoirBuffer;
		ImGui::InputDouble(("Atmosphere " + row + " exchanger conductance kW/K").c_str(),
			&atmosphere.exchangerConductanceKWPerK, 0.0, 0.0, "%.4f");
		ImGui::InputInt(("Atmosphere " + row + " occupants").c_str(),
			&atmosphere.occupants, 1, 0);
		ImGui::InputDouble(("Atmosphere " + row + " O2 use per occupant kg/s").c_str(),
			&atmosphere.oxygenUsePerOccupantKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Atmosphere " + row + " CO2 produced per occupant kg/s").c_str(),
			&atmosphere.co2ProductionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Atmosphere " + row + " sensible heat per occupant kW").c_str(),
			&atmosphere.sensibleHeatPerOccupantKW, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Atmosphere " + row + " moisture added per occupant kg/s").c_str(),
			&atmosphere.moistureAdditionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Atmosphere " + row + " leak rate kg/s").c_str(),
			&atmosphere.leakRateKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Atmosphere " + row + " min pressure kPa").c_str(),
			&atmosphere.minPressureKPa, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Atmosphere " + row + " max pressure kPa").c_str(),
			&atmosphere.maxPressureKPa, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Atmosphere " + row + " min O2 partial pressure kPa").c_str(),
			&atmosphere.minOxygenPartialPressureKPa, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Atmosphere " + row + " max CO2 partial pressure kPa").c_str(),
			&atmosphere.maxCarbonDioxidePartialPressureKPa, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove atmosphere " + row).c_str()))
			removeAtmosphere = true;
		ImGui::PopItemWidth();
		if (removeAtmosphere)
			mDraft.atmospheres.erase(mDraft.atmospheres.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.atmospheres.size() < core::MaxStationAtmospheres && ImGui::Button("Add atmosphere"))
	{
		core::AtmosphereSettings atmosphere;
		atmosphere.hazardClass = "normal";
		atmosphere.volumeM3 = 100.0;
		atmosphere.startingOxygenKg = 20.0;
		atmosphere.startingNitrogenKg = 80.0;
		atmosphere.startingTemperatureC = 20.0;
		atmosphere.thermalCapacityKJPerK = 100.0;
		atmosphere.minPressureKPa = 80.0;
		atmosphere.maxPressureKPa = 120.0;
		// Prefer coupling to the last drafted reservoir so a freshly
		// drafted atmosphere is thermally live by default.
		if (!mDraft.thermalReservoirs.empty())
			atmosphere.reservoirId = mDraft.thermalReservoirs.back().id;
		for (size_t candidate = mDraft.atmospheres.size() + 1;; ++candidate)
		{
			atmosphere.id = "compartment" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.atmospheres.begin(), mDraft.atmospheres.end(),
				[&](core::AtmosphereSettings const& existing) { return existing.id == atmosphere.id; });
			if (!taken) break;
		}
		mDraft.atmospheres.push_back(std::move(atmosphere));
	}
	ImGui::SeparatorText("Draft airlock cycles (kg/seconds; scheduled removals on the shared scenario schedule order; the recovered fraction is retained in the receiver, the remainder is lost through the outer hatch)");
	for (size_t index = 0; index < mDraft.airlockCycles.size(); )
	{
		auto& cycle = mDraft.airlockCycles[index];
		auto const row = std::to_string(index);
		bool removeCycle = false;
		ImGui::PushItemWidth(220);
		char cycleIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cycleIdBuffer, cycle.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Airlock cycle " + row + " id").c_str(),
			cycleIdBuffer, sizeof(cycleIdBuffer)))
			cycle.id = cycleIdBuffer;
		char cycleAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cycleAtmosphereBuffer, cycle.atmosphereId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Airlock cycle " + row + " atmosphere").c_str(),
			cycleAtmosphereBuffer, sizeof(cycleAtmosphereBuffer)))
			cycle.atmosphereId = cycleAtmosphereBuffer;
		ImGui::InputDouble(("Airlock cycle " + row + " gas mass kg").c_str(),
			&cycle.gasMassKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Airlock cycle " + row + " first occurrence s").c_str(),
			&cycle.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Airlock cycle " + row + " recurrence s (0 = one-off)").c_str(),
			&cycle.recurrenceSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Airlock cycle " + row + " recovered fraction").c_str(),
			&cycle.recoveredFraction, 0.0, 0.0, "%.3f");
		ImGui::InputInt(("Airlock cycle " + row + " priority").c_str(),
			&cycle.priority, 1, 0);
		ImGui::SameLine();
		if (ImGui::Button(("Remove airlock cycle " + row).c_str()))
			removeCycle = true;
		ImGui::PopItemWidth();
		if (removeCycle)
			mDraft.airlockCycles.erase(mDraft.airlockCycles.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.airlockCycles.size() < core::MaxStationAirlockCycles && ImGui::Button("Add airlock cycle"))
	{
		core::AirlockCycleSettings cycle;
		cycle.gasMassKg = 1.0;
		cycle.recurrenceSeconds = 60.0;
		cycle.recoveredFraction = 0.95;
		if (!mDraft.atmospheres.empty())
			cycle.atmosphereId = mDraft.atmospheres.back().id;
		for (size_t candidate = mDraft.airlockCycles.size() + 1;; ++candidate)
		{
			cycle.id = "airlockCycle" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.airlockCycles.begin(), mDraft.airlockCycles.end(),
				[&](core::AirlockCycleSettings const& existing) { return existing.id == cycle.id; });
			if (!taken) break;
		}
		mDraft.airlockCycles.push_back(std::move(cycle));
	}
	ImGui::SeparatorText("Draft crews (authored population; per-crew metabolic rates per second at full activity; food/water/urine/waste are the referenced compound store's units, gases kg/s, heat kW; crew are never derived from World Agents)");
	for (size_t index = 0; index < mDraft.crews.size(); )
	{
		auto& crew = mDraft.crews[index];
		auto const row = std::to_string(index);
		bool removeCrew = false;
		ImGui::PushItemWidth(220);
		char crewIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(crewIdBuffer, crew.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Crew " + row + " id").c_str(), crewIdBuffer, sizeof(crewIdBuffer)))
			crew.id = crewIdBuffer;
		char crewAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(crewAtmosphereBuffer, crew.atmosphereId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Crew " + row + " atmosphere").c_str(),
			crewAtmosphereBuffer, sizeof(crewAtmosphereBuffer)))
			crew.atmosphereId = crewAtmosphereBuffer;
		ImGui::InputInt(("Crew " + row + " count").c_str(), &crew.crewCount, 1, 0);
		bool crewPeriodic = crew.activityProfile.periodSeconds > 0.0;
		if (ImGui::Checkbox(("Crew " + row + " periodic activity").c_str(), &crewPeriodic))
		{
			if (crewPeriodic)
			{
				if (crew.activityProfile.periodSeconds <= 0.0) crew.activityProfile.periodSeconds = 120.0;
				if (crew.activityProfile.segments.empty())
					crew.activityProfile.segments.push_back({0.0, 1.0});
			}
			else
			{
				crew.activityProfile.periodSeconds = 0.0;
				crew.activityProfile.segments.clear();
			}
		}
		if (!crewPeriodic)
			ImGui::InputDouble(("Crew " + row + " constant activity level").c_str(),
				&crew.activityProfile.constantLevel, 0.0, 0.0, "%.3f");
		else
		{
			ImGui::InputDouble(("Crew " + row + " activity period seconds").c_str(),
				&crew.activityProfile.periodSeconds, 0.0, 0.0, "%.1f");
			for (size_t segment = 0; segment < crew.activityProfile.segments.size(); ++segment)
			{
				auto& stretch = crew.activityProfile.segments[segment];
				auto const segmentRow = "Crew " + row + " activity segment " + std::to_string(segment);
				ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
				ImGui::SameLine();
				ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
				ImGui::SameLine();
				if (ImGui::Button(("Remove " + segmentRow).c_str()))
					crew.activityProfile.segments.erase(
						crew.activityProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
				}
			if (crew.activityProfile.segments.size() < core::MaxStationProfileSegments
				&& ImGui::Button(("Add activity segment to crew " + row).c_str()))
				crew.activityProfile.segments.push_back(
					{crew.activityProfile.periodSeconds * 0.5, 0.0});
		}
		char crewFoodBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(crewFoodBuffer, crew.foodStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Crew " + row + " food store").c_str(),
			crewFoodBuffer, sizeof(crewFoodBuffer)))
			crew.foodStoreId = crewFoodBuffer;
		char crewWaterBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(crewWaterBuffer, crew.waterStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Crew " + row + " water store").c_str(),
			crewWaterBuffer, sizeof(crewWaterBuffer)))
			crew.waterStoreId = crewWaterBuffer;
		char crewUrineBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(crewUrineBuffer, crew.urineStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Crew " + row + " urine store").c_str(),
			crewUrineBuffer, sizeof(crewUrineBuffer)))
			crew.urineStoreId = crewUrineBuffer;
		char crewWasteBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(crewWasteBuffer, crew.organicWasteStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Crew " + row + " organic waste store").c_str(),
			crewWasteBuffer, sizeof(crewWasteBuffer)))
			crew.organicWasteStoreId = crewWasteBuffer;
		ImGui::InputDouble(("Crew " + row + " food per crew per s").c_str(),
			&crew.foodPerCrewPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " water per crew per s").c_str(),
			&crew.waterPerCrewPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " O2 per crew kg/s").c_str(),
			&crew.oxygenPerCrewKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " CO2 per crew kg/s").c_str(),
			&crew.co2PerCrewKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " moisture per crew kg/s").c_str(),
			&crew.moisturePerCrewKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " urine per crew per s").c_str(),
			&crew.urinePerCrewPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " organic waste per crew per s").c_str(),
			&crew.organicWastePerCrewPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Crew " + row + " metabolic heat per crew kW").c_str(),
			&crew.metabolicHeatPerCrewKW, 0.0, 0.0, "%.4f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove crew " + row).c_str()))
			removeCrew = true;
		ImGui::PopItemWidth();
		if (removeCrew)
			mDraft.crews.erase(mDraft.crews.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.crews.size() < core::MaxStationCrews && ImGui::Button("Add crew"))
	{
		core::CrewSettings crew;
		crew.crewCount = 4;
		crew.foodPerCrewPerS = 0.00006;
		crew.waterPerCrewPerS = 0.00008;
		crew.oxygenPerCrewKgPerS = 0.00009;
		crew.co2PerCrewKgPerS = 0.00011;
		crew.moisturePerCrewKgPerS = 0.00004;
		crew.urinePerCrewPerS = 0.00004;
		crew.organicWastePerCrewPerS = 0.00001;
		crew.metabolicHeatPerCrewKW = 0.1;
		if (!mDraft.atmospheres.empty())
			crew.atmosphereId = mDraft.atmospheres.back().id;
		if (!mDraft.stores.empty())
		{
			crew.foodStoreId = mDraft.stores.front().id;
			crew.waterStoreId = mDraft.stores.front().id;
			crew.urineStoreId = mDraft.stores.front().id;
			crew.organicWasteStoreId = mDraft.stores.front().id;
		}
		for (size_t candidate = mDraft.crews.size() + 1;; ++candidate)
		{
			crew.id = "crew" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.crews.begin(), mDraft.crews.end(),
				[&](core::CrewSettings const& existing) { return existing.id == crew.id; });
			if (!taken) break;
		}
		mDraft.crews.push_back(std::move(crew));
	}
	ImGui::SeparatorText("Draft water collectors (#402; condensate inherits the compartment's hazard class; the cold surface needs coolant circulation and must be colder than the compartment air; latent heat routes into the coolant loop once; destination must be a plain mass store)");
	for (size_t index = 0; index < mDraft.waterCollectors.size(); )
	{
		auto& collector = mDraft.waterCollectors[index];
		auto const row = std::to_string(index);
		bool removeCollector = false;
		char collectorIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(collectorIdBuffer, collector.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Collector " + row + " id").c_str(),
			collectorIdBuffer, sizeof(collectorIdBuffer)))
			collector.id = collectorIdBuffer;
		char collectorHazardBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(collectorHazardBuffer, collector.hazardClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Collector " + row + " hazard class").c_str(),
			collectorHazardBuffer, sizeof(collectorHazardBuffer)))
			collector.hazardClass = collectorHazardBuffer;
		char collectorAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(collectorAtmosphereBuffer, collector.atmosphereId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Collector " + row + " atmosphere").c_str(),
			collectorAtmosphereBuffer, sizeof(collectorAtmosphereBuffer)))
			collector.atmosphereId = collectorAtmosphereBuffer;
		char collectorLoopBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(collectorLoopBuffer, collector.coolingLoopId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Collector " + row + " cooling loop").c_str(),
			collectorLoopBuffer, sizeof(collectorLoopBuffer)))
			collector.coolingLoopId = collectorLoopBuffer;
		char collectorClassBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(collectorClassBuffer, collector.thermalClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Collector " + row + " thermal class").c_str(),
			collectorClassBuffer, sizeof(collectorClassBuffer)))
			collector.thermalClass = collectorClassBuffer;
		char collectorDestBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(collectorDestBuffer, collector.destStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Collector " + row + " raw water store").c_str(),
			collectorDestBuffer, sizeof(collectorDestBuffer)))
			collector.destStoreId = collectorDestBuffer;
		ImGui::PushItemWidth(140.0f);
		ImGui::InputDouble(("Collector " + row + " max rate kg/s").c_str(),
			&collector.maxRateKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Collector " + row + " target RH").c_str(),
			&collector.targetRelativeHumidity, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		if (ImGui::Button(("Remove collector " + row).c_str()))
			removeCollector = true;
		ImGui::PopItemWidth();
		if (removeCollector)
			mDraft.waterCollectors.erase(mDraft.waterCollectors.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.waterCollectors.size() < core::MaxStationWaterCollectors && ImGui::Button("Add water collector"))
	{
		core::WaterCollectorSettings collector;
		collector.hazardClass = "normal";
		collector.thermalClass = "moderate";
		collector.maxRateKgPerS = 0.001;
		collector.targetRelativeHumidity = 0.5;
		if (!mDraft.atmospheres.empty())
			collector.atmosphereId = mDraft.atmospheres.back().id;
		if (!mDraft.coolantLoops.empty())
			collector.coolingLoopId = mDraft.coolantLoops.back().id;
		if (!mDraft.stores.empty())
			collector.destStoreId = mDraft.stores.front().id;
		for (size_t candidate = mDraft.waterCollectors.size() + 1;; ++candidate)
		{
			collector.id = "collector" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.waterCollectors.begin(), mDraft.waterCollectors.end(),
				[&](core::WaterCollectorSettings const& existing) { return existing.id == collector.id; });
			if (!taken) break;
		}
		mDraft.waterCollectors.push_back(std::move(collector));
	}
	ImGui::SeparatorText("Draft water treatments (#402; raw water splits into treated water plus retained concentrate for declared electricity and consumables; full rated power qualifies the output, curtailed power holds it unqualified; water stores must be plain mass stores, the consumable store may be compound)");
	for (size_t index = 0; index < mDraft.waterTreatments.size(); )
	{
		auto& treatment = mDraft.waterTreatments[index];
		auto const row = std::to_string(index);
		bool removeTreatment = false;
		char treatmentIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(treatmentIdBuffer, treatment.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Treatment " + row + " id").c_str(),
			treatmentIdBuffer, sizeof(treatmentIdBuffer)))
			treatment.id = treatmentIdBuffer;
		char treatmentHazardBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(treatmentHazardBuffer, treatment.hazardClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Treatment " + row + " hazard class").c_str(),
			treatmentHazardBuffer, sizeof(treatmentHazardBuffer)))
			treatment.hazardClass = treatmentHazardBuffer;
		char treatmentSourceBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(treatmentSourceBuffer, treatment.sourceStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Treatment " + row + " raw water store").c_str(),
			treatmentSourceBuffer, sizeof(treatmentSourceBuffer)))
			treatment.sourceStoreId = treatmentSourceBuffer;
		char treatmentTreatedBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(treatmentTreatedBuffer, treatment.treatedStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Treatment " + row + " treated store").c_str(),
			treatmentTreatedBuffer, sizeof(treatmentTreatedBuffer)))
			treatment.treatedStoreId = treatmentTreatedBuffer;
		char treatmentConcentrateBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(treatmentConcentrateBuffer, treatment.concentrateStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Treatment " + row + " concentrate store").c_str(),
			treatmentConcentrateBuffer, sizeof(treatmentConcentrateBuffer)))
			treatment.concentrateStoreId = treatmentConcentrateBuffer;
		char treatmentConsumableBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(treatmentConsumableBuffer, treatment.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Treatment " + row + " consumable store").c_str(),
			treatmentConsumableBuffer, sizeof(treatmentConsumableBuffer)))
			treatment.consumableStoreId = treatmentConsumableBuffer;
		ImGui::PushItemWidth(140.0f);
		ImGui::InputDouble(("Treatment " + row + " throughput kg/s").c_str(),
			&treatment.throughputKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Treatment " + row + " recovery ratio").c_str(),
			&treatment.recoveryRatio, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(("Treatment " + row + " specific energy kWh/kg").c_str(),
			&treatment.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Treatment " + row + " consumable per kg").c_str(),
			&treatment.consumablePerKg, 0.0, 0.0, "%.6f");
		ImGui::InputInt(("Treatment " + row + " priority").c_str(), &treatment.priority, 1, 0);
		ImGui::SameLine();
		if (ImGui::Button(("Remove treatment " + row).c_str()))
			removeTreatment = true;
		ImGui::PopItemWidth();
		if (removeTreatment)
			mDraft.waterTreatments.erase(mDraft.waterTreatments.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.waterTreatments.size() < core::MaxStationWaterTreatments && ImGui::Button("Add water treatment"))
	{
		core::WaterTreatmentSettings treatment;
		treatment.hazardClass = "normal";
		treatment.throughputKgPerS = 0.002;
		treatment.recoveryRatio = 0.85;
		treatment.specificEnergyKWhPerKg = 0.5;
		treatment.consumablePerKg = 0.01;
		if (!mDraft.stores.empty())
		{
			treatment.sourceStoreId = mDraft.stores.front().id;
			treatment.treatedStoreId = mDraft.stores.front().id;
			treatment.concentrateStoreId = mDraft.stores.front().id;
			treatment.consumableStoreId = mDraft.stores.front().id;
		}
		for (size_t candidate = mDraft.waterTreatments.size() + 1;; ++candidate)
		{
			treatment.id = "treatment" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.waterTreatments.begin(), mDraft.waterTreatments.end(),
				[&](core::WaterTreatmentSettings const& existing) { return existing.id == treatment.id; });
			if (!taken) break;
		}
		mDraft.waterTreatments.push_back(std::move(treatment));
	}
	ImGui::SeparatorText("Draft electrolyzers (#404; qualified water splits into oxygen plus hydrogen at the declared stoichiometry; the electrical dose must cover the chemical energy the hydrogen retains, the difference is process heat into the class-matched sink; oxygen goes to a gas store OR a compartment, never both; product stores cap production instead of venting)");
	for (size_t index = 0; index < mDraft.electrolyzers.size(); )
	{
		auto& cell = mDraft.electrolyzers[index];
		auto const row = std::to_string(index);
		bool removeCell = false;
		char cellIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellIdBuffer, cell.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " id").c_str(),
			cellIdBuffer, sizeof(cellIdBuffer)))
			cell.id = cellIdBuffer;
		char cellHazardBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellHazardBuffer, cell.hazardClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " hazard class").c_str(),
			cellHazardBuffer, sizeof(cellHazardBuffer)))
			cell.hazardClass = cellHazardBuffer;
		char cellWaterBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellWaterBuffer, cell.waterStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " feed water store").c_str(),
			cellWaterBuffer, sizeof(cellWaterBuffer)))
			cell.waterStoreId = cellWaterBuffer;
		char cellOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellOxygenBuffer, cell.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " oxygen store (or leave empty for the compartment)").c_str(),
			cellOxygenBuffer, sizeof(cellOxygenBuffer)))
			cell.oxygenStoreId = cellOxygenBuffer;
		char cellHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellHydrogenBuffer, cell.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " hydrogen store").c_str(),
			cellHydrogenBuffer, sizeof(cellHydrogenBuffer)))
			cell.hydrogenStoreId = cellHydrogenBuffer;
		char cellAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellAtmosphereBuffer, cell.atmosphereId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " delivery compartment (or leave empty for the store)").c_str(),
			cellAtmosphereBuffer, sizeof(cellAtmosphereBuffer)))
			cell.atmosphereId = cellAtmosphereBuffer;
		char cellThermalBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellThermalBuffer, cell.thermalClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " heat sink thermal class").c_str(),
			cellThermalBuffer, sizeof(cellThermalBuffer)))
			cell.thermalClass = cellThermalBuffer;
		char cellSinkBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellSinkBuffer, cell.heatSinkId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Electrolyzer " + row + " heat sink reservoir").c_str(),
			cellSinkBuffer, sizeof(cellSinkBuffer)))
			cell.heatSinkId = cellSinkBuffer;
		ImGui::PushItemWidth(140.0f);
		ImGui::InputDouble(("Electrolyzer " + row + " rated split kg/s").c_str(),
			&cell.rateKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("Electrolyzer " + row + " specific energy kWh/kg water").c_str(),
			&cell.specificEnergyKWhPerKgWater, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("Electrolyzer " + row + " max sink temperature C").c_str(),
			&cell.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
		ImGui::InputInt(("Electrolyzer " + row + " priority").c_str(), &cell.priority, 1, 0);
		ImGui::SameLine();
		if (ImGui::Button(("Remove electrolyzer " + row).c_str()))
			removeCell = true;
		ImGui::PopItemWidth();
		if (removeCell)
			mDraft.electrolyzers.erase(mDraft.electrolyzers.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.electrolyzers.size() < core::MaxStationElectrolyzers && ImGui::Button("Add electrolyzer"))
	{
		core::ElectrolyzerSettings cell;
		cell.hazardClass = "normal";
		cell.thermalClass = "low";
		cell.rateKgPerS = 0.001;
		cell.specificEnergyKWhPerKgWater = 4.5;
		cell.maxSinkTemperatureC = 60.0;
		if (!mDraft.stores.empty())
		{
			cell.waterStoreId = mDraft.stores.front().id;
			cell.oxygenStoreId = mDraft.stores.front().id;
			cell.hydrogenStoreId = mDraft.stores.front().id;
		}
		if (!mDraft.thermalReservoirs.empty())
			cell.heatSinkId = mDraft.thermalReservoirs.front().id;
		for (size_t candidate = mDraft.electrolyzers.size() + 1;; ++candidate)
		{
			cell.id = "electrolyzer" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.electrolyzers.begin(), mDraft.electrolyzers.end(),
				[&](core::ElectrolyzerSettings const& existing) { return existing.id == cell.id; });
			if (!taken) break;
		}
		mDraft.electrolyzers.push_back(std::move(cell));
	}
	ImGui::SeparatorText("Draft CO2 capture machines (#404; a compartment's carbon dioxide is scrubbed into a finite capture store as inventory; capture is finite on rate, available CO2, store headroom and consumable; the unmet atmospheric service stays visible; a tick with no power scrubs nothing)");
	for (size_t index = 0; index < mDraft.co2Captures.size(); )
	{
		auto& capture = mDraft.co2Captures[index];
		auto const row = std::to_string(index);
		bool removeCapture = false;
		char captureIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(captureIdBuffer, capture.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("CO2 capture " + row + " id").c_str(),
			captureIdBuffer, sizeof(captureIdBuffer)))
			capture.id = captureIdBuffer;
		char captureHazardBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(captureHazardBuffer, capture.hazardClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("CO2 capture " + row + " hazard class").c_str(),
			captureHazardBuffer, sizeof(captureHazardBuffer)))
			capture.hazardClass = captureHazardBuffer;
		char captureAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(captureAtmosphereBuffer, capture.atmosphereId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("CO2 capture " + row + " compartment").c_str(),
			captureAtmosphereBuffer, sizeof(captureAtmosphereBuffer)))
			capture.atmosphereId = captureAtmosphereBuffer;
		char captureStoreBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(captureStoreBuffer, capture.captureStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("CO2 capture " + row + " capture store").c_str(),
			captureStoreBuffer, sizeof(captureStoreBuffer)))
			capture.captureStoreId = captureStoreBuffer;
		char captureConsumableBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(captureConsumableBuffer, capture.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("CO2 capture " + row + " consumable store (empty: none)").c_str(),
			captureConsumableBuffer, sizeof(captureConsumableBuffer)))
			capture.consumableStoreId = captureConsumableBuffer;
		ImGui::PushItemWidth(140.0f);
		ImGui::InputDouble(("CO2 capture " + row + " max rate kg/s").c_str(),
			&capture.maxRateKgPerS, 0.0, 0.0, "%.6f");
		ImGui::InputDouble(("CO2 capture " + row + " specific energy kWh/kg").c_str(),
			&capture.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(("CO2 capture " + row + " consumable per kg").c_str(),
			&capture.consumablePerKg, 0.0, 0.0, "%.6f");
		ImGui::InputInt(("CO2 capture " + row + " priority").c_str(), &capture.priority, 1, 0);
		ImGui::SameLine();
		if (ImGui::Button(("Remove CO2 capture " + row).c_str()))
			removeCapture = true;
		ImGui::PopItemWidth();
		if (removeCapture)
			mDraft.co2Captures.erase(mDraft.co2Captures.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	if (mDraft.co2Captures.size() < core::MaxStationCo2Captures && ImGui::Button("Add CO2 capture machine"))
	{
		core::Co2CaptureSettings capture;
		capture.hazardClass = "normal";
		capture.maxRateKgPerS = 0.001;
		capture.specificEnergyKWhPerKg = 0.5;
		capture.consumablePerKg = 0.0;
		if (!mDraft.stores.empty())
		{
			capture.captureStoreId = mDraft.stores.front().id;
			capture.consumableStoreId = mDraft.stores.front().id;
		}
		if (!mDraft.atmospheres.empty())
			capture.atmosphereId = mDraft.atmospheres.front().id;
		for (size_t candidate = mDraft.co2Captures.size() + 1;; ++candidate)
		{
			capture.id = "co2capture" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.co2Captures.begin(), mDraft.co2Captures.end(),
				[&](core::Co2CaptureSettings const& existing) { return existing.id == capture.id; });
			if (!taken) break;
		}
		mDraft.co2Captures.push_back(std::move(capture));
	}
	ImGui::SeparatorText("Draft fuel-cell backup units (#406; dispatchable power from two dedicated, finite reactant stores; output is bounded by the installed rating, the setpoint, the reactants drawable above their protected reserves and the product water store's headroom; waste heat needs a class-matched qualified sink; product water lands in the treatment destination, never drinking water directly)");
	for (size_t index = 0; index < mDraft.fuelCells.size(); )
	{
		auto& cell = mDraft.fuelCells[index];
		auto const row = std::to_string(index);
		bool removeCell = false;
		char cellIdBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellIdBuffer, cell.id.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " id").c_str(),
			cellIdBuffer, sizeof(cellIdBuffer)))
			cell.id = cellIdBuffer;
		char cellHazardBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellHazardBuffer, cell.hazardClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " hazard class").c_str(),
			cellHazardBuffer, sizeof(cellHazardBuffer)))
			cell.hazardClass = cellHazardBuffer;
		char cellHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellHydrogenBuffer, cell.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " hydrogen reactant store").c_str(),
			cellHydrogenBuffer, sizeof(cellHydrogenBuffer)))
			cell.hydrogenStoreId = cellHydrogenBuffer;
		char cellOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellOxygenBuffer, cell.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " oxygen reactant store").c_str(),
			cellOxygenBuffer, sizeof(cellOxygenBuffer)))
			cell.oxygenStoreId = cellOxygenBuffer;
		char cellWaterBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellWaterBuffer, cell.waterStoreId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " product water store").c_str(),
			cellWaterBuffer, sizeof(cellWaterBuffer)))
			cell.waterStoreId = cellWaterBuffer;
		char cellThermalBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellThermalBuffer, cell.thermalClass.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " heat sink thermal class").c_str(),
			cellThermalBuffer, sizeof(cellThermalBuffer)))
			cell.thermalClass = cellThermalBuffer;
		char cellSinkBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(cellSinkBuffer, cell.heatSinkId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Fuel cell " + row + " heat sink reservoir").c_str(),
			cellSinkBuffer, sizeof(cellSinkBuffer)))
			cell.heatSinkId = cellSinkBuffer;
		ImGui::InputDouble(("Fuel cell " + row + " rated output kW").c_str(),
			&cell.ratedOutputKW, 0.0, 10.0, "%.3f");
		ImGui::InputDouble(("Fuel cell " + row + " efficiency (0..1]").c_str(),
			&cell.efficiency, 0.0, 0.05, "%.3f");
		ImGui::InputDouble(("Fuel cell " + row + " commanded output kW").c_str(),
			&cell.commandedOutputKW, 0.0, 1.0, "%.3f");
		ImGui::InputDouble(("Fuel cell " + row + " max sink temperature C").c_str(),
			&cell.maxSinkTemperatureC, 0.0, 5.0, "%.1f");
		if (ImGui::Button(("Remove fuel cell " + row).c_str()))
			removeCell = true;
		if (removeCell)
		{
			mDraft.fuelCells.erase(mDraft.fuelCells.begin() + static_cast<std::ptrdiff_t>(index));
			continue;
		}
		++index;
	}
	if (mDraft.fuelCells.size() < core::MaxStationFuelCells && ImGui::Button("Add fuel cell"))
	{
		core::FuelCellSettings cell;
		cell.hazardClass = "normal";
		cell.thermalClass = "low";
		cell.ratedOutputKW = 10.0;
		cell.efficiency = 0.6;
		cell.maxSinkTemperatureC = 80.0;
		if (!mDraft.stores.empty())
		{
			cell.hydrogenStoreId = mDraft.stores.front().id;
			cell.oxygenStoreId = mDraft.stores.front().id;
			cell.waterStoreId = mDraft.stores.front().id;
		}
		if (!mDraft.thermalReservoirs.empty())
			cell.heatSinkId = mDraft.thermalReservoirs.front().id;
		for (size_t candidate = mDraft.fuelCells.size() + 1;; ++candidate)
		{
			cell.id = "fuelcell" + std::to_string(candidate);
			auto const taken = std::any_of(mDraft.fuelCells.begin(), mDraft.fuelCells.end(),
				[&](core::FuelCellSettings const& existing) { return existing.id == cell.id; });
			if (!taken) break;
		}
		mDraft.fuelCells.push_back(std::move(cell));
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
	ImGui::SeparatorText("Boundary balances (current + lost - delivered - collected must stay within tolerance)");
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
	ImGui::SeparatorText("Atmospheres (live; ideal-gas absolute and partial pressures, humidity capped by saturation, occupancy draw capped by inventory, leakage and airlock losses booked by mass; limits reported, never clamped)");
	if (snapshot.atmospheres.empty()) ImGui::TextDisabled("No atmosphere compartments authored.");
	for (auto const& atmosphere : snapshot.atmospheres)
	{
		char const* status = "idle";
		switch (atmosphere.status)
		{
		case core::StationTransferStatus::Full: status = "life support met"; break;
		case core::StationTransferStatus::Partial: status = "O2 limited"; break;
		case core::StationTransferStatus::Rejected: status = "O2 unavailable"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] -> %s: %s | pressure %.3f kPa (start %.3f) | O2 %.3f, N2 %.3f, CO2 %.3f, H2O %.3f kg (start %.3f/%.3f/%.3f/%.3f) | temp %.3f C (start %.3f C)",
			atmosphere.id.c_str(), atmosphere.hazardClass.c_str(),
			atmosphere.thermallyCoupled ? atmosphere.reservoirId.c_str() : "adiabatic",
			status, atmosphere.pressureKPa, atmosphere.startingPressureKPa,
			atmosphere.oxygenKg, atmosphere.nitrogenKg, atmosphere.carbonDioxideKg,
			atmosphere.waterVapourKg, atmosphere.startingOxygenKg,
			atmosphere.startingNitrogenKg, atmosphere.startingCarbonDioxideKg,
			atmosphere.startingWaterVapourKg, atmosphere.temperatureC,
			atmosphere.startingTemperatureC);
		ImGui::Text("  partial pressures (kPa): O2 %.3f, N2 %.3f, CO2 %.3f, H2O %.3f | saturation %.3f kPa, RH %.1f%% | volume %.3f m3, capacity %.3f kJ/K",
			atmosphere.oxygenPartialPressureKPa, atmosphere.nitrogenPartialPressureKPa,
			atmosphere.carbonDioxidePartialPressureKPa, atmosphere.waterVapourPartialPressureKPa,
			atmosphere.saturationVapourPressureKPa, atmosphere.relativeHumidity * 100.0,
			atmosphere.volumeM3, atmosphere.thermalCapacityKJPerK);
		ImGui::Text("  last tick: O2 consumed %.6f of %.6f kg (unmet %.6f) | CO2 produced %.6f kg | vapour added %.6f, condensed %.6f kg | leaked %.6f kg | airlock removed %.6f (recovered %.6f, lost %.6f) kg",
			atmosphere.lastOxygenConsumedKg, atmosphere.lastOxygenDemandKg,
			atmosphere.lastOxygenUnmetKg, atmosphere.lastCo2ProducedKg,
			atmosphere.lastVapourAddedKg, atmosphere.lastVapourCondensedKg,
			atmosphere.lastLeakedKg, atmosphere.lastCycleRemovedKg,
			atmosphere.lastCycleRecoveredKg, atmosphere.lastCycleLostKg);
		ImGui::Text("  heat: occupants %.4f kW, latent %.4f kW, reservoir exchange %+.4f kW | ledger (kJ): starting %.3f, current %.3f, occupant %.3f, latent %.3f, exchanged %.3f, residual %.9f (tolerance %.1e)",
			atmosphere.lastOccupantHeatKW, atmosphere.lastLatentHeatKW,
			atmosphere.lastExchangeIntoCompartmentKW, atmosphere.startingEnergyKJ,
			atmosphere.currentEnergyKJ, atmosphere.occupantHeatEnergyKJ,
			atmosphere.latentHeatEnergyKJ, atmosphere.exchangedEnergyKJ,
			atmosphere.residualKJ, snapshot.balanceTolerance);
		ImGui::Text("  cumulative (kg): consumed %.4f, CO2 %.4f, vapour added %.4f, condensed %.4f, leaked %.4f, recovered %.4f, lost %.4f",
			atmosphere.oxygenConsumedKg, atmosphere.co2ProducedKg,
			atmosphere.vapourAddedKg, atmosphere.condensedKg,
			atmosphere.leakedKg, atmosphere.recoveredKg, atmosphere.lostKg);
		if (atmosphere.abovePressureLimit)
			ImGui::Text("  ABOVE the declared maximum pressure limit: %.3f kPa", atmosphere.maxPressureKPa);
		if (atmosphere.belowPressureLimit)
			ImGui::Text("  BELOW the declared minimum pressure limit: %.3f kPa", atmosphere.minPressureKPa);
		if (atmosphere.lowOxygen)
			ImGui::Text("  LOW OXYGEN: partial pressure below the life-support minimum %.3f kPa",
				atmosphere.minOxygenPartialPressureKPa);
		if (atmosphere.highCarbonDioxide)
			ImGui::Text("  HIGH CO2: partial pressure above the air-quality maximum %.3f kPa",
				atmosphere.maxCarbonDioxidePartialPressureKPa);
		if (atmosphere.condensing)
			ImGui::Text("  CONDENSING: vapour at saturation, %.6f kg condensed this tick releasing latent heat",
				atmosphere.lastVapourCondensedKg);
	}
	ImGui::SeparatorText("Airlock cycles (live; scheduled removals with the recovered-versus-lost split; refusals still count as occurrences)");
	if (snapshot.airlockCycles.empty()) ImGui::TextDisabled("No scheduled airlock cycles authored.");
	for (auto const& cycle : snapshot.airlockCycles)
	{
		char const* status = "idle";
		switch (cycle.status)
		{
		case core::StationTransferStatus::Full: status = "cycled"; break;
		case core::StationTransferStatus::Partial: status = "partially-cycled"; break;
		case core::StationTransferStatus::Rejected: status = "refused (no gas)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s -> %s: %s | last at %.3f s: removed %.4f kg (recovered %.4f, lost %.4f) | total removed %.4f (recovered %.4f, lost %.4f) | %llu occurrence(s) | priority %d",
			cycle.id.c_str(), cycle.atmosphereId.c_str(), status,
			cycle.lastOccurrenceSeconds, cycle.lastRemovedKg,
			cycle.lastRecoveredKg, cycle.lastLostKg,
			cycle.removedTotal, cycle.recoveredTotal, cycle.lostTotal,
			static_cast<unsigned long long>(cycle.occurrencesCompleted), cycle.priority);
		if (cycle.exhausted)
			ImGui::Text("  one-off schedule spent: no further cycles");
		else
			ImGui::Text("  next cycle at %.3f s (%s)", cycle.nextOccurrenceSeconds,
				cycle.recurrenceSeconds > 0.0
					? ("recurring every " + std::to_string(cycle.recurrenceSeconds) + " s").c_str()
					: "one-off");
	}
	ImGui::SeparatorText("Crews (live; one uniform metabolic scale per tick - every pathway throttles together with the scarcest supply or the tightest waste headroom; requested vs actual is explicit, never nominal waste from absent material)");
	if (snapshot.crews.empty()) ImGui::TextDisabled("No crews authored.");
	for (auto const& crew : snapshot.crews)
	{
		char const* status = "idle";
		switch (crew.status)
		{
		case core::StationTransferStatus::Full: status = "fully supplied"; break;
		case core::StationTransferStatus::Partial: status = "partially supplied"; break;
		case core::StationTransferStatus::Rejected: status = "starved (nothing supplied)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s (%d crew in '%s'): %s | activity %.3f, metabolic scale %.3f%s%s",
			crew.id.c_str(), crew.crewCount, crew.atmosphereId.c_str(), status,
			crew.activityLevel, crew.metabolicScale,
			crew.limiter.empty() ? "" : " | limited by: ",
			crew.limiter.empty() ? "" : crew.limiter.c_str());
		ImGui::Text("  last tick: food %.6f/%.6f, water %.6f/%.6f, O2 %.6f/%.6f kg | CO2 %.6f kg, moisture %.6f kg | urine %.6f/%.6f, organic waste %.6f/%.6f | heat %.4f kW (actual/requested)",
			crew.lastFoodActual, crew.lastFoodRequested,
			crew.lastWaterActual, crew.lastWaterRequested,
			crew.lastOxygenActualKg, crew.lastOxygenRequestedKg,
			crew.lastCo2ProducedKg, crew.lastMoistureProducedKg,
			crew.lastUrineActual, crew.lastUrineRequested,
			crew.lastOrganicWasteActual, crew.lastOrganicWasteRequested,
			crew.lastHeatKW);
		ImGui::Text("  cumulative: consumed food %.4f, water %.4f, O2 %.4f kg | returned CO2 %.4f, moisture %.4f, urine %.4f, organic waste %.4f kg | heat %.2f kJ | retained body mass %.4f kg",
			crew.foodConsumed, crew.waterConsumed, crew.oxygenConsumedKg,
			crew.co2ProducedKg, crew.moistureProducedKg,
			crew.urineProduced, crew.organicWasteProduced,
			crew.heatKJ, crew.retainedKg);
		if (crew.disabled)
			ImGui::Text("  zero crew count: no demand, no returns");
	}
	ImGui::SeparatorText("Water collectors (live; condensate needs circulating coolant colder than the compartment air, stops at the humidity target, and its latent heat enters the coolant loop once)");
	for (auto const& collector : snapshot.waterCollectors)
	{
		char const* status = "idle";
		switch (collector.status)
		{
		case core::StationTransferStatus::Full: status = "collecting at the configured rate"; break;
		case core::StationTransferStatus::Partial: status = "collecting below the configured rate"; break;
		case core::StationTransferStatus::Rejected: status = "blocked (nothing collected)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] (%s -> %s via loop '%s', class %s): %s | cold surface %s, circulation %.3f",
			collector.id.c_str(), collector.hazardClass.c_str(), collector.atmosphereId.c_str(),
			collector.destStoreId.c_str(), collector.coolingLoopId.c_str(),
			collector.thermalClass.c_str(), status,
			collector.coldSurfaceAvailable ? "available" : "unavailable",
			collector.lastCirculationFraction);
		ImGui::Text("  last tick: requested %.6f kg, condensable %.6f kg, collected %.6f kg | latent heat to loop %.4f kW",
			collector.lastRequestedKg, collector.lastCondensableKg,
			collector.lastCollectedKg, collector.lastLatentHeatKW);
		if (!collector.blockReason.empty())
			ImGui::Text("  blocked: %s", collector.blockReason.c_str());
		ImGui::Text("  cumulative: collected %.4f kg | latent heat routed %.2f kJ",
			collector.collectedKg, collector.latentHeatEnergyKJ);
		if (collector.disabled)
			ImGui::Text("  zero maximum rate: no collection");
	}
	ImGui::SeparatorText("Water treatments (live; one uniform processing scale per tick; full rated power qualifies the output, curtailed power holds it unqualified; concentrate is retained pending disposal)");
	for (auto const& treatment : snapshot.waterTreatments)
	{
		char const* status = "idle";
		switch (treatment.status)
		{
		case core::StationTransferStatus::Full: status = "running at full rated power"; break;
		case core::StationTransferStatus::Partial: status = "running on curtailed power (output held)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing processed)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] (%s -> %s + %s, consumables from %s): %s | scale %.3f%s%s",
			treatment.id.c_str(), treatment.hazardClass.c_str(), treatment.sourceStoreId.c_str(),
			treatment.treatedStoreId.c_str(), treatment.concentrateStoreId.c_str(),
			treatment.consumableStoreId.c_str(), status, treatment.scale,
			treatment.limiter.empty() ? "" : " | limited by: ",
			treatment.limiter.empty() ? "" : treatment.limiter.c_str());
		ImGui::Text("  last tick: processed %.6f kg -> treated %.6f kg + concentrate %.6f kg | power %.4f/%.4f kW | consumable %.6f/%.6f",
			treatment.lastProcessedKg, treatment.lastTreatedKg, treatment.lastConcentrateKg,
			treatment.lastPowerSuppliedKW, treatment.lastPowerDemandKW,
			treatment.lastConsumableUsed, treatment.lastConsumableRequested);
		ImGui::Text("  quality: %s%s",
			treatment.qualityAccepted ? "treated output accepted" : "treated output HELD UNQUALIFIED",
			treatment.holdReason.empty() ? "" : (std::string(" - ") + treatment.holdReason).c_str());
		ImGui::Text("  cumulative: processed %.4f kg | treated %.4f kg (accepted %.4f, held %.4f) | concentrate retained %.4f kg | energy %.2f/%.2f kJ | consumables %.4f",
			treatment.processedKg, treatment.treatedKg, treatment.acceptedKg, treatment.heldKg,
			treatment.concentrateKg, treatment.energySuppliedKJ, treatment.energyOwedKJ,
			treatment.consumablesUsed);
		if (treatment.disabled)
			ImGui::Text("  zero throughput: no processing");
	}
	ImGui::SeparatorText("Electrolyzers (live; the split closes on mass - water = oxygen + hydrogen - and on energy - supplied = chemical + heat; a full product store caps production, a hot sink refuses the cell)");
	for (auto const& cell : snapshot.electrolyzers)
	{
		char const* status = "idle";
		switch (cell.status)
		{
		case core::StationTransferStatus::Full: status = "splitting at full rate"; break;
		case core::StationTransferStatus::Partial: status = "splitting below rated (production short)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing split)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] (%s -> %s + %s%s): %s | scale %.3f%s%s",
			cell.id.c_str(), cell.hazardClass.c_str(), cell.waterStoreId.c_str(),
			cell.oxygenStoreId.empty() ? cell.atmosphereId.c_str() : cell.oxygenStoreId.c_str(),
			cell.hydrogenStoreId.c_str(),
			cell.oxygenStoreId.empty() ? " (O2 to compartment)" : "",
			status, cell.scale,
			cell.limiter.empty() ? "" : " | limited by: ",
			cell.limiter.empty() ? "" : cell.limiter.c_str());
		ImGui::Text("  last tick: split %.6f kg -> O2 %.6f kg + H2 %.6f kg | power %.4f/%.4f kW | unmet production %.6f kg | cooling %s%s",
			cell.lastSplitKg, cell.lastOxygenKg, cell.lastHydrogenKg,
			cell.lastPowerSuppliedKW, cell.lastPowerDemandKW, cell.lastUnmetProductionKg,
			cell.coolingAvailable ? "qualified" : "NOT qualified",
			cell.blockReason.empty() ? "" : (std::string(" - ") + cell.blockReason).c_str());
		ImGui::Text("  cumulative: split %.4f kg | O2 %.4f kg | H2 %.4f kg | energy %.2f kJ (chemical %.2f + heat %.2f) | residual mass %.2e kg / energy %.2e kJ",
			cell.waterSplitKg, cell.oxygenProducedKg, cell.hydrogenProducedKg,
			cell.energySuppliedKJ, cell.chemicalEnergyKJ, cell.heatRoutedKJ,
			cell.residualKg, cell.residualKJ);
		if (cell.disabled)
			ImGui::Text("  zero rated split: no production");
	}
	ImGui::SeparatorText("CO2 capture (live; captured gas is inventory in the capture store, never vanished carbon; the unmet atmospheric service is the explicit remainder)");
	for (auto const& capture : snapshot.co2Captures)
	{
		char const* status = "idle";
		switch (capture.status)
		{
		case core::StationTransferStatus::Full: status = "capturing at full rate"; break;
		case core::StationTransferStatus::Partial: status = "capturing below rated (service unmet)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing captured)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] (%s -> %s%s): %s | scale %.3f%s%s",
			capture.id.c_str(), capture.hazardClass.c_str(), capture.atmosphereId.c_str(),
			capture.captureStoreId.c_str(),
			capture.consumableStoreId.empty() ? "" : (std::string(", consumables from ") + capture.consumableStoreId).c_str(),
			status, capture.scale,
			capture.limiter.empty() ? "" : " | limited by: ",
			capture.limiter.empty() ? "" : capture.limiter.c_str());
		ImGui::Text("  last tick: captured %.6f kg | unmet service %.6f kg | power %.4f/%.4f kW | consumable %.6f/%.6f%s",
			capture.lastCapturedKg, capture.lastUnmetServiceKg,
			capture.lastPowerSuppliedKW, capture.lastPowerDemandKW,
			capture.lastConsumableUsed, capture.lastConsumableRequested,
			capture.blockReason.empty() ? "" : (std::string(" - ") + capture.blockReason).c_str());
		ImGui::Text("  cumulative: captured %.4f kg | energy %.2f kJ | consumables %.4f",
			capture.capturedKg, capture.energySuppliedKJ, capture.consumablesUsed);
		if (capture.disabled)
			ImGui::Text("  zero capture capacity: no removal");
	}
	ImGui::SeparatorText("Fuel-cell backup (live; the reaction closes on mass - hydrogen + oxygen = water - and on energy - chemical = electrical + waste; output is capped by the rating, the drawable reactants and the water destination headroom; the endurance clock counts only reactants usable above the protected reserves)");
	for (auto const& cell : snapshot.fuelCells)
	{
		char const* status = "idle";
		switch (cell.status)
		{
		case core::StationTransferStatus::Full: status = "delivering at commanded output"; break;
		case core::StationTransferStatus::Partial: status = "delivering below command (reactants or headroom limit)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing delivered)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s] (%s + %s -> %s): %s | commanded %.3f kW%s%s",
			cell.id.c_str(), cell.hazardClass.c_str(),
			cell.hydrogenStoreId.c_str(), cell.oxygenStoreId.c_str(),
			cell.waterStoreId.c_str(), status, cell.commandedOutputKW,
			cell.limiter.empty() ? "" : " | limited by: ",
			cell.limiter.empty() ? "" : cell.limiter.c_str());
		ImGui::Text("  last tick: output %.4f kW | H2 %.6f kg + O2 %.6f kg -> water %.6f kg | waste heat %.4f kW | unmet %.4f kW | cooling %s%s",
			cell.lastOutputKW, cell.lastHydrogenKg, cell.lastOxygenKg, cell.lastWaterKg,
			cell.lastWasteHeatKW, cell.lastUnmetOutputKW,
			cell.coolingAvailable ? "qualified" : "NOT qualified",
			cell.blockReason.empty() ? "" : (std::string(" - ") + cell.blockReason).c_str());
		ImGui::Text("  cumulative: H2 %.4f kg | O2 %.4f kg | water %.4f kg | energy %.2f kJ (electrical %.2f + waste %.2f) | residual mass %.2e kg / energy %.2e kJ | endurance %.1f s",
			cell.hydrogenConsumedKg, cell.oxygenConsumedKg, cell.waterProducedKg,
			cell.chemicalEnergyKJ, cell.electricalKJ, cell.wasteHeatKJ,
			cell.residualKg, cell.residualKJ, cell.enduranceSeconds);
		if (cell.disabled)
			ImGui::Text("  zero rated output: no backup service");
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
