#include "StationSystemsEntryEditor.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>

namespace station_editor
{
namespace
{
// Identity changes must use Flow's preflight/confirmation/atomic cascade.
// Shared by the inspector and forms so neither can write a dangling ID.
void renderWaterQuality(core::WaterQualitySettings& gate, std::string const& label)
{
	ImGui::PushID(label.c_str());
	if (ImGui::TreeNode("Water quality tolerances"))
	{
		ImGui::TextUnformatted("Concentrations in kg constituent per kg stream");
		bool total = gate.maxTotalDissolved.has_value();
		if (ImGui::Checkbox("Gate total dissolved (default: no gate)", &total))
		{ if (total) gate.maxTotalDissolved = 0.0; else gate.maxTotalDissolved.reset(); }
		if (total) ImGui::InputDouble("Maximum total dissolved", &*gate.maxTotalDissolved, 0, 0, "%.6f");
		for (auto const* name : core::StationWaterDryConstituents)
		{
			auto found = gate.maxConcentrations.find(name);
			bool enabled = found != gate.maxConcentrations.end();
			if (ImGui::Checkbox((std::string("Gate ") + name).c_str(), &enabled))
			{ if (enabled) gate.maxConcentrations[name] = 0.0; else gate.maxConcentrations.erase(name); }
			if (enabled) ImGui::InputDouble(name, &gate.maxConcentrations.at(name), 0, 0, "%.6f");
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}
void renderIdentity(std::string const& label, std::string const& id)
{
	char buffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(buffer, id.c_str(), sizeof(buffer) - 1);
	ImGui::InputText((label + " id").c_str(), buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("To rename, select this node in Flow and use 'New node name' / 'Rename selected node'.");
}
}

bool renderConversionEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::ResourceConversionSettings& conversion,
	std::string const& label, std::string const& removeLabel)
{
	bool removeConversion = false;
	auto const row = label.substr(label.find_last_of(' ') + 1);
	ImGui::PushItemWidth(220);
	renderIdentity(label, conversion.id);
	ImGui::InputInt((label + " priority").c_str(), &conversion.priority);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeConversion = true;
	for (size_t flow = 0; flow < conversion.inputs.size(); )
	{
		auto& input = conversion.inputs[flow];
		auto const flowRow = label + " input " + std::to_string(flow);
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
		auto const flowRow = label + " output " + std::to_string(flow);
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
	return removeConversion;
}

bool renderSolarEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::SolarArraySettings& array,
	std::string const& label, std::string const& removeLabel)
{
	bool removeArray = false;
	auto const row = label.substr(label.find_last_of(' ') + 1);
	ImGui::PushItemWidth(220);
	renderIdentity(label, array.id);
	ImGui::InputDouble((label + " capacity kW").c_str(), &array.capacityKW, 0.0, 0.0, "%.3f");
	bool solarPeriodic = array.illumination.periodSeconds > 0.0;
	if (ImGui::Checkbox((label + " periodic illumination").c_str(), &solarPeriodic))
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
		ImGui::InputDouble((label + " constant illumination").c_str(),
			&array.illumination.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble((label + " period seconds").c_str(),
			&array.illumination.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < array.illumination.segments.size(); )
		{
			auto& stretch = array.illumination.segments[segment];
			auto const segmentRow = label + " segment " + std::to_string(segment);
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
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeArray = true;
	ImGui::PopItemWidth();
	return removeArray;
}

// The draft is carried into the per-entry editors as the shared seam's
// context: the entry is always one of the draft's own elements, so a
// surface that only holds the draft can edit any entry without owning
// the list around it. Today's fields never read across the draft; the
// parameter is the seam the fan-out tickets (#647-#651) extend.
bool renderCrewEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::CrewSettings& crew,
	std::string const& label, std::string const& removeLabel)
{
	bool removeCrew = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, crew.id);
	renderWaterQuality(crew.waterQuality, label);
	char greywaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(greywaterBuffer, crew.greywaterStoreId.c_str(), sizeof(greywaterBuffer) - 1);
	if (ImGui::InputText((label + " greywater store").c_str(), greywaterBuffer, sizeof(greywaterBuffer)))
		crew.greywaterStoreId = greywaterBuffer;
	ImGui::InputDouble((label + " greywater kg per crew-day").c_str(), &crew.greywaterKgPerCrewDay, 0, 0, "%.6f");
	for (auto const* name : {"dryOrganics", "nutrientNitrogen"})
	{
		bool enabled = crew.greywaterLoadKgPerCrewDay.contains(name);
		if (ImGui::Checkbox((label + " greywater load " + name).c_str(), &enabled))
		{ if (enabled) crew.greywaterLoadKgPerCrewDay[name] = 0; else crew.greywaterLoadKgPerCrewDay.erase(name); }
		if (enabled) ImGui::InputDouble((label + " " + name + " kg per crew-day").c_str(),
			&crew.greywaterLoadKgPerCrewDay.at(name), 0, 0, "%.6f");
	}
	char crewAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewAtmosphereBuffer, crew.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " atmosphere").c_str(),
		crewAtmosphereBuffer, sizeof(crewAtmosphereBuffer)))
		crew.atmosphereId = crewAtmosphereBuffer;
	ImGui::InputInt((label + " count").c_str(), &crew.crewCount, 1, 0);
	bool crewPeriodic = crew.activityProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox((label + " periodic activity").c_str(), &crewPeriodic))
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
		ImGui::InputDouble((label + " constant activity level").c_str(),
			&crew.activityProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble((label + " activity period seconds").c_str(),
			&crew.activityProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < crew.activityProfile.segments.size(); ++segment)
		{
			auto& stretch = crew.activityProfile.segments[segment];
			auto const segmentRow = label + " activity segment " + std::to_string(segment);
			ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(("Remove " + segmentRow).c_str()))
				crew.activityProfile.segments.erase(
					crew.activityProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
			}
		if (crew.activityProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add activity segment to " + label).c_str()))
			crew.activityProfile.segments.push_back(
				{crew.activityProfile.periodSeconds * 0.5, 0.0});
	}
	char crewFoodBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewFoodBuffer, crew.foodStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " food store").c_str(),
		crewFoodBuffer, sizeof(crewFoodBuffer)))
		crew.foodStoreId = crewFoodBuffer;
	char crewWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewWaterBuffer, crew.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " water store").c_str(),
		crewWaterBuffer, sizeof(crewWaterBuffer)))
		crew.waterStoreId = crewWaterBuffer;
	char crewUrineBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewUrineBuffer, crew.urineStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " urine store").c_str(),
		crewUrineBuffer, sizeof(crewUrineBuffer)))
		crew.urineStoreId = crewUrineBuffer;
	char crewWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewWasteBuffer, crew.organicWasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " organic waste store").c_str(),
		crewWasteBuffer, sizeof(crewWasteBuffer)))
		crew.organicWasteStoreId = crewWasteBuffer;
	ImGui::InputDouble((label + " food per crew per s").c_str(),
		&crew.foodPerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " water per crew per s").c_str(),
		&crew.waterPerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " O2 per crew kg/s").c_str(),
		&crew.oxygenPerCrewKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " CO2 per crew kg/s").c_str(),
		&crew.co2PerCrewKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " moisture per crew kg/s").c_str(),
		&crew.moisturePerCrewKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " urine per crew per s").c_str(),
		&crew.urinePerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " organic waste per crew per s").c_str(),
		&crew.organicWastePerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " metabolic heat per crew kW").c_str(),
		&crew.metabolicHeatPerCrewKW, 0.0, 0.0, "%.4f");
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeCrew = true;
	ImGui::PopItemWidth();
	return removeCrew;
}

bool renderMicrobialProcessorEntry(core::StationSystemsSettings& draft, core::MicrobialProcessorSettings& processor,
	std::string const& label, std::string const& removeLabel)
{
	bool removeProcessor = false;
	renderIdentity(label, processor.id);
	renderWaterQuality(processor.waterQuality, label);
	for (auto const* name : core::StationWaterDryConstituents)
	{
		bool enabled = processor.recoveredWaterCarryoverFractions.contains(name);
		if (ImGui::Checkbox((label + " carry to recovered water: " + name).c_str(), &enabled))
		{ if (enabled) processor.recoveredWaterCarryoverFractions[name] = 0; else processor.recoveredWaterCarryoverFractions.erase(name); }
		if (enabled) ImGui::InputDouble((label + " " + name + " carry-over fraction").c_str(),
			&processor.recoveredWaterCarryoverFractions.at(name), 0, 0, "%.6f");
	}
	char processorHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorHazardBuffer, processor.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		processorHazardBuffer, sizeof(processorHazardBuffer)))
		processor.hazardClass = processorHazardBuffer;
	char processorFeedBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorFeedBuffer, processor.feedStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " organic waste feed store").c_str(),
		processorFeedBuffer, sizeof(processorFeedBuffer)))
		processor.feedStoreId = processorFeedBuffer;
	char processorOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorOxygenBuffer, processor.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " oxygen store").c_str(),
		processorOxygenBuffer, sizeof(processorOxygenBuffer)))
		processor.oxygenStoreId = processorOxygenBuffer;
	char processorWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorWaterBuffer, processor.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " process water store").c_str(),
		processorWaterBuffer, sizeof(processorWaterBuffer)))
		processor.waterStoreId = processorWaterBuffer;
	char processorMediaBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorMediaBuffer, processor.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " culture medium store").c_str(),
		processorMediaBuffer, sizeof(processorMediaBuffer)))
		processor.consumableStoreId = processorMediaBuffer;
	char processorNutrientBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorNutrientBuffer, processor.nutrientStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " recovered nutrient store").c_str(),
		processorNutrientBuffer, sizeof(processorNutrientBuffer)))
		processor.nutrientStoreId = processorNutrientBuffer;
	char processorRecoveredWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorRecoveredWaterBuffer, processor.recoveredWaterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " recovered water store").c_str(),
		processorRecoveredWaterBuffer, sizeof(processorRecoveredWaterBuffer)))
		processor.recoveredWaterStoreId = processorRecoveredWaterBuffer;
	char processorCo2Buffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorCo2Buffer, processor.co2StoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " CO2 store").c_str(),
		processorCo2Buffer, sizeof(processorCo2Buffer)))
		processor.co2StoreId = processorCo2Buffer;
	char processorResidueBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorResidueBuffer, processor.residueStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " residue store").c_str(),
		processorResidueBuffer, sizeof(processorResidueBuffer)))
		processor.residueStoreId = processorResidueBuffer;
	char processorStockBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorStockBuffer, processor.stockStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " replacement biological stock store").c_str(),
		processorStockBuffer, sizeof(processorStockBuffer)))
		processor.stockStoreId = processorStockBuffer;
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
	ImGui::InputDouble((label + " rated feed processing kg/s").c_str(),
		&processor.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " specific energy kWh/kg feed").c_str(),
		&processor.specificEnergyKWhPerKgFeed, 0.0, 0.0, "%.4f");
	int basis = static_cast<int>(processor.rateBasis);
	if (ImGui::Combo((label + " rate basis").c_str(), &basis, "Fixed per feed mass\0Composition derived\0"))
		processor.rateBasis = static_cast<core::StationRateBasis>(basis);
	if (processor.rateBasis == core::StationRateBasis::FixedPerFeedMass)
	{
		ImGui::InputDouble((label + " oxygen per kg feed kg").c_str(),
			&processor.oxygenPerKgFeedKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " process water per kg feed kg").c_str(),
			&processor.processWaterPerKgFeedKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " medium units per kg feed").c_str(),
			&processor.consumablePerKgFeed, 0.0, 0.0, "%.4f");
		ImGui::InputDouble((label + " CO2 per kg feed kg").c_str(),
			&processor.co2PerKgFeedKg, 0.0, 0.0, "%.4f");
	}
	else
	{
		ImGui::InputDouble((label + " organic oxidation fraction").c_str(), &processor.organicOxidationFraction);
		ImGui::InputDouble((label + " O2 per kg organic oxidized").c_str(), &processor.o2PerKgOrganicOxidized);
		ImGui::InputDouble((label + " nitrification fraction").c_str(), &processor.nitrificationFraction);
		ImGui::InputDouble((label + " O2 per kg nitrified N").c_str(), &processor.o2PerKgNitrifiedN);
		ImGui::InputDouble((label + " CO2 per kg organic oxidized").c_str(), &processor.co2PerKgOrganicOxidized);
		ImGui::InputDouble((label + " process water target moisture").c_str(), &processor.processWaterTargetMoisture);
		ImGui::InputDouble((label + " heat release fraction").c_str(), &processor.heatReleaseFraction);
		ImGui::InputDouble((label + " medium kg per kg organic oxidized").c_str(), &processor.consumablePerKgOrganicOxidized);
	}
	ImGui::InputDouble((label + " nitrogen recovery").c_str(),
		&processor.nitrogenRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " phosphorus recovery").c_str(),
		&processor.phosphorusRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " potassium recovery").c_str(),
		&processor.potassiumRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " minerals recovery").c_str(),
		&processor.mineralsRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " water recovery").c_str(),
		&processor.waterRecovery, 0.0, 0.0, "%.4f");
	if (processor.rateBasis == core::StationRateBasis::FixedPerFeedMass)
		ImGui::InputDouble((label + " reaction heat kJ/kg feed").c_str(),
			&processor.reactionHeatKJPerKgFeed, 0.0, 0.0, "%.2f");
	// Re-evaluate from the draft every frame, after all rate/recovery edits.
	// This deliberately never observes the World's live feed blend.
	if (processor.rateBasis == core::StationRateBasis::CompositionDerived && !processor.feedStoreId.empty())
	{
		auto const feed = std::find_if(draft.stores.begin(), draft.stores.end(), [&](auto const& store) {
			return store.id == processor.feedStoreId;
		});
		if (feed != draft.stores.end() && feed->composition)
		{
			auto const rates = core::stationMicrobialRates(processor, *feed->composition);
			std::array values{rates.oxygen * processor.rateKgPerS, rates.water * processor.rateKgPerS,
				rates.co2 * processor.rateKgPerS, rates.consumable * processor.rateKgPerS,
				rates.heat * processor.rateKgPerS, rates.nitrifiedNitrogen * processor.rateKgPerS};
			if (std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v) && v >= 0.0; }))
				ImGui::TextWrapped("Preview at declared feed composition (not runtime): O2 %.4g kg/s; water %.4g kg/s; CO2 %.4g kg/s; medium %.4g kg/s; heat %.4g kW; nitrified N %.4g kg/s",
					values[0], values[1], values[2], values[3], values[4], values[5]);
			else
				ImGui::TextUnformatted("Preview unavailable at declared feed composition: invalid draft rates.");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Estimate at rated feed throughput, before condition or resource throttling. Runtime uses the actual drawn blend, not this declaration.");
		}
	}
	ImGui::InputDouble((label + " min operating temperature C").c_str(),
		&processor.minOperatingTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble((label + " max sink temperature C").c_str(),
		&processor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble((label + " initial condition").c_str(),
		&processor.initialCondition, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " starvation degrade per hour").c_str(),
		&processor.starvationDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " consumable exhaustion degrade per hour").c_str(),
		&processor.consumableExhaustionDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " cold stress degrade per hour").c_str(),
		&processor.coldStressDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " heat stress degrade per hour").c_str(),
		&processor.heatStressDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " recovery per hour").c_str(),
		&processor.recoveryPerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " stock units per condition point").c_str(),
		&processor.stockUnitsPerConditionPoint, 0.0, 0.0, "%.4f");
	ImGui::InputInt((label + " priority").c_str(), &processor.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeProcessor = true;
	ImGui::PopItemWidth();
	return removeProcessor;
}

bool renderCropPopulationEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::CropPopulationSettings& crop,
	std::string const& label, std::string const& removeLabel)
{
	bool removeCrop = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, crop.id);
	renderWaterQuality(crop.waterQuality, label);
	char cropAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropAtmosphereBuffer, crop.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " atmosphere").c_str(),
		cropAtmosphereBuffer, sizeof(cropAtmosphereBuffer)))
		crop.atmosphereId = cropAtmosphereBuffer;
	char cropWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropWaterBuffer, crop.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " irrigation water store").c_str(),
		cropWaterBuffer, sizeof(cropWaterBuffer)))
		crop.waterStoreId = cropWaterBuffer;
	char cropNitrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropNitrogenBuffer, crop.nitrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " nitrogen store").c_str(),
		cropNitrogenBuffer, sizeof(cropNitrogenBuffer)))
		crop.nitrogenStoreId = cropNitrogenBuffer;
	char cropPhosphorusBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropPhosphorusBuffer, crop.phosphorusStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " phosphorus store").c_str(),
		cropPhosphorusBuffer, sizeof(cropPhosphorusBuffer)))
		crop.phosphorusStoreId = cropPhosphorusBuffer;
	char cropPotassiumBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropPotassiumBuffer, crop.potassiumStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " potassium store").c_str(),
		cropPotassiumBuffer, sizeof(cropPotassiumBuffer)))
		crop.potassiumStoreId = cropPotassiumBuffer;
	char cropMineralsBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropMineralsBuffer, crop.mineralsStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " minerals store").c_str(),
		cropMineralsBuffer, sizeof(cropMineralsBuffer)))
		crop.mineralsStoreId = cropMineralsBuffer;
	// #581: the capacity-factor fields use %g so a large authored
	// value round-trips through the 64-character InputScalar buffer.
	// With %f a value above ~1e62 truncated to the buffer width and
	// re-committing it silently changed the number; %g keeps every
	// magnitude - including the overflow candidates the capacity
	// contract now refuses - displayable and editable exactly.
	ImGui::InputDouble((label + " growing area m2").c_str(),
		&crop.growingAreaM2, 0.0, 0.0, "%.15g");
	ImGui::InputDouble((label + " max biomass density kg/m2").c_str(),
		&crop.maxBiomassDensityKgPerM2, 0.0, 0.0, "%.15g");
	ImGui::InputDouble((label + " initial dry biomass kg").c_str(),
		&crop.initialBiomassKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " lamp rated kW").c_str(),
		&crop.lampRatedKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " circulation power kW (drawn before lamp)").c_str(),
		&crop.circulationPowerKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " growth yield kg per kWh light").c_str(),
		&crop.growthYieldKgPerKWh, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " respiration per hour").c_str(),
		&crop.respirationPerHour, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " transpiration kg per kg growth").c_str(),
		&crop.transpirationKgPerKgGrowth, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " min growing temperature C").c_str(),
		&crop.minGrowingTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble((label + " max growing temperature C").c_str(),
		&crop.maxGrowingTemperatureC, 0.0, 0.0, "%.2f");
	char cropFoodBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropFoodBuffer, crop.foodStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " food store").c_str(),
		cropFoodBuffer, sizeof(cropFoodBuffer)))
		crop.foodStoreId = cropFoodBuffer;
	char cropWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropWasteBuffer, crop.wasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " waste store").c_str(),
		cropWasteBuffer, sizeof(cropWasteBuffer)))
		crop.wasteStoreId = cropWasteBuffer;
	ImGui::InputDouble((label + " harvest trigger kg (0 disables harvest)").c_str(),
		&crop.harvestTriggerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " harvest fraction of living biomass").c_str(),
		&crop.harvestFraction, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " edible fraction of harvest").c_str(),
		&crop.edibleFraction, 0.0, 0.0, "%.4f");
	bool cropPeriodic = crop.illuminationProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox((label + " periodic illumination").c_str(), &cropPeriodic))
	{
		if (cropPeriodic)
		{
			if (crop.illuminationProfile.periodSeconds <= 0.0) crop.illuminationProfile.periodSeconds = 120.0;
			if (crop.illuminationProfile.segments.empty())
				crop.illuminationProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			crop.illuminationProfile.periodSeconds = 0.0;
			crop.illuminationProfile.segments.clear();
		}
	}
	if (!cropPeriodic)
		ImGui::InputDouble((label + " constant illumination level").c_str(),
			&crop.illuminationProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble((label + " illumination period seconds").c_str(),
			&crop.illuminationProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < crop.illuminationProfile.segments.size(); ++segment)
		{
			auto& stretch = crop.illuminationProfile.segments[segment];
			auto const segmentRow = label + " illumination segment " + std::to_string(segment);
			ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(("Remove " + segmentRow).c_str()))
				crop.illuminationProfile.segments.erase(
					crop.illuminationProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (crop.illuminationProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add illumination segment to " + label).c_str()))
			crop.illuminationProfile.segments.push_back(
				{crop.illuminationProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::Text("  declared dry-mass composition (moisture and atmospheric nitrogen must be 0 for crops):");
	ImGui::InputDouble((label + " composition moisture fraction").c_str(),
		&crop.composition.moisture, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition nutrient nitrogen fraction").c_str(),
		&crop.composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition atmospheric nitrogen fraction").c_str(),
		&crop.composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition phosphorus fraction").c_str(),
		&crop.composition.phosphorus, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition potassium fraction").c_str(),
		&crop.composition.potassium, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition other minerals fraction").c_str(),
		&crop.composition.otherMinerals, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition energy kJ per kg dry").c_str(),
		&crop.composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	ImGui::InputInt((label + " priority").c_str(), &crop.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeCrop = true;
	ImGui::PopItemWidth();
	return removeCrop;
}

bool renderAlgaeCultureEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::AlgaeCultureSettings& algae,
	std::string const& label, std::string const& removeLabel)
{
	bool removeAlgae = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, algae.id);
	renderWaterQuality(algae.waterQuality, label);
	char algaeAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeAtmosphereBuffer, algae.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " atmosphere").c_str(),
		algaeAtmosphereBuffer, sizeof(algaeAtmosphereBuffer)))
		algae.atmosphereId = algaeAtmosphereBuffer;
	char algaeWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeWaterBuffer, algae.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " culture water store").c_str(),
		algaeWaterBuffer, sizeof(algaeWaterBuffer)))
		algae.waterStoreId = algaeWaterBuffer;
	char algaeNitrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeNitrogenBuffer, algae.nitrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " nitrogen store").c_str(),
		algaeNitrogenBuffer, sizeof(algaeNitrogenBuffer)))
		algae.nitrogenStoreId = algaeNitrogenBuffer;
	char algaePhosphorusBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaePhosphorusBuffer, algae.phosphorusStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " phosphorus store").c_str(),
		algaePhosphorusBuffer, sizeof(algaePhosphorusBuffer)))
		algae.phosphorusStoreId = algaePhosphorusBuffer;
	char algaePotassiumBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaePotassiumBuffer, algae.potassiumStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " potassium store").c_str(),
		algaePotassiumBuffer, sizeof(algaePotassiumBuffer)))
		algae.potassiumStoreId = algaePotassiumBuffer;
	char algaeMineralsBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeMineralsBuffer, algae.mineralsStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " minerals store").c_str(),
		algaeMineralsBuffer, sizeof(algaeMineralsBuffer)))
		algae.mineralsStoreId = algaeMineralsBuffer;
	ImGui::InputDouble((label + " active culture volume L").c_str(),
		&algae.cultureVolumeL, 0.0, 0.0, "%.15g");
	ImGui::InputDouble((label + " max biomass density kg/L").c_str(),
		&algae.maxBiomassDensityKgPerL, 0.0, 0.0, "%.15g");
	ImGui::InputDouble((label + " initial dry biomass kg").c_str(),
		&algae.initialBiomassKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " lamp rated kW").c_str(),
		&algae.lampRatedKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " circulation power kW (drawn before lamp)").c_str(),
		&algae.circulationPowerKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " growth yield kg per kWh light").c_str(),
		&algae.growthYieldKgPerKWh, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " respiration per hour").c_str(),
		&algae.respirationPerHour, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " evaporation kg per kg growth").c_str(),
		&algae.evaporationKgPerKgGrowth, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " min growing temperature C").c_str(),
		&algae.minGrowingTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble((label + " max growing temperature C").c_str(),
		&algae.maxGrowingTemperatureC, 0.0, 0.0, "%.2f");
	char algaeFoodBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeFoodBuffer, algae.foodStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " food store").c_str(),
		algaeFoodBuffer, sizeof(algaeFoodBuffer)))
		algae.foodStoreId = algaeFoodBuffer;
	char algaeRecoveryBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeRecoveryBuffer, algae.recoveryStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " recovery store").c_str(),
		algaeRecoveryBuffer, sizeof(algaeRecoveryBuffer)))
		algae.recoveryStoreId = algaeRecoveryBuffer;
	char algaeWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeWasteBuffer, algae.wasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " waste store").c_str(),
		algaeWasteBuffer, sizeof(algaeWasteBuffer)))
		algae.wasteStoreId = algaeWasteBuffer;
	ImGui::InputDouble((label + " harvest trigger kg (0 disables harvest)").c_str(),
		&algae.harvestTriggerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " harvest fraction of living biomass").c_str(),
		&algae.harvestFraction, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " food fraction of harvest").c_str(),
		&algae.foodFraction, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " recovery fraction of harvest").c_str(),
		&algae.recoveryFraction, 0.0, 0.0, "%.4f");
	bool algaePeriodic = algae.illuminationProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox((label + " periodic illumination").c_str(), &algaePeriodic))
	{
		if (algaePeriodic)
		{
			if (algae.illuminationProfile.periodSeconds <= 0.0) algae.illuminationProfile.periodSeconds = 120.0;
			if (algae.illuminationProfile.segments.empty())
				algae.illuminationProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			algae.illuminationProfile.periodSeconds = 0.0;
			algae.illuminationProfile.segments.clear();
		}
	}
	if (!algaePeriodic)
		ImGui::InputDouble((label + " constant illumination level").c_str(),
			&algae.illuminationProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble((label + " illumination period seconds").c_str(),
			&algae.illuminationProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < algae.illuminationProfile.segments.size(); ++segment)
		{
			auto& stretch = algae.illuminationProfile.segments[segment];
			auto const segmentRow = label + " illumination segment " + std::to_string(segment);
			ImGui::InputDouble((segmentRow + " start").c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble((segmentRow + " level").c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(("Remove " + segmentRow).c_str()))
				algae.illuminationProfile.segments.erase(
					algae.illuminationProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (algae.illuminationProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add illumination segment to " + label).c_str()))
			algae.illuminationProfile.segments.push_back(
				{algae.illuminationProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::Text("  declared dry-mass composition (moisture and atmospheric nitrogen must be 0 for cultures):");
	ImGui::InputDouble((label + " composition moisture fraction").c_str(),
		&algae.composition.moisture, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition nutrient nitrogen fraction").c_str(),
		&algae.composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition atmospheric nitrogen fraction").c_str(),
		&algae.composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition phosphorus fraction").c_str(),
		&algae.composition.phosphorus, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition potassium fraction").c_str(),
		&algae.composition.potassium, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition other minerals fraction").c_str(),
		&algae.composition.otherMinerals, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " composition energy kJ per kg dry").c_str(),
		&algae.composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	ImGui::InputInt((label + " priority").c_str(), &algae.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeAlgae = true;
	ImGui::PopItemWidth();
	return removeAlgae;
}

bool renderStoreEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceStoreSettings& store,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, store.id);
	char materialBuffer[core::MaxStationMaterialIdLength + 1]{};
	std::strncpy(materialBuffer, store.materialId.c_str(), sizeof(materialBuffer) - 1);
	if (ImGui::InputText((label + " material identity").c_str(), materialBuffer, sizeof(materialBuffer)))
		store.materialId = materialBuffer;
	if (store.materialId.empty()) ImGui::TextUnformatted("Material: Unclassified");
	else if (!core::isValidStationMaterialId(store.materialId))
		ImGui::TextUnformatted("Invalid material identity: use lowercase words/digits separated by single hyphens.");
	else ImGui::Text("Material: %s (explicit)", store.materialId.c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material identity is independent of store name, unit, hazard and composition. Empty is unclassified; it never changes allocation.");
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

bool renderWaterCollectorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WaterCollectorSettings& collector, std::string const& label, std::string const& removeLabel)
{
	bool removeCollector = false;
	renderIdentity(label, collector.id);
	char collectorHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorHazardBuffer, collector.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		collectorHazardBuffer, sizeof(collectorHazardBuffer)))
		collector.hazardClass = collectorHazardBuffer;
	char collectorAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorAtmosphereBuffer, collector.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " atmosphere").c_str(),
		collectorAtmosphereBuffer, sizeof(collectorAtmosphereBuffer)))
		collector.atmosphereId = collectorAtmosphereBuffer;
	char collectorLoopBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorLoopBuffer, collector.coolingLoopId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " cooling loop").c_str(),
		collectorLoopBuffer, sizeof(collectorLoopBuffer)))
		collector.coolingLoopId = collectorLoopBuffer;
	char collectorClassBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorClassBuffer, collector.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " thermal class").c_str(),
		collectorClassBuffer, sizeof(collectorClassBuffer)))
		collector.thermalClass = collectorClassBuffer;
	char collectorDestBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorDestBuffer, collector.destStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " raw water store").c_str(),
		collectorDestBuffer, sizeof(collectorDestBuffer)))
		collector.destStoreId = collectorDestBuffer;
	ImGui::PushItemWidth(140.0f);
	ImGui::InputDouble((label + " max rate kg/s").c_str(),
		&collector.maxRateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " target RH").c_str(),
		&collector.targetRelativeHumidity, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCollector = true;
	ImGui::PopItemWidth();
	return removeCollector;
}

bool renderWaterTreatmentEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WaterTreatmentSettings& treatment, std::string const& label, std::string const& removeLabel)
{
	bool removeTreatment = false;
	renderIdentity(label, treatment.id);
	char treatmentHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentHazardBuffer, treatment.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		treatmentHazardBuffer, sizeof(treatmentHazardBuffer)))
		treatment.hazardClass = treatmentHazardBuffer;
	char treatmentSourceBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentSourceBuffer, treatment.sourceStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " raw water store").c_str(),
		treatmentSourceBuffer, sizeof(treatmentSourceBuffer)))
		treatment.sourceStoreId = treatmentSourceBuffer;
	char treatmentTreatedBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentTreatedBuffer, treatment.treatedStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " treated store").c_str(),
		treatmentTreatedBuffer, sizeof(treatmentTreatedBuffer)))
		treatment.treatedStoreId = treatmentTreatedBuffer;
	char treatmentConcentrateBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentConcentrateBuffer, treatment.concentrateStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " concentrate store").c_str(),
		treatmentConcentrateBuffer, sizeof(treatmentConcentrateBuffer)))
		treatment.concentrateStoreId = treatmentConcentrateBuffer;
	char treatmentConsumableBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentConsumableBuffer, treatment.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " consumable store").c_str(),
		treatmentConsumableBuffer, sizeof(treatmentConsumableBuffer)))
		treatment.consumableStoreId = treatmentConsumableBuffer;
	char treatmentClearanceBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentClearanceBuffer, treatment.clearanceStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " release-clearance store").c_str(),
		treatmentClearanceBuffer, sizeof(treatmentClearanceBuffer)))
		treatment.clearanceStoreId = treatmentClearanceBuffer;
	ImGui::PushItemWidth(140.0f);
	ImGui::InputDouble((label + " clearance per kg").c_str(),
		&treatment.clearancePerKg, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " throughput kg/s").c_str(),
		&treatment.throughputKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " recovery ratio").c_str(),
		&treatment.recoveryRatio, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " specific energy kWh/kg").c_str(),
		&treatment.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " consumable per kg").c_str(),
		&treatment.consumablePerKg, 0.0, 0.0, "%.6f");
	ImGui::TextUnformatted("Removal fractions (default: all removed)");
	for (auto const* name : core::StationWaterDryConstituents)
	{
		auto found = treatment.removalFractions.find(name);
		bool authored = found != treatment.removalFractions.end();
		if (ImGui::Checkbox((label + " override " + name).c_str(), &authored))
		{
			if (authored) treatment.removalFractions[name] = 1.0;
			else treatment.removalFractions.erase(name);
		}
		if (authored)
			ImGui::InputDouble((label + " removal " + name).c_str(),
				&treatment.removalFractions.at(name), 0, 0, "%.6f");
	}
	auto const source = std::find_if(draft.stores.begin(), draft.stores.end(),
		[&](auto const& store) { return store.id == treatment.sourceStoreId; });
	if (source == draft.stores.end()) ImGui::TextUnformatted("Effluent preview: connect a source store");
	else if (!source->composition) ImGui::TextUnformatted("Effluent preview: pure water (declared source)");
	else
	{
		auto const effluent = core::waterTreatmentEffluent(*source->composition, treatment);
		double total = 0;
		for (auto const& [name, mass] : effluent) if (name != "energy") total += mass;
		ImGui::TextUnformatted("Effluent concentration preview (declared source; kg/kg stream)");
		for (auto const* name : core::StationWaterDryConstituents)
			ImGui::Text("  %s: %.8g", name, total > 0 ? effluent.at(name) / total : 0.0);
	}
	ImGui::InputInt((label + " priority").c_str(), &treatment.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeTreatment = true;
	ImGui::PopItemWidth();
	return removeTreatment;
}

bool renderElectrolyzerEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ElectrolyzerSettings& cell, std::string const& label, std::string const& removeLabel)
{
	bool removeCell = false;
	renderIdentity(label, cell.id);
	char cellHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellHazardBuffer, cell.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		cellHazardBuffer, sizeof(cellHazardBuffer)))
		cell.hazardClass = cellHazardBuffer;
	char cellWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellWaterBuffer, cell.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " feed water store").c_str(),
		cellWaterBuffer, sizeof(cellWaterBuffer)))
		cell.waterStoreId = cellWaterBuffer;
	char cellOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellOxygenBuffer, cell.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " oxygen store (or leave empty for the compartment)").c_str(),
		cellOxygenBuffer, sizeof(cellOxygenBuffer)))
		cell.oxygenStoreId = cellOxygenBuffer;
	char cellHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellHydrogenBuffer, cell.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hydrogen store").c_str(),
		cellHydrogenBuffer, sizeof(cellHydrogenBuffer)))
		cell.hydrogenStoreId = cellHydrogenBuffer;
	char cellAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellAtmosphereBuffer, cell.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " delivery compartment (or leave empty for the store)").c_str(),
		cellAtmosphereBuffer, sizeof(cellAtmosphereBuffer)))
		cell.atmosphereId = cellAtmosphereBuffer;
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
	ImGui::PushItemWidth(140.0f);
	ImGui::InputDouble((label + " rated split kg/s").c_str(),
		&cell.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " specific energy kWh/kg water").c_str(),
		&cell.specificEnergyKWhPerKgWater, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " max sink temperature C").c_str(),
		&cell.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt((label + " priority").c_str(), &cell.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCell = true;
	ImGui::PopItemWidth();
	return removeCell;
}

bool renderCo2CaptureEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::Co2CaptureSettings& capture, std::string const& label, std::string const& removeLabel)
{
	bool removeCapture = false;
	renderIdentity(label, capture.id);
	char captureHazardBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureHazardBuffer, capture.hazardClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " hazard class").c_str(),
		captureHazardBuffer, sizeof(captureHazardBuffer)))
		capture.hazardClass = captureHazardBuffer;
	char captureAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureAtmosphereBuffer, capture.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " compartment").c_str(),
		captureAtmosphereBuffer, sizeof(captureAtmosphereBuffer)))
		capture.atmosphereId = captureAtmosphereBuffer;
	char captureStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureStoreBuffer, capture.captureStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " capture store").c_str(),
		captureStoreBuffer, sizeof(captureStoreBuffer)))
		capture.captureStoreId = captureStoreBuffer;
	char captureConsumableBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureConsumableBuffer, capture.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " consumable store (empty: none)").c_str(),
		captureConsumableBuffer, sizeof(captureConsumableBuffer)))
		capture.consumableStoreId = captureConsumableBuffer;
	ImGui::PushItemWidth(140.0f);
	ImGui::InputDouble((label + " max rate kg/s").c_str(),
		&capture.maxRateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " specific energy kWh/kg").c_str(),
		&capture.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " consumable per kg").c_str(),
		&capture.consumablePerKg, 0.0, 0.0, "%.6f");
	ImGui::InputInt((label + " priority").c_str(), &capture.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCapture = true;
	ImGui::PopItemWidth();
	return removeCapture;
}

bool renderAtmosphereEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::AtmosphereSettings& atmosphere, std::string const& label, std::string const& removeLabel)
{
	bool removeAtmosphere = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, atmosphere.id);
	char hazardBuffer[16]{};
	std::strncpy(hazardBuffer, atmosphere.hazardClass.c_str(), sizeof(hazardBuffer) - 1);
	if (ImGui::InputText((label + " hazard class").c_str(),
		hazardBuffer, sizeof(hazardBuffer)))
		atmosphere.hazardClass = hazardBuffer;
	ImGui::InputDouble((label + " volume m3").c_str(),
		&atmosphere.volumeM3, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting oxygen kg").c_str(),
		&atmosphere.startingOxygenKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " starting nitrogen kg").c_str(),
		&atmosphere.startingNitrogenKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " starting CO2 kg").c_str(),
		&atmosphere.startingCarbonDioxideKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " starting water vapour kg").c_str(),
		&atmosphere.startingWaterVapourKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " starting temperature C").c_str(),
		&atmosphere.startingTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " thermal capacity kJ/K").c_str(),
		&atmosphere.thermalCapacityKJPerK, 0.0, 0.0, "%.3f");
	char atmosphereReservoirBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(atmosphereReservoirBuffer, atmosphere.reservoirId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " reservoir (empty = adiabatic)").c_str(),
		atmosphereReservoirBuffer, sizeof(atmosphereReservoirBuffer)))
		atmosphere.reservoirId = atmosphereReservoirBuffer;
	ImGui::InputDouble((label + " exchanger conductance kW/K").c_str(),
		&atmosphere.exchangerConductanceKWPerK, 0.0, 0.0, "%.4f");
	ImGui::InputInt((label + " occupants").c_str(),
		&atmosphere.occupants, 1, 0);
	ImGui::InputDouble((label + " O2 use per occupant kg/s").c_str(),
		&atmosphere.oxygenUsePerOccupantKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " CO2 produced per occupant kg/s").c_str(),
		&atmosphere.co2ProductionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " sensible heat per occupant kW").c_str(),
		&atmosphere.sensibleHeatPerOccupantKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " moisture added per occupant kg/s").c_str(),
		&atmosphere.moistureAdditionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " leak rate kg/s").c_str(),
		&atmosphere.leakRateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble((label + " min pressure kPa").c_str(),
		&atmosphere.minPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max pressure kPa").c_str(),
		&atmosphere.maxPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " min O2 partial pressure kPa").c_str(),
		&atmosphere.minOxygenPartialPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max CO2 partial pressure kPa").c_str(),
		&atmosphere.maxCarbonDioxidePartialPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeAtmosphere = true;
	ImGui::PopItemWidth();
	return removeAtmosphere;
}

bool renderAirlockCycleEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::AirlockCycleSettings& cycle, std::string const& label, std::string const& removeLabel)
{
	bool removeCycle = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, cycle.id);
	char cycleAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cycleAtmosphereBuffer, cycle.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " atmosphere").c_str(),
		cycleAtmosphereBuffer, sizeof(cycleAtmosphereBuffer)))
		cycle.atmosphereId = cycleAtmosphereBuffer;
	ImGui::InputDouble((label + " gas mass kg").c_str(),
		&cycle.gasMassKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " first occurrence s").c_str(),
		&cycle.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recurrence s (0 = one-off)").c_str(),
		&cycle.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recovered fraction").c_str(),
		&cycle.recoveredFraction, 0.0, 0.0, "%.3f");
	ImGui::InputInt((label + " priority").c_str(),
		&cycle.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCycle = true;
	ImGui::PopItemWidth();
	return removeCycle;
}

bool renderTransferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceTransferSettings& transfer,
	std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, transfer.id);
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
	int mode = static_cast<int>(transfer.mode);
	if (ImGui::Combo((label + " mode").c_str(), &mode, "Continuous\0Scheduled\0"))
		transfer.mode = static_cast<core::ResourceTransferMode>(mode);
	if (transfer.mode == core::ResourceTransferMode::Scheduled)
	{
		ImGui::InputDouble((label + " quantity").c_str(), &transfer.quantity, 0.0, 0.0, "%.3f");
		ImGui::InputDouble((label + " first occurrence seconds").c_str(), &transfer.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble((label + " recurrence seconds (0 = one-off)").c_str(), &transfer.recurrenceSeconds, 0.0, 0.0, "%.6f");
		ImGui::TextWrapped("Recurrence is zero for one-off, otherwise at least one fixed tick. Each occurrence requests the original quantity; shortages never accumulate. Lossless Non-physical inventory event; connection ratings are informational.");
	}
	else ImGui::InputDouble((label + " rate per second").c_str(), &transfer.ratePerSecond, 0.0, 0.0, "%.3f");
	ImGui::TextWrapped("Switching mode retains inactive settings and connection design properties. Only the active mode executes after Apply.");
	// Keep invalid drafts editable (including hidden settings after a switch),
	// but use the same active-mode validation as Apply and document loading.
	std::string diagnostic;
	if (!core::validateStationTransferSettings(transfer, diagnostic))
		ImGui::TextWrapped("%s", diagnostic.c_str());
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
	renderIdentity(label, load.id);
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
	renderIdentity(label, battery.id);
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
	renderIdentity(label, generator.id);
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
	renderIdentity(label, cell.id);
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
	renderIdentity(label, reactor.id);
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
	renderIdentity(label, processor.id);
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
	renderIdentity(label, reservoir.id);
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
	renderIdentity(label, radiator.id);
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

bool renderCoolantLoopEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::CoolantLoopSettings& loop,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushItemWidth(220);
	renderIdentity(label, loop.id);
	char hazard[16]{};
	std::strncpy(hazard, loop.hazardClass.c_str(), sizeof(hazard) - 1);
	if (ImGui::InputText((label + " hazard class").c_str(), hazard, sizeof(hazard))) loop.hazardClass = hazard;
	char host[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(host, loop.reservoirId.c_str(), sizeof(host) - 1);
	if (ImGui::InputText((label + " reservoir").c_str(), host, sizeof(host))) loop.reservoirId = host;
	ImGui::InputDouble((label + " coolant capacity kg").c_str(), &loop.coolantCapacityKg, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting coolant kg").c_str(), &loop.startingCoolantKg, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " specific heat kJ/kg K").c_str(), &loop.specificHeatKJPerKgK, 0.0, 0.0, "%.4f");
	ImGui::InputDouble((label + " starting temperature C").c_str(), &loop.startingTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " freeze setpoint C").c_str(), &loop.freezeProtectionSetpointC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " pump power kW").c_str(), &loop.pumpPowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " rated flow kg/s").c_str(), &loop.ratedFlowKgPerS, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " exchanger conductance kW/K").c_str(), &loop.exchangerConductanceKWPerK, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " heater capacity kW").c_str(), &loop.heaterCapacityKW, 0.0, 0.0, "%.3f");
	ImGui::Checkbox((label + " protected critical service"
		+ (loop.hazardClass == "hazardous" ? " (hazardous loops are always protected)"
			: " (circulation and freeze heat drawn ahead of discretionary loads)")).c_str(), &loop.criticalService);
	ImGui::SameLine();
	bool const remove = !removeLabel.empty() && ImGui::Button(removeLabel.c_str());
	ImGui::PopItemWidth();
	return remove;
}

bool renderThermalBufferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ThermalBufferSettings& buffer, std::string const& label, std::string const& removeLabel)
{
	bool removeRequested = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, buffer.id);
	char host[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(host, buffer.reservoirId.c_str(), sizeof(host) - 1);
	if (ImGui::InputText((label + " host reservoir id").c_str(), host, sizeof(host))) buffer.reservoirId = host;
	char access[16]{};
	std::strncpy(access, buffer.accessMode.c_str(), sizeof(access) - 1);
	if (ImGui::InputText((label + " access mode (circulation|passive)").c_str(), access, sizeof(access))) buffer.accessMode = access;
	char loop[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(loop, buffer.circulationLoopId.c_str(), sizeof(loop) - 1);
	if (ImGui::InputText((label + " circulation loop id").c_str(), loop, sizeof(loop))) buffer.circulationLoopId = loop;
	ImGui::InputDouble((label + " capacity kJ").c_str(), &buffer.capacityKJ, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " starting energy kJ").c_str(), &buffer.startingEnergyKJ, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " min usable temperature C").c_str(), &buffer.minUsableTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max usable temperature C").c_str(), &buffer.maxUsableTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max charge kW").c_str(), &buffer.maxChargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max discharge kW").c_str(), &buffer.maxDischargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " transfer conductance kW/K").c_str(), &buffer.transferConductanceKWPerK, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str())) removeRequested = true;
	ImGui::PopItemWidth();
	return removeRequested;
}

bool renderChillerEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ChillerSettings& chiller, std::string const& label, std::string const& removeLabel)
{
	bool removeChiller = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, chiller.id);
	char chillerClassBuffer[16]{};
	std::strncpy(chillerClassBuffer, chiller.thermalClass.c_str(), sizeof(chillerClassBuffer) - 1);
	if (ImGui::InputText((label + " thermal class").c_str(),
		chillerClassBuffer, sizeof(chillerClassBuffer)))
		chiller.thermalClass = chillerClassBuffer;
	char coldLoopBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(coldLoopBuffer, chiller.coldLoopId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " cold loop").c_str(),
		coldLoopBuffer, sizeof(coldLoopBuffer)))
		chiller.coldLoopId = coldLoopBuffer;
	char condenserBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(condenserBuffer, chiller.condenserReservoirId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " condenser reservoir").c_str(),
		condenserBuffer, sizeof(condenserBuffer)))
		chiller.condenserReservoirId = condenserBuffer;
	ImGui::InputDouble((label + " rated cooling capacity kW").c_str(),
		&chiller.ratedCoolingCapacityKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " COP").c_str(),
		&chiller.cop, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " auxiliary power kW").c_str(),
		&chiller.auxiliaryPowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " chilled setpoint C").c_str(),
		&chiller.chilledSetpointC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " max condensing temperature C").c_str(),
		&chiller.maxCondensingTemperatureC, 0.0, 0.0, "%.3f");
	// #416: a protected critical-service chiller's work is credited
	// from the actual pool ahead of every discretionary external load,
	// sized from its real tick-start state, so shutdown-cooling and
	// containment-thermal chillers keep running when the experiments or
	// generation they serve stop.
	ImGui::Checkbox((label
		+ " protected critical service (work credited ahead of discretionary loads)").c_str(),
		&chiller.criticalService);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeChiller = true;
	ImGui::PopItemWidth();
	return removeChiller;
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

bool renderDeliveryEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceDeliverySettings& delivery, std::string const& label, std::string const& removeLabel)
{
	bool removeDelivery = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, delivery.id);
	char deliveryStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(deliveryStoreBuffer, delivery.storeId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " store").c_str(),
		deliveryStoreBuffer, sizeof(deliveryStoreBuffer)))
		delivery.storeId = deliveryStoreBuffer;
	char deliveryUnitBuffer[8]{};
	std::strncpy(deliveryUnitBuffer, delivery.unit.c_str(), sizeof(deliveryUnitBuffer) - 1);
	if (ImGui::InputText((label + " unit").c_str(), deliveryUnitBuffer, sizeof(deliveryUnitBuffer)))
		delivery.unit = deliveryUnitBuffer;
	ImGui::InputDouble((label + " quantity").c_str(), &delivery.quantity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " first occurrence seconds").c_str(),
		&delivery.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recurrence seconds").c_str(),
		&delivery.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::Checkbox((label + " arrives usable").c_str(), &delivery.arrivesQualified);
	ImGui::SameLine();
	ImGui::InputInt((label + " priority").c_str(), &delivery.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeDelivery = true;
	ImGui::PopItemWidth();
	return removeDelivery;
}

bool renderWasteEjectionEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WasteEjectionSettings& ejection, std::string const& label, std::string const& removeLabel)
{
	bool removeEjection = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, ejection.id);
	char ejectionStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(ejectionStoreBuffer, ejection.storeId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " store").c_str(),
		ejectionStoreBuffer, sizeof(ejectionStoreBuffer)))
		ejection.storeId = ejectionStoreBuffer;
	char ejectionUnitBuffer[8]{};
	std::strncpy(ejectionUnitBuffer, ejection.unit.c_str(), sizeof(ejectionUnitBuffer) - 1);
	if (ImGui::InputText((label + " unit").c_str(), ejectionUnitBuffer, sizeof(ejectionUnitBuffer)))
		ejection.unit = ejectionUnitBuffer;
	ImGui::InputDouble((label + " quantity").c_str(), &ejection.quantity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " first occurrence seconds").c_str(),
		&ejection.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recurrence seconds").c_str(),
		&ejection.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	ImGui::InputInt((label + " priority").c_str(), &ejection.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeEjection = true;
	ImGui::PopItemWidth();
	return removeEjection;
}

bool renderOutageEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::InstallationOutageSettings& outage, std::string const& label, std::string const& removeLabel)
{
	bool removeOutage = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, outage.id);
	char outageTargetBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(outageTargetBuffer, outage.installationId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " installation id").c_str(),
		outageTargetBuffer, sizeof(outageTargetBuffer)))
		outage.installationId = outageTargetBuffer;
	ImGui::InputDouble((label + " capacity fraction (0..1)").c_str(),
		&outage.capacityFraction, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " first occurrence seconds").c_str(),
		&outage.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " duration seconds").c_str(),
		&outage.durationSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recurrence seconds (0 = one-off)").c_str(),
		&outage.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputInt((label + " priority").c_str(), &outage.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeOutage = true;
	ImGui::PopItemWidth();
	return removeOutage;
}

bool renderMaintenanceEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::MaintenanceJobSettings& job, std::string const& label, std::string const& removeLabel)
{
	auto const row = label.substr(label.find_last_of(' ') + 1);
	bool removeJob = false;
	ImGui::PushItemWidth(220);
	renderIdentity(label, job.id);
	char jobTargetBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(jobTargetBuffer, job.installationId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " installation id").c_str(),
		jobTargetBuffer, sizeof(jobTargetBuffer)))
		job.installationId = jobTargetBuffer;
	ImGui::InputDouble((label + " first occurrence seconds").c_str(),
		&job.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " duration seconds").c_str(),
		&job.durationSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble((label + " recurrence seconds (0 = one-off)").c_str(),
		&job.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputInt((label + " priority").c_str(), &job.priority);
	char jobWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(jobWasteBuffer, job.wasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText((label + " waste store id").c_str(),
		jobWasteBuffer, sizeof(jobWasteBuffer)))
		job.wasteStoreId = jobWasteBuffer;
	for (size_t requirementIndex = 0; requirementIndex < job.requirements.size(); )
	{
		auto& requirement = job.requirements[requirementIndex];
		auto const requirementRow = row + "." + std::to_string(requirementIndex);
		bool removeRequirement = false;
		char requirementStoreBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(requirementStoreBuffer, requirement.storeId.c_str(),
			core::MaxStationStoreIdLength);
		if (ImGui::InputText(("Maintenance requirement " + requirementRow + " store id").c_str(),
			requirementStoreBuffer, sizeof(requirementStoreBuffer)))
			requirement.storeId = requirementStoreBuffer;
		ImGui::InputDouble(("Maintenance requirement " + requirementRow + " quantity").c_str(),
			&requirement.quantity, 0.0, 0.0, "%.3f");
		ImGui::Checkbox(("Maintenance requirement " + requirementRow
			+ " consumed (unchecked = reusable tool)").c_str(), &requirement.consumed);
		if (job.requirements.size() > 1 && ImGui::Button(("Remove requirement " + requirementRow).c_str()))
			removeRequirement = true;
		if (removeRequirement)
			job.requirements.erase(
				job.requirements.begin() + static_cast<std::ptrdiff_t>(requirementIndex));
		else
			++requirementIndex;
	}
	if (job.requirements.size() < core::MaxStationMaintenanceRequirements
		&& ImGui::Button(("Add requirement to maintenance job " + row).c_str()))
		job.requirements.push_back({});
	ImGui::PopItemWidth();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeJob = true;
	return removeJob;
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

bool renderThermalBufferEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.thermalBuffers.size()) return false;
	return renderThermalBufferEntry(draft, draft.thermalBuffers[index], "Buffer " + std::to_string(index), {});
}

bool renderChillerEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.chillers.size()) return false;
	return renderChillerEntry(draft, draft.chillers[index], "Chiller " + std::to_string(index), {});
}

bool renderCoolantLoopEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.coolantLoops.size()) return false;
	return renderCoolantLoopEntry(draft, draft.coolantLoops[index],
		"Coolant loop " + std::to_string(index), {});
}

bool renderRadiatorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.radiators.size()) return false;
	return renderRadiatorEntry(draft, draft.radiators[index],
		"Radiator " + std::to_string(index), {});
}

bool renderWaterCollectorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.waterCollectors.size()) return false;
	return renderWaterCollectorEntry(draft, draft.waterCollectors[index], "Collector " + std::to_string(index), {});
}

bool renderWaterTreatmentEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.waterTreatments.size()) return false;
	return renderWaterTreatmentEntry(draft, draft.waterTreatments[index], "Treatment " + std::to_string(index), {});
}

bool renderElectrolyzerEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.electrolyzers.size()) return false;
	return renderElectrolyzerEntry(draft, draft.electrolyzers[index], "Electrolyzer " + std::to_string(index), {});
}

bool renderCo2CaptureEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.co2Captures.size()) return false;
	return renderCo2CaptureEntry(draft, draft.co2Captures[index], "CO2 capture " + std::to_string(index), {});
}

bool renderAtmosphereEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.atmospheres.size()) return false;
	return renderAtmosphereEntry(draft, draft.atmospheres[index], "Atmosphere " + std::to_string(index), {});
}

bool renderAirlockCycleEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.airlockCycles.size()) return false;
	return renderAirlockCycleEntry(draft, draft.airlockCycles[index], "Airlock cycle " + std::to_string(index), {});
}

bool renderCrewEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.crews.size()) return false;
	return renderCrewEntry(draft, draft.crews[index], "Crew " + std::to_string(index), {});
}

bool renderMicrobialProcessorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.microbialProcessors.size()) return false;
	return renderMicrobialProcessorEntry(draft, draft.microbialProcessors[index], "Microbial processor " + std::to_string(index), {});
}

bool renderCropPopulationEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.cropPopulations.size()) return false;
	return renderCropPopulationEntry(draft, draft.cropPopulations[index], "Crop " + std::to_string(index), {});
}

bool renderAlgaeCultureEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.algaeCultures.size()) return false;
	return renderAlgaeCultureEntry(draft, draft.algaeCultures[index], "Algae " + std::to_string(index), {});
}

bool renderDeliveryEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.deliveries.size()) return false;
	return renderDeliveryEntry(draft, draft.deliveries[index], "Delivery " + std::to_string(index), {});
}
bool renderWasteEjectionEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.wasteEjections.size()) return false;
	return renderWasteEjectionEntry(draft, draft.wasteEjections[index], "Waste ejection " + std::to_string(index), {});
}
bool renderOutageEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.outages.size()) return false;
	return renderOutageEntry(draft, draft.outages[index], "Outage " + std::to_string(index), {});
}
bool renderMaintenanceEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.maintenanceJobs.size()) return false;
	return renderMaintenanceEntry(draft, draft.maintenanceJobs[index], "Maintenance job " + std::to_string(index), {});
}

bool renderSolarEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.solarArrays.size()) return false;
	return renderSolarEntry(draft, draft.solarArrays[index], "Solar " + std::to_string(index), {});
}

bool renderConversionEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.conversions.size()) return false;
	return renderConversionEntry(draft, draft.conversions[index], "Conversion " + std::to_string(index), {});
}

// Keyed by the Flow node type names in core::stationPortDescriptors().
constexpr std::array<StationEntryEditor, 29> kEntryEditors{{
	{"Solar", &renderSolarEntryAt},
	{"Conversion", &renderConversionEntryAt},
	{"Delivery", &renderDeliveryEntryAt},
	{"Waste ejection", &renderWasteEjectionEntryAt},
	{"Outage", &renderOutageEntryAt},
	{"Maintenance", &renderMaintenanceEntryAt},
	{"Store", &renderStoreEntryAt},
	{"Crew", &renderCrewEntryAt},
	{"Crops", &renderCropPopulationEntryAt},
	{"Algae", &renderAlgaeCultureEntryAt},
	{"Microbial processor", &renderMicrobialProcessorEntryAt},
	{"Water collector", &renderWaterCollectorEntryAt},
	{"Water treatment", &renderWaterTreatmentEntryAt},
	{"Electrolyzer", &renderElectrolyzerEntryAt},
	{"CO2 capture", &renderCo2CaptureEntryAt},
	{"Atmosphere", &renderAtmosphereEntryAt},
	{"Airlock cycle", &renderAirlockCycleEntryAt},

	{"Transfer", &renderTransferEntryAt},
	{"Equipment", &renderEquipmentLoadEntryAt},
	{"Battery", &renderBatteryEntryAt},
	{"Generator", &renderGeneratorEntryAt},
	{"Fuel cell", &renderFuelCellEntryAt},
	{"Sabatier", &renderSabatierEntryAt},
	{"Methane processor", &renderMethaneProcessorEntryAt},
	{"Reservoir", &renderThermalReservoirEntryAt},
	{"Radiator", &renderRadiatorEntryAt},
	{"Coolant loop", &renderCoolantLoopEntryAt},
	{"Chiller", &renderChillerEntryAt},
	{"Thermal buffer", &renderThermalBufferEntryAt},
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
