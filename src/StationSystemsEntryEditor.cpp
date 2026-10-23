#include "StationSystemsEntryEditor.h"
#include "core/StationPorts.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <string>
#include <string_view>

namespace station_editor
{
namespace
{
// The Flow inspector shows one node at a time, so a field's visible
// description drops the redundant "<entry> " prefix. The '##' suffix keeps
// that prefix in the label string rather than in the rendered text: ImGui
// draws only what precedes '##' (RenderText hides the rest), while the
// ImGui ID and the full name the headless Test Engine addresses still carry
// the entry. That keeps a field unambiguous on the form surfaces, which
// render every entry of a type at once, without showing the prefix twice.
std::string describe(std::string_view field, std::string const& label)
{
	return std::string(field) + "##" + label;
}
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
	ImGui::InputText(describe("id", label).c_str(), buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("To rename, select this node in Flow and use 'New node name' / 'Rename selected node'.");
}
}

bool renderConversionEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::ResourceConversionSettings& conversion,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeConversion = false;
	auto const row = label.substr(label.find_last_of(' ') + 1);
	renderIdentity(label, conversion.id);
	ImGui::InputInt(describe("priority", label).c_str(), &conversion.priority);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeConversion = true;
	for (size_t flow = 0; flow < conversion.inputs.size(); )
	{
		auto& input = conversion.inputs[flow];
		auto const flowRow = std::string("input ") + std::to_string(flow);
		char storeBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(storeBuffer, input.storeId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(describe(flowRow + " store", label).c_str(), storeBuffer, sizeof(storeBuffer)))
			input.storeId = storeBuffer;
		ImGui::SameLine();
		ImGui::InputDouble(describe(flowRow + " rate", label).c_str(), &input.ratePerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		bool const removeInput = ImGui::Button(describe("Remove " + flowRow, label).c_str());
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
		auto const flowRow = std::string("output ") + std::to_string(flow);
		char storeBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(storeBuffer, output.storeId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(describe(flowRow + " store", label).c_str(), storeBuffer, sizeof(storeBuffer)))
			output.storeId = storeBuffer;
		ImGui::SameLine();
		ImGui::InputDouble(describe(flowRow + " yield", label).c_str(), &output.yieldPerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		bool const removeOutput = ImGui::Button(describe("Remove " + flowRow, label).c_str());
		if (removeOutput)
			conversion.outputs.erase(conversion.outputs.begin() + static_cast<std::ptrdiff_t>(flow));
		else
			++flow;
	}
	if (conversion.outputs.size() < core::MaxStationConversionFlows
		&& ImGui::Button(("Add output to conversion " + row).c_str()))
		conversion.outputs.push_back({});
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeConversion;
}

bool renderSolarEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::SolarArraySettings& array,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeArray = false;
	auto const row = label.substr(label.find_last_of(' ') + 1);
	renderIdentity(label, array.id);
	ImGui::InputDouble(describe("capacity kW", label).c_str(), &array.capacityKW, 0.0, 0.0, "%.3f");
	bool solarPeriodic = array.illumination.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic illumination", label).c_str(), &solarPeriodic))
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
		ImGui::InputDouble(describe("constant illumination", label).c_str(),
			&array.illumination.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble(describe("period seconds", label).c_str(),
			&array.illumination.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < array.illumination.segments.size(); )
		{
			auto& stretch = array.illumination.segments[segment];
			auto const segmentRow = std::string("segment ") + std::to_string(segment);
			ImGui::InputDouble(describe(segmentRow + " start", label).c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble(describe(segmentRow + " level", label).c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			bool const removeSegment = ImGui::Button(describe("Remove " + segmentRow, label).c_str());
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
	ImGui::PopID();
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
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeCrew = false;
	renderIdentity(label, crew.id);
	renderWaterQuality(crew.waterQuality, label);
	char greywaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(greywaterBuffer, crew.greywaterStoreId.c_str(), sizeof(greywaterBuffer) - 1);
	if (ImGui::InputText(describe("greywater store", label).c_str(), greywaterBuffer, sizeof(greywaterBuffer)))
		crew.greywaterStoreId = greywaterBuffer;
	ImGui::InputDouble(describe("greywater kg per crew-day", label).c_str(), &crew.greywaterKgPerCrewDay, 0, 0, "%.6f");
	for (auto const* name : {"dryOrganics", "nutrientNitrogen"})
	{
		bool enabled = crew.greywaterLoadKgPerCrewDay.contains(name);
		if (ImGui::Checkbox((std::string("greywater load ") + name).c_str(), &enabled))
		{ if (enabled) crew.greywaterLoadKgPerCrewDay[name] = 0; else crew.greywaterLoadKgPerCrewDay.erase(name); }
		if (enabled) ImGui::InputDouble((std::string("") + name + " kg per crew-day").c_str(),
			&crew.greywaterLoadKgPerCrewDay.at(name), 0, 0, "%.6f");
	}
	char crewAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewAtmosphereBuffer, crew.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("atmosphere", label).c_str(),
		crewAtmosphereBuffer, sizeof(crewAtmosphereBuffer)))
		crew.atmosphereId = crewAtmosphereBuffer;
	ImGui::InputInt(describe("count", label).c_str(), &crew.crewCount, 1, 0);
	bool crewPeriodic = crew.activityProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic activity", label).c_str(), &crewPeriodic))
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
		ImGui::InputDouble(describe("constant activity level", label).c_str(),
			&crew.activityProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble(describe("activity period seconds", label).c_str(),
			&crew.activityProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < crew.activityProfile.segments.size(); ++segment)
		{
			auto& stretch = crew.activityProfile.segments[segment];
			auto const segmentRow = std::string("activity segment ") + std::to_string(segment);
			ImGui::InputDouble(describe(segmentRow + " start", label).c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble(describe(segmentRow + " level", label).c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
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
	if (ImGui::InputText(describe("food store", label).c_str(),
		crewFoodBuffer, sizeof(crewFoodBuffer)))
		crew.foodStoreId = crewFoodBuffer;
	char crewWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewWaterBuffer, crew.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("water store", label).c_str(),
		crewWaterBuffer, sizeof(crewWaterBuffer)))
		crew.waterStoreId = crewWaterBuffer;
	char crewUrineBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewUrineBuffer, crew.urineStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("urine store", label).c_str(),
		crewUrineBuffer, sizeof(crewUrineBuffer)))
		crew.urineStoreId = crewUrineBuffer;
	char crewWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(crewWasteBuffer, crew.organicWasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("organic waste store", label).c_str(),
		crewWasteBuffer, sizeof(crewWasteBuffer)))
		crew.organicWasteStoreId = crewWasteBuffer;
	ImGui::InputDouble(describe("food per crew per s", label).c_str(),
		&crew.foodPerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("water per crew per s", label).c_str(),
		&crew.waterPerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("O2 per crew kg/s", label).c_str(),
		&crew.oxygenPerCrewKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("CO2 per crew kg/s", label).c_str(),
		&crew.co2PerCrewKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("moisture per crew kg/s", label).c_str(),
		&crew.moisturePerCrewKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("urine per crew per s", label).c_str(),
		&crew.urinePerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("organic waste per crew per s", label).c_str(),
		&crew.organicWastePerCrewPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("metabolic heat per crew kW", label).c_str(),
		&crew.metabolicHeatPerCrewKW, 0.0, 0.0, "%.4f");
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeCrew = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeCrew;
}

bool renderMicrobialProcessorEntry(core::StationSystemsSettings& draft, core::MicrobialProcessorSettings& processor,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeProcessor = false;
	renderIdentity(label, processor.id);
	renderWaterQuality(processor.waterQuality, label);
	for (auto const* name : core::StationWaterDryConstituents)
	{
		bool enabled = processor.recoveredWaterCarryoverFractions.contains(name);
		if (ImGui::Checkbox((std::string("carry to recovered water: ") + name).c_str(), &enabled))
		{ if (enabled) processor.recoveredWaterCarryoverFractions[name] = 0; else processor.recoveredWaterCarryoverFractions.erase(name); }
		if (enabled) ImGui::InputDouble((std::string("") + name + " carry-over fraction").c_str(),
			&processor.recoveredWaterCarryoverFractions.at(name), 0, 0, "%.6f");
	}
	char processorFeedBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorFeedBuffer, processor.feedStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("organic waste feed store", label).c_str(),
		processorFeedBuffer, sizeof(processorFeedBuffer)))
		processor.feedStoreId = processorFeedBuffer;
	char processorOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorOxygenBuffer, processor.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("oxygen store", label).c_str(),
		processorOxygenBuffer, sizeof(processorOxygenBuffer)))
		processor.oxygenStoreId = processorOxygenBuffer;
	char processorWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorWaterBuffer, processor.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("process water store", label).c_str(),
		processorWaterBuffer, sizeof(processorWaterBuffer)))
		processor.waterStoreId = processorWaterBuffer;
	char processorMediaBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorMediaBuffer, processor.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("culture medium store", label).c_str(),
		processorMediaBuffer, sizeof(processorMediaBuffer)))
		processor.consumableStoreId = processorMediaBuffer;
	char processorNutrientBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorNutrientBuffer, processor.nutrientStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("recovered nutrient store", label).c_str(),
		processorNutrientBuffer, sizeof(processorNutrientBuffer)))
		processor.nutrientStoreId = processorNutrientBuffer;
	char processorRecoveredWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorRecoveredWaterBuffer, processor.recoveredWaterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("recovered water store", label).c_str(),
		processorRecoveredWaterBuffer, sizeof(processorRecoveredWaterBuffer)))
		processor.recoveredWaterStoreId = processorRecoveredWaterBuffer;
	char processorCo2Buffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorCo2Buffer, processor.co2StoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("CO2 store", label).c_str(),
		processorCo2Buffer, sizeof(processorCo2Buffer)))
		processor.co2StoreId = processorCo2Buffer;
	char processorResidueBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorResidueBuffer, processor.residueStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("residue store", label).c_str(),
		processorResidueBuffer, sizeof(processorResidueBuffer)))
		processor.residueStoreId = processorResidueBuffer;
	char processorStockBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorStockBuffer, processor.stockStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("replacement biological stock store", label).c_str(),
		processorStockBuffer, sizeof(processorStockBuffer)))
		processor.stockStoreId = processorStockBuffer;
	char processorThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorThermalBuffer, processor.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink thermal class", label).c_str(),
		processorThermalBuffer, sizeof(processorThermalBuffer)))
		processor.thermalClass = processorThermalBuffer;
	char processorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorSinkBuffer, processor.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink reservoir", label).c_str(),
		processorSinkBuffer, sizeof(processorSinkBuffer)))
		processor.heatSinkId = processorSinkBuffer;
	ImGui::InputDouble(describe("rated feed processing kg/s", label).c_str(),
		&processor.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("specific energy kWh/kg feed", label).c_str(),
		&processor.specificEnergyKWhPerKgFeed, 0.0, 0.0, "%.4f");
	int basis = static_cast<int>(processor.rateBasis);
	if (ImGui::Combo(describe("rate basis", label).c_str(), &basis, "Fixed per feed mass\0Composition derived\0"))
		processor.rateBasis = static_cast<core::StationRateBasis>(basis);
	if (processor.rateBasis == core::StationRateBasis::FixedPerFeedMass)
	{
		ImGui::InputDouble(describe("oxygen per kg feed kg", label).c_str(),
			&processor.oxygenPerKgFeedKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("process water per kg feed kg", label).c_str(),
			&processor.processWaterPerKgFeedKg, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("medium units per kg feed", label).c_str(),
			&processor.consumablePerKgFeed, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("CO2 per kg feed kg", label).c_str(),
			&processor.co2PerKgFeedKg, 0.0, 0.0, "%.4f");
	}
	else
	{
		ImGui::InputDouble(describe("organic oxidation fraction", label).c_str(), &processor.organicOxidationFraction);
		ImGui::InputDouble(describe("O2 per kg organic oxidized", label).c_str(), &processor.o2PerKgOrganicOxidized);
		ImGui::InputDouble(describe("nitrification fraction", label).c_str(), &processor.nitrificationFraction);
		ImGui::InputDouble(describe("O2 per kg nitrified N", label).c_str(), &processor.o2PerKgNitrifiedN);
		ImGui::InputDouble(describe("CO2 per kg organic oxidized", label).c_str(), &processor.co2PerKgOrganicOxidized);
		ImGui::InputDouble(describe("process water target moisture", label).c_str(), &processor.processWaterTargetMoisture);
		ImGui::InputDouble(describe("heat release fraction", label).c_str(), &processor.heatReleaseFraction);
		ImGui::InputDouble(describe("medium kg per kg organic oxidized", label).c_str(), &processor.consumablePerKgOrganicOxidized);
	}
	ImGui::InputDouble(describe("nitrogen recovery", label).c_str(),
		&processor.nitrogenRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("phosphorus recovery", label).c_str(),
		&processor.phosphorusRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("potassium recovery", label).c_str(),
		&processor.potassiumRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("minerals recovery", label).c_str(),
		&processor.mineralsRecovery, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("water recovery", label).c_str(),
		&processor.waterRecovery, 0.0, 0.0, "%.4f");
	if (processor.rateBasis == core::StationRateBasis::FixedPerFeedMass)
		ImGui::InputDouble(describe("reaction heat kJ/kg feed", label).c_str(),
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
	ImGui::InputDouble(describe("min operating temperature C", label).c_str(),
		&processor.minOperatingTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble(describe("max sink temperature C", label).c_str(),
		&processor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble(describe("initial condition", label).c_str(),
		&processor.initialCondition, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("starvation degrade per hour", label).c_str(),
		&processor.starvationDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("consumable exhaustion degrade per hour", label).c_str(),
		&processor.consumableExhaustionDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("cold stress degrade per hour", label).c_str(),
		&processor.coldStressDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("heat stress degrade per hour", label).c_str(),
		&processor.heatStressDegradePerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("recovery per hour", label).c_str(),
		&processor.recoveryPerHour, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("stock units per condition point", label).c_str(),
		&processor.stockUnitsPerConditionPoint, 0.0, 0.0, "%.4f");
	ImGui::InputInt(describe("priority", label).c_str(), &processor.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeProcessor = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeProcessor;
}

bool renderCropPopulationEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::CropPopulationSettings& crop,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeCrop = false;
	renderIdentity(label, crop.id);
	renderWaterQuality(crop.waterQuality, label);
	bool selectiveIrrigation = !crop.irrigationUptakeFractions.empty();
	if (ImGui::Checkbox(describe("selective irrigation", label).c_str(), &selectiveIrrigation))
	{
		if (selectiveIrrigation) crop.irrigationUptakeFractions = {{"nutrientNitrogen", 0}, {"phosphorus", 0}, {"potassium", 0}};
		else crop.irrigationUptakeFractions.clear();
	}
	if (selectiveIrrigation)
		for (auto const* name : {"nutrientNitrogen", "phosphorus", "potassium"})
		{
			auto const found = crop.irrigationUptakeFractions.find(name);
			double fraction = found == crop.irrigationUptakeFractions.end() ? 0 : found->second;
			if (ImGui::InputDouble((std::string("irrigation uptake ") + name).c_str(), &fraction, 0, 0, "%.6f"))
				crop.irrigationUptakeFractions[name] = fraction;
		}
	char cropAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropAtmosphereBuffer, crop.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("atmosphere", label).c_str(),
		cropAtmosphereBuffer, sizeof(cropAtmosphereBuffer)))
		crop.atmosphereId = cropAtmosphereBuffer;
	char cropWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropWaterBuffer, crop.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("irrigation water store", label).c_str(),
		cropWaterBuffer, sizeof(cropWaterBuffer)))
		crop.waterStoreId = cropWaterBuffer;
	char cropNitrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropNitrogenBuffer, crop.nitrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("nitrogen store", label).c_str(),
		cropNitrogenBuffer, sizeof(cropNitrogenBuffer)))
		crop.nitrogenStoreId = cropNitrogenBuffer;
	char cropPhosphorusBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropPhosphorusBuffer, crop.phosphorusStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("phosphorus store", label).c_str(),
		cropPhosphorusBuffer, sizeof(cropPhosphorusBuffer)))
		crop.phosphorusStoreId = cropPhosphorusBuffer;
	char cropPotassiumBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropPotassiumBuffer, crop.potassiumStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("potassium store", label).c_str(),
		cropPotassiumBuffer, sizeof(cropPotassiumBuffer)))
		crop.potassiumStoreId = cropPotassiumBuffer;
	char cropMineralsBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropMineralsBuffer, crop.mineralsStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("minerals store", label).c_str(),
		cropMineralsBuffer, sizeof(cropMineralsBuffer)))
		crop.mineralsStoreId = cropMineralsBuffer;
	// #581: the capacity-factor fields use %g so a large authored
	// value round-trips through the 64-character InputScalar buffer.
	// With %f a value above ~1e62 truncated to the buffer width and
	// re-committing it silently changed the number; %g keeps every
	// magnitude - including the overflow candidates the capacity
	// contract now refuses - displayable and editable exactly.
	ImGui::InputDouble(describe("growing area m2", label).c_str(),
		&crop.growingAreaM2, 0.0, 0.0, "%.15g");
	ImGui::InputDouble(describe("max biomass density kg/m2", label).c_str(),
		&crop.maxBiomassDensityKgPerM2, 0.0, 0.0, "%.15g");
	ImGui::InputDouble(describe("initial dry biomass kg", label).c_str(),
		&crop.initialBiomassKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("lamp rated kW", label).c_str(),
		&crop.lampRatedKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("circulation power kW (drawn before lamp)", label).c_str(),
		&crop.circulationPowerKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("growth yield kg per kWh light", label).c_str(),
		&crop.growthYieldKgPerKWh, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("respiration per hour", label).c_str(),
		&crop.respirationPerHour, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("transpiration kg per kg growth", label).c_str(),
		&crop.transpirationKgPerKgGrowth, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("min growing temperature C", label).c_str(),
		&crop.minGrowingTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble(describe("max growing temperature C", label).c_str(),
		&crop.maxGrowingTemperatureC, 0.0, 0.0, "%.2f");
	char cropFoodBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropFoodBuffer, crop.foodStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("food store", label).c_str(),
		cropFoodBuffer, sizeof(cropFoodBuffer)))
		crop.foodStoreId = cropFoodBuffer;
	char cropWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cropWasteBuffer, crop.wasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("waste store", label).c_str(),
		cropWasteBuffer, sizeof(cropWasteBuffer)))
		crop.wasteStoreId = cropWasteBuffer;
	ImGui::InputDouble(describe("harvest trigger kg (0 disables harvest)", label).c_str(),
		&crop.harvestTriggerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("harvest fraction of living biomass", label).c_str(),
		&crop.harvestFraction, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("edible fraction of harvest", label).c_str(),
		&crop.edibleFraction, 0.0, 0.0, "%.4f");
	bool cropPeriodic = crop.illuminationProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic illumination", label).c_str(), &cropPeriodic))
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
		ImGui::InputDouble(describe("constant illumination level", label).c_str(),
			&crop.illuminationProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble(describe("illumination period seconds", label).c_str(),
			&crop.illuminationProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < crop.illuminationProfile.segments.size(); ++segment)
		{
			auto& stretch = crop.illuminationProfile.segments[segment];
			auto const segmentRow = std::string("illumination segment ") + std::to_string(segment);
			ImGui::InputDouble(describe(segmentRow + " start", label).c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble(describe(segmentRow + " level", label).c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				crop.illuminationProfile.segments.erase(
					crop.illuminationProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (crop.illuminationProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add illumination segment to " + label).c_str()))
			crop.illuminationProfile.segments.push_back(
				{crop.illuminationProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::Text("  declared dry-mass composition (moisture and atmospheric nitrogen must be 0 for crops):");
	ImGui::InputDouble(describe("composition moisture fraction", label).c_str(),
		&crop.composition.moisture, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition nutrient nitrogen fraction", label).c_str(),
		&crop.composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition atmospheric nitrogen fraction", label).c_str(),
		&crop.composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition phosphorus fraction", label).c_str(),
		&crop.composition.phosphorus, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition potassium fraction", label).c_str(),
		&crop.composition.potassium, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition other minerals fraction", label).c_str(),
		&crop.composition.otherMinerals, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition energy kJ per kg dry", label).c_str(),
		&crop.composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	ImGui::InputInt(describe("priority", label).c_str(), &crop.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeCrop = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeCrop;
}

bool renderAlgaeCultureEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::AlgaeCultureSettings& algae,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeAlgae = false;
	renderIdentity(label, algae.id);
	renderWaterQuality(algae.waterQuality, label);
	char algaeAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeAtmosphereBuffer, algae.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("atmosphere", label).c_str(),
		algaeAtmosphereBuffer, sizeof(algaeAtmosphereBuffer)))
		algae.atmosphereId = algaeAtmosphereBuffer;
	char algaeWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeWaterBuffer, algae.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("culture water store", label).c_str(),
		algaeWaterBuffer, sizeof(algaeWaterBuffer)))
		algae.waterStoreId = algaeWaterBuffer;
	char algaeNitrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeNitrogenBuffer, algae.nitrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("nitrogen store", label).c_str(),
		algaeNitrogenBuffer, sizeof(algaeNitrogenBuffer)))
		algae.nitrogenStoreId = algaeNitrogenBuffer;
	char algaePhosphorusBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaePhosphorusBuffer, algae.phosphorusStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("phosphorus store", label).c_str(),
		algaePhosphorusBuffer, sizeof(algaePhosphorusBuffer)))
		algae.phosphorusStoreId = algaePhosphorusBuffer;
	char algaePotassiumBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaePotassiumBuffer, algae.potassiumStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("potassium store", label).c_str(),
		algaePotassiumBuffer, sizeof(algaePotassiumBuffer)))
		algae.potassiumStoreId = algaePotassiumBuffer;
	char algaeMineralsBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeMineralsBuffer, algae.mineralsStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("minerals store", label).c_str(),
		algaeMineralsBuffer, sizeof(algaeMineralsBuffer)))
		algae.mineralsStoreId = algaeMineralsBuffer;
	ImGui::InputDouble(describe("active culture volume L", label).c_str(),
		&algae.cultureVolumeL, 0.0, 0.0, "%.15g");
	ImGui::InputDouble(describe("max biomass density kg/L", label).c_str(),
		&algae.maxBiomassDensityKgPerL, 0.0, 0.0, "%.15g");
	ImGui::InputDouble(describe("initial dry biomass kg", label).c_str(),
		&algae.initialBiomassKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("lamp rated kW", label).c_str(),
		&algae.lampRatedKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("circulation power kW (drawn before lamp)", label).c_str(),
		&algae.circulationPowerKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("growth yield kg per kWh light", label).c_str(),
		&algae.growthYieldKgPerKWh, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("respiration per hour", label).c_str(),
		&algae.respirationPerHour, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("evaporation kg per kg growth", label).c_str(),
		&algae.evaporationKgPerKgGrowth, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("min growing temperature C", label).c_str(),
		&algae.minGrowingTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputDouble(describe("max growing temperature C", label).c_str(),
		&algae.maxGrowingTemperatureC, 0.0, 0.0, "%.2f");
	char algaeFoodBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeFoodBuffer, algae.foodStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("food store", label).c_str(),
		algaeFoodBuffer, sizeof(algaeFoodBuffer)))
		algae.foodStoreId = algaeFoodBuffer;
	char algaeRecoveryBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeRecoveryBuffer, algae.recoveryStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("recovery store", label).c_str(),
		algaeRecoveryBuffer, sizeof(algaeRecoveryBuffer)))
		algae.recoveryStoreId = algaeRecoveryBuffer;
	char algaeWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(algaeWasteBuffer, algae.wasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("waste store", label).c_str(),
		algaeWasteBuffer, sizeof(algaeWasteBuffer)))
		algae.wasteStoreId = algaeWasteBuffer;
	ImGui::InputDouble(describe("harvest trigger kg (0 disables harvest)", label).c_str(),
		&algae.harvestTriggerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("harvest fraction of living biomass", label).c_str(),
		&algae.harvestFraction, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("food fraction of harvest", label).c_str(),
		&algae.foodFraction, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("recovery fraction of harvest", label).c_str(),
		&algae.recoveryFraction, 0.0, 0.0, "%.4f");
	bool algaePeriodic = algae.illuminationProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic illumination", label).c_str(), &algaePeriodic))
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
		ImGui::InputDouble(describe("constant illumination level", label).c_str(),
			&algae.illuminationProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble(describe("illumination period seconds", label).c_str(),
			&algae.illuminationProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < algae.illuminationProfile.segments.size(); ++segment)
		{
			auto& stretch = algae.illuminationProfile.segments[segment];
			auto const segmentRow = std::string("illumination segment ") + std::to_string(segment);
			ImGui::InputDouble(describe(segmentRow + " start", label).c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble(describe(segmentRow + " level", label).c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				algae.illuminationProfile.segments.erase(
					algae.illuminationProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (algae.illuminationProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add illumination segment to " + label).c_str()))
			algae.illuminationProfile.segments.push_back(
				{algae.illuminationProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::Text("  declared dry-mass composition (moisture and atmospheric nitrogen must be 0 for cultures):");
	ImGui::InputDouble(describe("composition moisture fraction", label).c_str(),
		&algae.composition.moisture, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition nutrient nitrogen fraction", label).c_str(),
		&algae.composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition atmospheric nitrogen fraction", label).c_str(),
		&algae.composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition phosphorus fraction", label).c_str(),
		&algae.composition.phosphorus, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition potassium fraction", label).c_str(),
		&algae.composition.potassium, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition other minerals fraction", label).c_str(),
		&algae.composition.otherMinerals, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("composition energy kJ per kg dry", label).c_str(),
		&algae.composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	ImGui::InputInt(describe("priority", label).c_str(), &algae.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeAlgae = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeAlgae;
}

bool renderStoreEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceStoreSettings& store,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, store.id);
	char materialBuffer[core::MaxStationMaterialIdLength + 1]{};
	std::strncpy(materialBuffer, store.materialId.c_str(), sizeof(materialBuffer) - 1);
	if (ImGui::InputText(describe("material identity", label).c_str(), materialBuffer, sizeof(materialBuffer)))
		store.materialId = materialBuffer;
	if (store.materialId.empty()) ImGui::TextUnformatted("Material: Unclassified");
	else if (!core::isValidStationMaterialId(store.materialId))
		ImGui::TextUnformatted("Invalid material identity: use lowercase words/digits separated by single hyphens.");
	else ImGui::Text("Material: %s (explicit)", store.materialId.c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material identity is independent of store name, unit and composition. Empty is unclassified; it never changes allocation.");
	char unitBuffer[8]{};
	std::strncpy(unitBuffer, store.unit.c_str(), sizeof(unitBuffer) - 1);
	if (ImGui::InputText(describe("unit", label).c_str(), unitBuffer, sizeof(unitBuffer)))
		store.unit = unitBuffer;
	ImGui::InputDouble(describe("capacity", label).c_str(), &store.capacity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting usable", label).c_str(), &store.startingQualified, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting held", label).c_str(), &store.startingUnqualified, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("protected reserve", label).c_str(), &store.protectedReserve, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	bool compound = store.composition.has_value();
	if (ImGui::Checkbox(describe("declared composition", label).c_str(), &compound))
	{
		if (compound && !store.composition) store.composition = core::ResourceCompositionSettings{};
		else if (!compound) store.composition.reset();
	}
	if (store.composition)
	{
		auto& composition = *store.composition;
		ImGui::InputDouble(describe("moisture fraction", label).c_str(), &composition.moisture, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("nutrient nitrogen fraction", label).c_str(), &composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("atmospheric nitrogen fraction", label).c_str(), &composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("phosphorus fraction", label).c_str(), &composition.phosphorus, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("potassium fraction", label).c_str(), &composition.potassium, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("other minerals fraction", label).c_str(), &composition.otherMinerals, 0.0, 0.0, "%.4f");
		ImGui::InputDouble(describe("energy kJ per kg dry", label).c_str(), &composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	}
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderWaterCollectorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WaterCollectorSettings& collector, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeCollector = false;
	renderIdentity(label, collector.id);
	char collectorAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorAtmosphereBuffer, collector.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("atmosphere", label).c_str(),
		collectorAtmosphereBuffer, sizeof(collectorAtmosphereBuffer)))
		collector.atmosphereId = collectorAtmosphereBuffer;
	char collectorLoopBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorLoopBuffer, collector.coolingLoopId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("cooling loop", label).c_str(),
		collectorLoopBuffer, sizeof(collectorLoopBuffer)))
		collector.coolingLoopId = collectorLoopBuffer;
	char collectorClassBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorClassBuffer, collector.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("thermal class", label).c_str(),
		collectorClassBuffer, sizeof(collectorClassBuffer)))
		collector.thermalClass = collectorClassBuffer;
	char collectorDestBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(collectorDestBuffer, collector.destStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("raw water store", label).c_str(),
		collectorDestBuffer, sizeof(collectorDestBuffer)))
		collector.destStoreId = collectorDestBuffer;
	ImGui::InputDouble(describe("max rate kg/s", label).c_str(),
		&collector.maxRateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("target RH", label).c_str(),
		&collector.targetRelativeHumidity, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCollector = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeCollector;
}

bool renderWaterTreatmentEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WaterTreatmentSettings& treatment, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeTreatment = false;
	renderIdentity(label, treatment.id);
	char treatmentSourceBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentSourceBuffer, treatment.sourceStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("raw water store", label).c_str(),
		treatmentSourceBuffer, sizeof(treatmentSourceBuffer)))
		treatment.sourceStoreId = treatmentSourceBuffer;
	char treatmentTreatedBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentTreatedBuffer, treatment.treatedStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("treated store", label).c_str(),
		treatmentTreatedBuffer, sizeof(treatmentTreatedBuffer)))
		treatment.treatedStoreId = treatmentTreatedBuffer;
	char treatmentConcentrateBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentConcentrateBuffer, treatment.concentrateStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("concentrate store", label).c_str(),
		treatmentConcentrateBuffer, sizeof(treatmentConcentrateBuffer)))
		treatment.concentrateStoreId = treatmentConcentrateBuffer;
	char treatmentConsumableBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(treatmentConsumableBuffer, treatment.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("consumable store", label).c_str(),
		treatmentConsumableBuffer, sizeof(treatmentConsumableBuffer)))
		treatment.consumableStoreId = treatmentConsumableBuffer;
	ImGui::InputDouble(describe("throughput kg/s", label).c_str(),
		&treatment.throughputKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("recovery ratio", label).c_str(),
		&treatment.recoveryRatio, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("specific energy kWh/kg", label).c_str(),
		&treatment.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("consumable per kg", label).c_str(),
		&treatment.consumablePerKg, 0.0, 0.0, "%.6f");
	ImGui::TextUnformatted("Removal fractions (default: all removed)");
	for (auto const* name : core::StationWaterDryConstituents)
	{
		auto found = treatment.removalFractions.find(name);
		bool authored = found != treatment.removalFractions.end();
		if (ImGui::Checkbox((std::string("override ") + name).c_str(), &authored))
		{
			if (authored) treatment.removalFractions[name] = 1.0;
			else treatment.removalFractions.erase(name);
		}
		if (authored)
			ImGui::InputDouble((std::string("removal ") + name).c_str(),
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
	ImGui::InputInt(describe("priority", label).c_str(), &treatment.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeTreatment = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeTreatment;
}

bool renderElectrolyzerEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ElectrolyzerSettings& cell, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeCell = false;
	renderIdentity(label, cell.id);
	char cellWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellWaterBuffer, cell.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("feed water store", label).c_str(),
		cellWaterBuffer, sizeof(cellWaterBuffer)))
		cell.waterStoreId = cellWaterBuffer;
	char cellOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellOxygenBuffer, cell.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("oxygen store (or leave empty for the compartment)", label).c_str(),
		cellOxygenBuffer, sizeof(cellOxygenBuffer)))
		cell.oxygenStoreId = cellOxygenBuffer;
	char cellHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellHydrogenBuffer, cell.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("hydrogen store", label).c_str(),
		cellHydrogenBuffer, sizeof(cellHydrogenBuffer)))
		cell.hydrogenStoreId = cellHydrogenBuffer;
	char cellAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellAtmosphereBuffer, cell.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("delivery compartment (or leave empty for the store)", label).c_str(),
		cellAtmosphereBuffer, sizeof(cellAtmosphereBuffer)))
		cell.atmosphereId = cellAtmosphereBuffer;
	char cellThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellThermalBuffer, cell.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink thermal class", label).c_str(),
		cellThermalBuffer, sizeof(cellThermalBuffer)))
		cell.thermalClass = cellThermalBuffer;
	char cellSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellSinkBuffer, cell.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink reservoir", label).c_str(),
		cellSinkBuffer, sizeof(cellSinkBuffer)))
		cell.heatSinkId = cellSinkBuffer;
	ImGui::InputDouble(describe("rated split kg/s", label).c_str(),
		&cell.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("specific energy kWh/kg water", label).c_str(),
		&cell.specificEnergyKWhPerKgWater, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("max sink temperature C", label).c_str(),
		&cell.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt(describe("priority", label).c_str(), &cell.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCell = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeCell;
}

bool renderCo2CaptureEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::Co2CaptureSettings& capture, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeCapture = false;
	renderIdentity(label, capture.id);
	char captureAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureAtmosphereBuffer, capture.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("compartment", label).c_str(),
		captureAtmosphereBuffer, sizeof(captureAtmosphereBuffer)))
		capture.atmosphereId = captureAtmosphereBuffer;
	char captureStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureStoreBuffer, capture.captureStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("capture store", label).c_str(),
		captureStoreBuffer, sizeof(captureStoreBuffer)))
		capture.captureStoreId = captureStoreBuffer;
	char captureConsumableBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(captureConsumableBuffer, capture.consumableStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("consumable store (empty: none)", label).c_str(),
		captureConsumableBuffer, sizeof(captureConsumableBuffer)))
		capture.consumableStoreId = captureConsumableBuffer;
	ImGui::InputDouble(describe("max rate kg/s", label).c_str(),
		&capture.maxRateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("specific energy kWh/kg", label).c_str(),
		&capture.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("consumable per kg", label).c_str(),
		&capture.consumablePerKg, 0.0, 0.0, "%.6f");
	ImGui::InputInt(describe("priority", label).c_str(), &capture.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCapture = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeCapture;
}

bool renderAtmosphereEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::AtmosphereSettings& atmosphere, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeAtmosphere = false;
	renderIdentity(label, atmosphere.id);
	ImGui::InputDouble(describe("volume m3", label).c_str(),
		&atmosphere.volumeM3, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting oxygen kg", label).c_str(),
		&atmosphere.startingOxygenKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("starting nitrogen kg", label).c_str(),
		&atmosphere.startingNitrogenKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("starting CO2 kg", label).c_str(),
		&atmosphere.startingCarbonDioxideKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("starting water vapour kg", label).c_str(),
		&atmosphere.startingWaterVapourKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("starting temperature C", label).c_str(),
		&atmosphere.startingTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("thermal capacity kJ/K", label).c_str(),
		&atmosphere.thermalCapacityKJPerK, 0.0, 0.0, "%.3f");
	char atmosphereReservoirBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(atmosphereReservoirBuffer, atmosphere.reservoirId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("reservoir (empty = adiabatic)", label).c_str(),
		atmosphereReservoirBuffer, sizeof(atmosphereReservoirBuffer)))
		atmosphere.reservoirId = atmosphereReservoirBuffer;
	ImGui::InputDouble(describe("exchanger conductance kW/K", label).c_str(),
		&atmosphere.exchangerConductanceKWPerK, 0.0, 0.0, "%.4f");
	ImGui::InputInt(describe("occupants", label).c_str(),
		&atmosphere.occupants, 1, 0);
	ImGui::InputDouble(describe("O2 use per occupant kg/s", label).c_str(),
		&atmosphere.oxygenUsePerOccupantKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("CO2 produced per occupant kg/s", label).c_str(),
		&atmosphere.co2ProductionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("sensible heat per occupant kW", label).c_str(),
		&atmosphere.sensibleHeatPerOccupantKW, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("moisture added per occupant kg/s", label).c_str(),
		&atmosphere.moistureAdditionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("leak rate kg/s", label).c_str(),
		&atmosphere.leakRateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("min pressure kPa", label).c_str(),
		&atmosphere.minPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max pressure kPa", label).c_str(),
		&atmosphere.maxPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("min O2 partial pressure kPa", label).c_str(),
		&atmosphere.minOxygenPartialPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max CO2 partial pressure kPa", label).c_str(),
		&atmosphere.maxCarbonDioxidePartialPressureKPa, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeAtmosphere = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeAtmosphere;
}

bool renderAirlockCycleEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::AirlockCycleSettings& cycle, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeCycle = false;
	renderIdentity(label, cycle.id);
	char cycleAtmosphereBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cycleAtmosphereBuffer, cycle.atmosphereId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("atmosphere", label).c_str(),
		cycleAtmosphereBuffer, sizeof(cycleAtmosphereBuffer)))
		cycle.atmosphereId = cycleAtmosphereBuffer;
	ImGui::InputDouble(describe("gas mass kg", label).c_str(),
		&cycle.gasMassKg, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("first occurrence s", label).c_str(),
		&cycle.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recurrence s (0 = one-off)", label).c_str(),
		&cycle.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recovered fraction", label).c_str(),
		&cycle.recoveredFraction, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(),
		&cycle.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCycle = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeCycle;
}

bool renderAtmospherePathwayEntry(core::StationSystemsSettings& draft,
	core::AtmospherePathwaySettings& pathway, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removePathway = false;
	renderIdentity(label, pathway.id);
	// The two ends are the same references the hatch node's End A / End B
	// connectors write, so the form and the canvas cannot disagree about
	// which compartments one opening joins. Openness is authored here and
	// actuated at runtime by scheduled commands; this field is where the
	// opening starts, not where it is held.
	auto renderEnd = [&](char const* field, std::string& end, std::string const& otherEnd) {
		auto const* preview = end.empty() ? "Unconnected"
			: end == core::StationExteriorAtmosphereId ? "Exterior vacuum" : end.c_str();
		if (!ImGui::BeginCombo(describe(field, label).c_str(), preview)) return;
		if (ImGui::Selectable("Unconnected", end.empty())) end.clear();
		for (auto const& atmosphere : draft.atmospheres)
		{
			if (atmosphere.id.empty() || atmosphere.id == otherEnd
				|| atmosphere.id == core::StationExteriorAtmosphereId) continue;
			if (ImGui::Selectable(atmosphere.id.c_str(), end == atmosphere.id))
				end = atmosphere.id;
		}
		if (otherEnd != core::StationExteriorAtmosphereId
			&& ImGui::Selectable("Exterior vacuum", end == core::StationExteriorAtmosphereId))
			end = core::StationExteriorAtmosphereId;
		ImGui::EndCombo();
	};
	renderEnd("end A compartment", pathway.aAtmosphereId, pathway.bAtmosphereId);
	renderEnd("end B compartment", pathway.bAtmosphereId, pathway.aAtmosphereId);
	ImGui::InputDouble(describe("area m2", label).c_str(),
		&pathway.areaM2, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("open fraction", label).c_str(),
		&pathway.openFraction, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removePathway = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removePathway;
}

bool renderThermalLinkEntry(core::StationSystemsSettings& draft,
	core::ThermalLinkSettings& link, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeLink = false;
	renderIdentity(label, link.id);
	// The two ends are the same references the wall node's End A / End B
	// connectors write, so the form and the canvas cannot disagree about
	// which compartments one wall joins. Exterior vacuum is never offered:
	// it has no temperature to conduct against, and the validator refuses it.
	auto renderEnd = [&](char const* field, std::string& end, std::string const& otherEnd) {
		auto const* preview = end.empty() ? "Unconnected" : end.c_str();
		if (!ImGui::BeginCombo(describe(field, label).c_str(), preview)) return;
		if (ImGui::Selectable("Unconnected", end.empty())) end.clear();
		for (auto const& atmosphere : draft.atmospheres)
		{
			if (atmosphere.id.empty() || atmosphere.id == otherEnd
				|| atmosphere.id == core::StationExteriorAtmosphereId) continue;
			if (ImGui::Selectable(atmosphere.id.c_str(), end == atmosphere.id))
				end = atmosphere.id;
		}
		ImGui::EndCombo();
	};
	renderEnd("end A compartment", link.aAtmosphereId, link.bAtmosphereId);
	renderEnd("end B compartment", link.bAtmosphereId, link.aAtmosphereId);
	ImGui::InputDouble(describe("conductance kW/K", label).c_str(),
		&link.conductanceKWPerK, 0.0, 0.0, "%.4f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeLink = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeLink;
}

bool renderTransferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceTransferSettings& transfer,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, transfer.id);
	char sourceBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(sourceBuffer, transfer.sourceId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("source", label).c_str(), sourceBuffer, sizeof(sourceBuffer)))
		transfer.sourceId = sourceBuffer;
	char destinationBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(destinationBuffer, transfer.destinationId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("destination", label).c_str(), destinationBuffer, sizeof(destinationBuffer)))
		transfer.destinationId = destinationBuffer;
	char transferUnitBuffer[8]{};
	std::strncpy(transferUnitBuffer, transfer.unit.c_str(), sizeof(transferUnitBuffer) - 1);
	if (ImGui::InputText(describe("unit", label).c_str(), transferUnitBuffer, sizeof(transferUnitBuffer)))
		transfer.unit = transferUnitBuffer;
	int mode = static_cast<int>(transfer.mode);
	if (ImGui::Combo(describe("mode", label).c_str(), &mode, "Continuous\0Scheduled\0"))
		transfer.mode = static_cast<core::ResourceTransferMode>(mode);
	if (transfer.mode == core::ResourceTransferMode::Scheduled)
	{
		ImGui::InputDouble(describe("quantity", label).c_str(), &transfer.quantity, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(describe("first occurrence seconds", label).c_str(), &transfer.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
		ImGui::InputDouble(describe("recurrence seconds (0 = one-off)", label).c_str(), &transfer.recurrenceSeconds, 0.0, 0.0, "%.6f");
		ImGui::TextWrapped("Recurrence is zero for one-off, otherwise at least one fixed tick. Each occurrence requests the original quantity; shortages never accumulate. Lossless Non-physical inventory event; connection ratings are informational.");
	}
	else ImGui::InputDouble(describe("rate per second", label).c_str(), &transfer.ratePerSecond, 0.0, 0.0, "%.3f");
	ImGui::TextWrapped("Switching mode retains inactive settings and connection design properties. Only the active mode executes after Apply.");
	// Keep invalid drafts editable (including hidden settings after a switch),
	// but use the same active-mode validation as Apply and document loading.
	std::string diagnostic;
	if (!core::validateStationTransferSettings(transfer, diagnostic))
		ImGui::TextWrapped("%s", diagnostic.c_str());
	ImGui::InputInt(describe("priority", label).c_str(), &transfer.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
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
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, load.id);
	ImGui::InputDouble(describe("electrical demand kW", label).c_str(),
		&load.electricalDemandKW, 0.0, 0.0, "%.3f");
	bool loadPeriodic = load.operatingProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic duty", label).c_str(), &loadPeriodic))
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
		ImGui::InputDouble(describe("constant duty", label).c_str(),
			&load.operatingProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		ImGui::InputDouble(describe("duty period seconds", label).c_str(),
			&load.operatingProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < load.operatingProfile.segments.size(); ++segment)
		{
			auto& stretch = load.operatingProfile.segments[segment];
			auto const segmentRow = std::string("duty segment ") + std::to_string(segment);
			ImGui::InputDouble(describe(segmentRow + " start", label).c_str(), &stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			ImGui::InputDouble(describe(segmentRow + " level", label).c_str(), &stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				load.operatingProfile.segments.erase(
					load.operatingProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (load.operatingProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add duty segment to " + label).c_str()))
			load.operatingProfile.segments.push_back(
				{load.operatingProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::InputDouble(describe("heat output kW", label).c_str(), &load.heatOutputKW, 0.0, 0.0, "%.3f");
	char thermalClassBuffer[16]{};
	std::strncpy(thermalClassBuffer, load.thermalClass.c_str(), sizeof(thermalClassBuffer) - 1);
	if (ImGui::InputText(describe("thermal class", label).c_str(),
		thermalClassBuffer, sizeof(thermalClassBuffer)))
		load.thermalClass = thermalClassBuffer;
	bool requiresCooling = load.coolingDisposition == core::StationCoolingDisposition::CoolingService;
	if (ImGui::Checkbox(describe("requires cooling service", label).c_str(), &requiresCooling))
		load.coolingDisposition = requiresCooling
			? core::StationCoolingDisposition::CoolingService
			: core::StationCoolingDisposition::Exported;
	ImGui::InputDouble(describe("critical floor kW", label).c_str(),
		&load.criticalFloorKW, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(), &load.priority);
	ImGui::InputDouble(describe("max curtailment", label).c_str(),
		&load.maxCurtailment, 0.0, 0.0, "%.3f");
	// #417 recovery rules (purely discretionary loads only; a load
	// with a protected floor is restored by the protected pass). All
	// default to zero: no dwell/hysteresis, clean restart.
	ImGui::InputDouble(describe("recovery dwell (s)", label).c_str(),
		&load.recoveryDwellSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recovery hysteresis fraction", label).c_str(),
		&load.recoveryHysteresisFraction, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("restart surge power (kW)", label).c_str(),
		&load.restartSurgePowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("restart surge seconds", label).c_str(),
		&load.restartSurgeSeconds, 0.0, 0.0, "%.3f");
	char heatSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(heatSinkBuffer, load.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink", label).c_str(),
		heatSinkBuffer, sizeof(heatSinkBuffer)))
		load.heatSinkId = heatSinkBuffer;
	for (size_t exchange = 0; exchange < load.materialExchanges.size(); )
	{
		auto& flow = load.materialExchanges[exchange];
		auto const exchangeRow = std::string("exchange ") + std::to_string(exchange);
		char exchangeStoreBuffer[core::MaxStationStoreIdLength + 1]{};
		std::strncpy(exchangeStoreBuffer, flow.storeId.c_str(), core::MaxStationStoreIdLength);
		if (ImGui::InputText(describe(exchangeRow + " store", label).c_str(),
			exchangeStoreBuffer, sizeof(exchangeStoreBuffer)))
			flow.storeId = exchangeStoreBuffer;
		ImGui::SameLine();
		ImGui::InputDouble(describe(exchangeRow + " rate", label).c_str(), &flow.ratePerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		ImGui::Checkbox(describe(exchangeRow + " consumes", label).c_str(), &flow.consumes);
		ImGui::SameLine();
		bool const removeExchange = ImGui::Button(describe("Remove " + exchangeRow, label).c_str());
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
	ImGui::PopID();
	return removeRequested;
}

bool renderBatteryEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::BatterySettings& battery,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, battery.id);
	ImGui::InputDouble(describe("nameplate kWh", label).c_str(),
		&battery.nameplateKWh, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("usable kWh", label).c_str(),
		&battery.usableKWh, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting stored kWh", label).c_str(),
		&battery.startingStoredKWh, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max charge kW", label).c_str(),
		&battery.maxChargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max discharge kW", label).c_str(),
		&battery.maxDischargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("charge efficiency", label).c_str(),
		&battery.chargeEfficiency, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("discharge efficiency", label).c_str(),
		&battery.dischargeEfficiency, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderGeneratorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::GeneratorSettings& generator,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, generator.id);
	char fuelStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(fuelStoreBuffer, generator.fuelStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("fuel store", label).c_str(),
		fuelStoreBuffer, sizeof(fuelStoreBuffer)))
		generator.fuelStoreId = fuelStoreBuffer;
	char generatorClassBuffer[16]{};
	std::strncpy(generatorClassBuffer, generator.thermalClass.c_str(), sizeof(generatorClassBuffer) - 1);
	if (ImGui::InputText(describe("thermal class", label).c_str(),
		generatorClassBuffer, sizeof(generatorClassBuffer)))
		generator.thermalClass = generatorClassBuffer;
	char generatorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(generatorSinkBuffer, generator.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink", label).c_str(),
		generatorSinkBuffer, sizeof(generatorSinkBuffer)))
		generator.heatSinkId = generatorSinkBuffer;
	ImGui::InputDouble(describe("rated output kW", label).c_str(),
		&generator.ratedOutputKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("efficiency", label).c_str(),
		&generator.efficiency, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("ramp kW per second", label).c_str(),
		&generator.maxRampKWPerSecond, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("commanded output kW", label).c_str(),
		&generator.commandedOutputKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("startup power kW", label).c_str(),
		&generator.startupPowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("startup seconds", label).c_str(),
		&generator.startupSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("shutdown heat kW", label).c_str(),
		&generator.shutdownHeatKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("shutdown seconds", label).c_str(),
		&generator.shutdownSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max sink temperature C", label).c_str(),
		&generator.maxSinkTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderFuelCellEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::FuelCellSettings& cell,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, cell.id);
	char cellHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellHydrogenBuffer, cell.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("hydrogen reactant store", label).c_str(),
		cellHydrogenBuffer, sizeof(cellHydrogenBuffer)))
		cell.hydrogenStoreId = cellHydrogenBuffer;
	char cellOxygenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellOxygenBuffer, cell.oxygenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("oxygen reactant store", label).c_str(),
		cellOxygenBuffer, sizeof(cellOxygenBuffer)))
		cell.oxygenStoreId = cellOxygenBuffer;
	char cellWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellWaterBuffer, cell.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("product water store", label).c_str(),
		cellWaterBuffer, sizeof(cellWaterBuffer)))
		cell.waterStoreId = cellWaterBuffer;
	char cellThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellThermalBuffer, cell.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink thermal class", label).c_str(),
		cellThermalBuffer, sizeof(cellThermalBuffer)))
		cell.thermalClass = cellThermalBuffer;
	char cellSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(cellSinkBuffer, cell.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink reservoir", label).c_str(),
		cellSinkBuffer, sizeof(cellSinkBuffer)))
		cell.heatSinkId = cellSinkBuffer;
	ImGui::InputDouble(describe("rated output kW", label).c_str(),
		&cell.ratedOutputKW, 0.0, 10.0, "%.3f");
	ImGui::InputDouble(describe("efficiency (0..1]", label).c_str(),
		&cell.efficiency, 0.0, 0.05, "%.3f");
	ImGui::InputDouble(describe("commanded output kW", label).c_str(),
		&cell.commandedOutputKW, 0.0, 1.0, "%.3f");
	ImGui::InputDouble(describe("max sink temperature C", label).c_str(),
		&cell.maxSinkTemperatureC, 0.0, 5.0, "%.1f");
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	return removeRequested;
}

bool renderSabatierReactorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::SabatierReactorSettings& reactor,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, reactor.id);
	char reactorCo2Buffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorCo2Buffer, reactor.co2StoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("CO2 feed store", label).c_str(),
		reactorCo2Buffer, sizeof(reactorCo2Buffer)))
		reactor.co2StoreId = reactorCo2Buffer;
	char reactorHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorHydrogenBuffer, reactor.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("hydrogen feed store", label).c_str(),
		reactorHydrogenBuffer, sizeof(reactorHydrogenBuffer)))
		reactor.hydrogenStoreId = reactorHydrogenBuffer;
	char reactorMethaneBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorMethaneBuffer, reactor.methaneStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("methane product store", label).c_str(),
		reactorMethaneBuffer, sizeof(reactorMethaneBuffer)))
		reactor.methaneStoreId = reactorMethaneBuffer;
	char reactorWaterBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorWaterBuffer, reactor.waterStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("product water store", label).c_str(),
		reactorWaterBuffer, sizeof(reactorWaterBuffer)))
		reactor.waterStoreId = reactorWaterBuffer;
	char reactorThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorThermalBuffer, reactor.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink thermal class", label).c_str(),
		reactorThermalBuffer, sizeof(reactorThermalBuffer)))
		reactor.thermalClass = reactorThermalBuffer;
	char reactorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(reactorSinkBuffer, reactor.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink reservoir", label).c_str(),
		reactorSinkBuffer, sizeof(reactorSinkBuffer)))
		reactor.heatSinkId = reactorSinkBuffer;
	ImGui::InputDouble(describe("rated CO2 conversion kg/s", label).c_str(),
		&reactor.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("specific energy kWh/kg CO2", label).c_str(),
		&reactor.specificEnergyKWhPerKgCO2, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("max sink temperature C", label).c_str(),
		&reactor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt(describe("priority", label).c_str(), &reactor.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderMethaneProcessorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::MethaneProcessorSettings& processor,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, processor.id);
	char processorMethaneBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorMethaneBuffer, processor.methaneStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("methane feed store", label).c_str(),
		processorMethaneBuffer, sizeof(processorMethaneBuffer)))
		processor.methaneStoreId = processorMethaneBuffer;
	char processorHydrogenBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorHydrogenBuffer, processor.hydrogenStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("recovered hydrogen store", label).c_str(),
		processorHydrogenBuffer, sizeof(processorHydrogenBuffer)))
		processor.hydrogenStoreId = processorHydrogenBuffer;
	char processorCarbonBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorCarbonBuffer, processor.carbonStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("solid carbon store", label).c_str(),
		processorCarbonBuffer, sizeof(processorCarbonBuffer)))
		processor.carbonStoreId = processorCarbonBuffer;
	char processorThermalBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorThermalBuffer, processor.thermalClass.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink thermal class", label).c_str(),
		processorThermalBuffer, sizeof(processorThermalBuffer)))
		processor.thermalClass = processorThermalBuffer;
	char processorSinkBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(processorSinkBuffer, processor.heatSinkId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("heat sink reservoir", label).c_str(),
		processorSinkBuffer, sizeof(processorSinkBuffer)))
		processor.heatSinkId = processorSinkBuffer;
	ImGui::InputDouble(describe("rated CH4 processing kg/s", label).c_str(),
		&processor.rateKgPerS, 0.0, 0.0, "%.6f");
	ImGui::InputDouble(describe("specific energy kWh/kg CH4", label).c_str(),
		&processor.specificEnergyKWhPerKgCH4, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("max sink temperature C", label).c_str(),
		&processor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt(describe("priority", label).c_str(), &processor.priority, 1, 0);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

// #656: the thermal reservoir entry editor, extracted from the Station
// Systems form tab so the Flow inspector renders the exact same fields,
// labels and deferred-removal behaviour against the same draft.
bool renderThermalReservoirEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ThermalReservoirSettings& reservoir,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, reservoir.id);
	char reservoirClassBuffer[16]{};
	std::strncpy(reservoirClassBuffer, reservoir.thermalClass.c_str(), sizeof(reservoirClassBuffer) - 1);
	if (ImGui::InputText(describe("thermal class", label).c_str(),
		reservoirClassBuffer, sizeof(reservoirClassBuffer)))
		reservoir.thermalClass = reservoirClassBuffer;
	ImGui::InputDouble(describe("heat capacity kJ/K", label).c_str(),
		&reservoir.heatCapacityKJPerK, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting temperature C", label).c_str(),
		&reservoir.startingTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("min temperature C", label).c_str(),
		&reservoir.minTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max temperature C", label).c_str(),
		&reservoir.maxTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("absorbed heat kW", label).c_str(),
		&reservoir.absorbedHeatKW, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderRadiatorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::RadiatorSettings& radiator,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, radiator.id);
	char radiatorHostBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(radiatorHostBuffer, radiator.reservoirId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("reservoir", label).c_str(),
		radiatorHostBuffer, sizeof(radiatorHostBuffer)))
		radiator.reservoirId = radiatorHostBuffer;
	ImGui::InputDouble(describe("area m2", label).c_str(),
		&radiator.areaM2, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("emissivity", label).c_str(),
		&radiator.emissivity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("view factor", label).c_str(),
		&radiator.viewFactor, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("sink temperature C", label).c_str(),
		&radiator.sinkTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderCoolantLoopEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::CoolantLoopSettings& loop,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	ImGui::PushItemWidth(220);
	renderIdentity(label, loop.id);
	char host[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(host, loop.reservoirId.c_str(), sizeof(host) - 1);
	if (ImGui::InputText(describe("reservoir", label).c_str(), host, sizeof(host))) loop.reservoirId = host;
	ImGui::InputDouble(describe("coolant capacity kg", label).c_str(), &loop.coolantCapacityKg, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting coolant kg", label).c_str(), &loop.startingCoolantKg, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("specific heat kJ/kg K", label).c_str(), &loop.specificHeatKJPerKgK, 0.0, 0.0, "%.4f");
	ImGui::InputDouble(describe("starting temperature C", label).c_str(), &loop.startingTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("freeze setpoint C", label).c_str(), &loop.freezeProtectionSetpointC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("pump power kW", label).c_str(), &loop.pumpPowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("rated flow kg/s", label).c_str(), &loop.ratedFlowKgPerS, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("exchanger conductance kW/K", label).c_str(), &loop.exchangerConductanceKWPerK, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("heater capacity kW", label).c_str(), &loop.heaterCapacityKW, 0.0, 0.0, "%.3f");
	ImGui::Checkbox((std::string("protected critical service")
		+ " (circulation and freeze heat drawn ahead of discretionary loads; "
		  "protection is authored here, never inferred)").c_str(),
		&loop.criticalService);
	ImGui::SameLine();
	bool const remove = !removeLabel.empty() && ImGui::Button(removeLabel.c_str());
	ImGui::PopItemWidth();
	ImGui::PopID();
	return remove;
}

bool renderThermalBufferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ThermalBufferSettings& buffer, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeRequested = false;
	renderIdentity(label, buffer.id);
	char host[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(host, buffer.reservoirId.c_str(), sizeof(host) - 1);
	if (ImGui::InputText(describe("host reservoir id", label).c_str(), host, sizeof(host))) buffer.reservoirId = host;
	char access[16]{};
	std::strncpy(access, buffer.accessMode.c_str(), sizeof(access) - 1);
	if (ImGui::InputText(describe("access mode (circulation|passive)", label).c_str(), access, sizeof(access))) buffer.accessMode = access;
	char loop[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(loop, buffer.circulationLoopId.c_str(), sizeof(loop) - 1);
	if (ImGui::InputText(describe("circulation loop id", label).c_str(), loop, sizeof(loop))) buffer.circulationLoopId = loop;
	ImGui::InputDouble(describe("capacity kJ", label).c_str(), &buffer.capacityKJ, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("starting energy kJ", label).c_str(), &buffer.startingEnergyKJ, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("min usable temperature C", label).c_str(), &buffer.minUsableTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max usable temperature C", label).c_str(), &buffer.maxUsableTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max charge kW", label).c_str(), &buffer.maxChargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max discharge kW", label).c_str(), &buffer.maxDischargeKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("transfer conductance kW/K", label).c_str(), &buffer.transferConductanceKWPerK, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str())) removeRequested = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeRequested;
}

bool renderChillerEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ChillerSettings& chiller, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeChiller = false;
	renderIdentity(label, chiller.id);
	char chillerClassBuffer[16]{};
	std::strncpy(chillerClassBuffer, chiller.thermalClass.c_str(), sizeof(chillerClassBuffer) - 1);
	if (ImGui::InputText(describe("thermal class", label).c_str(),
		chillerClassBuffer, sizeof(chillerClassBuffer)))
		chiller.thermalClass = chillerClassBuffer;
	char coldLoopBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(coldLoopBuffer, chiller.coldLoopId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("cold loop", label).c_str(),
		coldLoopBuffer, sizeof(coldLoopBuffer)))
		chiller.coldLoopId = coldLoopBuffer;
	char condenserBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(condenserBuffer, chiller.condenserReservoirId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("condenser reservoir", label).c_str(),
		condenserBuffer, sizeof(condenserBuffer)))
		chiller.condenserReservoirId = condenserBuffer;
	ImGui::InputDouble(describe("rated cooling capacity kW", label).c_str(),
		&chiller.ratedCoolingCapacityKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("COP", label).c_str(),
		&chiller.cop, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("auxiliary power kW", label).c_str(),
		&chiller.auxiliaryPowerKW, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("chilled setpoint C", label).c_str(),
		&chiller.chilledSetpointC, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("max condensing temperature C", label).c_str(),
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
	ImGui::PopID();
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
}

bool renderDeliveryEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceDeliverySettings& delivery, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeDelivery = false;
	renderIdentity(label, delivery.id);
	char deliveryStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(deliveryStoreBuffer, delivery.storeId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("store", label).c_str(),
		deliveryStoreBuffer, sizeof(deliveryStoreBuffer)))
		delivery.storeId = deliveryStoreBuffer;
	char deliveryUnitBuffer[8]{};
	std::strncpy(deliveryUnitBuffer, delivery.unit.c_str(), sizeof(deliveryUnitBuffer) - 1);
	if (ImGui::InputText(describe("unit", label).c_str(), deliveryUnitBuffer, sizeof(deliveryUnitBuffer)))
		delivery.unit = deliveryUnitBuffer;
	ImGui::InputDouble(describe("quantity", label).c_str(), &delivery.quantity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("first occurrence seconds", label).c_str(),
		&delivery.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recurrence seconds", label).c_str(),
		&delivery.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::Checkbox(describe("arrives usable", label).c_str(), &delivery.arrivesQualified);
	ImGui::SameLine();
	ImGui::InputInt(describe("priority", label).c_str(), &delivery.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeDelivery = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeDelivery;
}

bool renderWasteEjectionEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WasteEjectionSettings& ejection, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeEjection = false;
	renderIdentity(label, ejection.id);
	char ejectionStoreBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(ejectionStoreBuffer, ejection.storeId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("store", label).c_str(),
		ejectionStoreBuffer, sizeof(ejectionStoreBuffer)))
		ejection.storeId = ejectionStoreBuffer;
	char ejectionUnitBuffer[8]{};
	std::strncpy(ejectionUnitBuffer, ejection.unit.c_str(), sizeof(ejectionUnitBuffer) - 1);
	if (ImGui::InputText(describe("unit", label).c_str(), ejectionUnitBuffer, sizeof(ejectionUnitBuffer)))
		ejection.unit = ejectionUnitBuffer;
	int eligibility = static_cast<int>(ejection.eligibility);
	if (ImGui::Combo(describe("eligibility", label).c_str(), &eligibility, "Qualified only\0Held only\0"))
		ejection.eligibility = static_cast<core::WasteEjectionEligibility>(eligibility);
	ImGui::InputDouble(describe("quantity", label).c_str(), &ejection.quantity, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("first occurrence seconds", label).c_str(),
		&ejection.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recurrence seconds", label).c_str(),
		&ejection.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	ImGui::InputInt(describe("priority", label).c_str(), &ejection.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeEjection = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeEjection;
}

bool renderOutageEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::InstallationOutageSettings& outage, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	bool removeOutage = false;
	renderIdentity(label, outage.id);
	char outageTargetBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(outageTargetBuffer, outage.installationId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("installation id", label).c_str(),
		outageTargetBuffer, sizeof(outageTargetBuffer)))
		outage.installationId = outageTargetBuffer;
	ImGui::InputDouble(describe("capacity fraction (0..1)", label).c_str(),
		&outage.capacityFraction, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("first occurrence seconds", label).c_str(),
		&outage.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("duration seconds", label).c_str(),
		&outage.durationSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recurrence seconds (0 = one-off)", label).c_str(),
		&outage.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(), &outage.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeOutage = true;
	ImGui::PopItemWidth();
	ImGui::PopID();
	return removeOutage;
}

bool renderMaintenanceEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::MaintenanceJobSettings& job, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	ImGui::PushItemWidth(128.0f);
	auto const row = label.substr(label.find_last_of(' ') + 1);
	bool removeJob = false;
	renderIdentity(label, job.id);
	char jobTargetBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(jobTargetBuffer, job.installationId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("installation id", label).c_str(),
		jobTargetBuffer, sizeof(jobTargetBuffer)))
		job.installationId = jobTargetBuffer;
	ImGui::InputDouble(describe("first occurrence seconds", label).c_str(),
		&job.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("duration seconds", label).c_str(),
		&job.durationSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputDouble(describe("recurrence seconds (0 = one-off)", label).c_str(),
		&job.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(), &job.priority);
	char jobWasteBuffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(jobWasteBuffer, job.wasteStoreId.c_str(), core::MaxStationStoreIdLength);
	if (ImGui::InputText(describe("waste store id", label).c_str(),
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
		if (ImGui::InputText(describe("Maintenance requirement " + requirementRow + " store id", label).c_str(),
			requirementStoreBuffer, sizeof(requirementStoreBuffer)))
			requirement.storeId = requirementStoreBuffer;
		ImGui::InputDouble(describe("Maintenance requirement " + requirementRow + " quantity", label).c_str(),
			&requirement.quantity, 0.0, 0.0, "%.3f");
		ImGui::Checkbox(describe("Maintenance requirement " + requirementRow
			+ " consumed (unchecked = reusable tool)", label).c_str(), &requirement.consumed);
		if (job.requirements.size() > 1 && ImGui::Button(describe("Remove requirement " + requirementRow, label).c_str()))
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
	ImGui::PopID();
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

bool renderAtmospherePathwayEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.atmospherePathways.size()) return false;
	return renderAtmospherePathwayEntry(draft, draft.atmospherePathways[index],
		"Atmosphere pathway " + std::to_string(index), {});
}

bool renderThermalLinkEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.thermalLinks.size()) return false;
	return renderThermalLinkEntry(draft, draft.thermalLinks[index],
		"Thermal link " + std::to_string(index), {});
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
constexpr std::array<StationEntryEditor, 31> kEntryEditors{{
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
	{"Atmosphere pathway", &renderAtmospherePathwayEntryAt},
	{"Thermal link", &renderThermalLinkEntryAt},
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
namespace
{
struct EntryCreator
{
	std::string_view nodeType;
	char const* label;
	bool (*canAdd)(core::StationSystemsSettings const&);
	void (*add)(core::StationSystemsSettings&);
};

// Existing form defaults and reference preferences, independent of list rendering.
// Groups and ordering come from the same catalogue as Flow placement.
constexpr std::array<EntryCreator, 31> kEntryCreators{{
	{"Store", "Store",
		[](core::StationSystemsSettings const& draft) { return draft.stores.size() < core::MaxStationResourceStores; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Transfer", "Transfer",
		[](core::StationSystemsSettings const& draft) { return draft.transfers.size() < core::MaxStationResourceTransfers; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Conversion", "Conversion",
		[](core::StationSystemsSettings const& draft) { return draft.conversions.size() < core::MaxStationResourceConversions; },
		[](core::StationSystemsSettings& draft) {
			core::ResourceConversionSettings conversion;
			for (size_t candidate = draft.conversions.size() + 1;; ++candidate)
			{
				conversion.id = "conversion" + std::to_string(candidate);
				auto const taken = std::any_of(draft.conversions.begin(), draft.conversions.end(),
					[&](core::ResourceConversionSettings const& existing) { return existing.id == conversion.id; });
				if (!taken) break;
			}
			conversion.inputs.push_back({});
			conversion.outputs.push_back({});
			draft.conversions.push_back(std::move(conversion));
		}},
	{"Solar", "Solar array",
		[](core::StationSystemsSettings const& draft) { return draft.solarArrays.size() < core::MaxStationSolarArrays; },
		[](core::StationSystemsSettings& draft) {
			core::SolarArraySettings array;
			array.capacityKW = 10.0;
			for (size_t candidate = draft.solarArrays.size() + 1;; ++candidate)
			{
				array.id = "solar" + std::to_string(candidate);
				auto const taken = std::any_of(draft.solarArrays.begin(), draft.solarArrays.end(),
					[&](core::SolarArraySettings const& existing) { return existing.id == array.id; });
				if (!taken) break;
			}
			draft.solarArrays.push_back(std::move(array));
		}},
	{"Equipment", "Equipment load",
		[](core::StationSystemsSettings const& draft) { return draft.equipmentLoads.size() < core::MaxStationEquipmentLoads; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Battery", "Battery",
		[](core::StationSystemsSettings const& draft) { return draft.batteries.size() < core::MaxStationBatteries; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Generator", "Generator",
		[](core::StationSystemsSettings const& draft) { return draft.generators.size() < core::MaxStationGenerators; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Fuel cell", "Fuel cell",
		[](core::StationSystemsSettings const& draft) { return draft.fuelCells.size() < core::MaxStationFuelCells; },
		[](core::StationSystemsSettings& draft) {
			core::FuelCellSettings cell;
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
		}},
	{"Sabatier", "Sabatier reactor",
		[](core::StationSystemsSettings const& draft) { return draft.sabatierReactors.size() < core::MaxStationSabatierReactors; },
		[](core::StationSystemsSettings& draft) {
			core::SabatierReactorSettings reactor;
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
		}},
	{"Methane processor", "Methane processor",
		[](core::StationSystemsSettings const& draft) { return draft.methaneProcessors.size() < core::MaxStationMethaneProcessors; },
		[](core::StationSystemsSettings& draft) {
			core::MethaneProcessorSettings processor;
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
		}},
	{"Reservoir", "Thermal reservoir",
		[](core::StationSystemsSettings const& draft) { return draft.thermalReservoirs.size() < core::MaxStationThermalReservoirs; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Radiator", "Radiator",
		[](core::StationSystemsSettings const& draft) { return draft.radiators.size() < core::MaxStationRadiators; },
		[](core::StationSystemsSettings& draft) {
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
		}},
	{"Coolant loop", "Coolant loop",
		[](core::StationSystemsSettings const& draft) { return draft.coolantLoops.size() < core::MaxStationCoolantLoops; },
		[](core::StationSystemsSettings& draft) {
			core::CoolantLoopSettings loop;
			loop.coolantCapacityKg = 100.0;
			loop.startingCoolantKg = 100.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.startingTemperatureC = 20.0;
			loop.freezeProtectionSetpointC = 5.0;
			loop.pumpPowerKW = 1.0;
			loop.ratedFlowKgPerS = 1.0;
			loop.exchangerConductanceKWPerK = 2.0;
			loop.heaterCapacityKW = 1.0;
			if (!draft.thermalReservoirs.empty())
				loop.reservoirId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.coolantLoops.size() + 1;; ++candidate)
			{
				loop.id = "coolantLoop" + std::to_string(candidate);
				auto const taken = std::any_of(draft.coolantLoops.begin(), draft.coolantLoops.end(),
					[&](core::CoolantLoopSettings const& existing) { return existing.id == loop.id; });
				if (!taken) break;
			}
			draft.coolantLoops.push_back(std::move(loop));
		}},
	{"Chiller", "Chiller",
		[](core::StationSystemsSettings const& draft) { return draft.chillers.size() < core::MaxStationChillers; },
		[](core::StationSystemsSettings& draft) {
			core::ChillerSettings chiller;
			chiller.thermalClass = "moderate";
			chiller.ratedCoolingCapacityKW = 5.0;
			chiller.cop = 3.0;
			chiller.auxiliaryPowerKW = 0.2;
			chiller.chilledSetpointC = 5.0;
			chiller.maxCondensingTemperatureC = 45.0;
			if (!draft.coolantLoops.empty())
				chiller.coldLoopId = draft.coolantLoops.front().id;
			// Prefer a condenser sink that is not the cold loop's own host:
			// rejecting into the reservoir the cold side cools is a heater,
			// so the default avoids that refusal when another reservoir exists.
			if (!draft.thermalReservoirs.empty())
			{
				std::string coldHost;
				for (auto const& loop : draft.coolantLoops)
					if (loop.id == chiller.coldLoopId) coldHost = loop.reservoirId;
				chiller.condenserReservoirId = draft.thermalReservoirs.back().id;
				for (auto const& reservoir : draft.thermalReservoirs)
					if (reservoir.id != coldHost) chiller.condenserReservoirId = reservoir.id;
			}
			for (size_t candidate = draft.chillers.size() + 1;; ++candidate)
			{
				chiller.id = "chiller" + std::to_string(candidate);
				auto const taken = std::any_of(draft.chillers.begin(), draft.chillers.end(),
					[&](core::ChillerSettings const& existing) { return existing.id == chiller.id; });
				if (!taken) break;
			}
			draft.chillers.push_back(std::move(chiller));
		}},
	{"Thermal buffer", "Thermal buffer",
		[](core::StationSystemsSettings const& draft) { return draft.thermalBuffers.size() < core::MaxStationThermalBuffers; },
		[](core::StationSystemsSettings& draft) {
			core::ThermalBufferSettings buffer;
			buffer.accessMode = "circulation";
			buffer.capacityKJ = 1000.0;
			buffer.startingEnergyKJ = 0.0;
			buffer.minUsableTemperatureC = 0.0;
			buffer.maxUsableTemperatureC = 100.0;
			buffer.maxChargeKW = 10.0;
			buffer.maxDischargeKW = 10.0;
			buffer.transferConductanceKWPerK = 1.0;
			if (!draft.thermalReservoirs.empty())
				buffer.reservoirId = draft.thermalReservoirs.front().id;
			if (!draft.coolantLoops.empty())
				buffer.circulationLoopId = draft.coolantLoops.front().id;
			for (size_t candidate = draft.thermalBuffers.size() + 1;; ++candidate)
			{
				buffer.id = "buffer" + std::to_string(candidate);
				auto const taken = std::any_of(draft.thermalBuffers.begin(), draft.thermalBuffers.end(),
					[&](core::ThermalBufferSettings const& existing) { return existing.id == buffer.id; });
				if (!taken) break;
			}
			draft.thermalBuffers.push_back(std::move(buffer));
		}},
	{"Crew", "Crew",
		[](core::StationSystemsSettings const& draft) { return draft.crews.size() < core::MaxStationCrews; },
		[](core::StationSystemsSettings& draft) {
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
			if (!draft.atmospheres.empty())
				crew.atmosphereId = draft.atmospheres.back().id;
			if (!draft.stores.empty())
			{
				crew.foodStoreId = draft.stores.front().id;
				crew.waterStoreId = draft.stores.front().id;
				crew.urineStoreId = draft.stores.front().id;
				crew.organicWasteStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.crews.size() + 1;; ++candidate)
			{
				crew.id = "crew" + std::to_string(candidate);
				auto const taken = std::any_of(draft.crews.begin(), draft.crews.end(),
					[&](core::CrewSettings const& existing) { return existing.id == crew.id; });
				if (!taken) break;
			}
			draft.crews.push_back(std::move(crew));
		}},
	{"Crops", "Crop population",
		[](core::StationSystemsSettings const& draft) { return draft.cropPopulations.size() < core::MaxStationCropPopulations; },
		[](core::StationSystemsSettings& draft) {
			core::CropPopulationSettings crop;
			crop.growingAreaM2 = 10.0;
			crop.maxBiomassDensityKgPerM2 = 2.0;
			crop.initialBiomassKg = 1.0;
			crop.lampRatedKW = 1.0;
			crop.illuminationProfile.constantLevel = 1.0;
			crop.circulationPowerKW = 0.1;
			crop.growthYieldKgPerKWh = 0.01;
			crop.respirationPerHour = 0.012;
			crop.composition.energyKJPerKgDry = 1700.0;
			if (!draft.atmospheres.empty())
				crop.atmosphereId = draft.atmospheres.front().id;
			if (!draft.stores.empty())
			{
				crop.waterStoreId = draft.stores.front().id;
				crop.nitrogenStoreId = draft.stores.front().id;
				crop.phosphorusStoreId = draft.stores.front().id;
				crop.potassiumStoreId = draft.stores.front().id;
				crop.mineralsStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.cropPopulations.size() + 1;; ++candidate)
			{
				crop.id = "croppopulation" + std::to_string(candidate);
				auto const taken = std::any_of(draft.cropPopulations.begin(), draft.cropPopulations.end(),
					[&](core::CropPopulationSettings const& existing) { return existing.id == crop.id; });
				if (!taken) break;
			}
			draft.cropPopulations.push_back(std::move(crop));
		}},
	{"Algae", "Algae culture",
		[](core::StationSystemsSettings const& draft) { return draft.algaeCultures.size() < core::MaxStationAlgaeCultures; },
		[](core::StationSystemsSettings& draft) {
			core::AlgaeCultureSettings algae;
			algae.cultureVolumeL = 1000.0;
			algae.maxBiomassDensityKgPerL = 0.002;
			algae.initialBiomassKg = 0.5;
			algae.lampRatedKW = 1.0;
			algae.illuminationProfile.constantLevel = 1.0;
			algae.circulationPowerKW = 0.1;
			algae.growthYieldKgPerKWh = 0.01;
			algae.respirationPerHour = 0.012;
			algae.composition.energyKJPerKgDry = 1700.0;
			if (!draft.atmospheres.empty())
				algae.atmosphereId = draft.atmospheres.front().id;
			if (!draft.stores.empty())
			{
				algae.waterStoreId = draft.stores.front().id;
				algae.nitrogenStoreId = draft.stores.front().id;
				algae.phosphorusStoreId = draft.stores.front().id;
				algae.potassiumStoreId = draft.stores.front().id;
				algae.mineralsStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.algaeCultures.size() + 1;; ++candidate)
			{
				algae.id = "algaeculture" + std::to_string(candidate);
				auto const taken = std::any_of(draft.algaeCultures.begin(), draft.algaeCultures.end(),
					[&](core::AlgaeCultureSettings const& existing) { return existing.id == algae.id; });
				if (!taken) break;
			}
			draft.algaeCultures.push_back(std::move(algae));
		}},
	{"Microbial processor", "Microbial processor",
		[](core::StationSystemsSettings const& draft) { return draft.microbialProcessors.size() < core::MaxStationMicrobialProcessors; },
		[](core::StationSystemsSettings& draft) {
			core::MicrobialProcessorSettings processor;
			processor.thermalClass = "low";
			processor.rateKgPerS = 0.001;
			if (!draft.stores.empty())
			{
				processor.feedStoreId = draft.stores.front().id;
				processor.oxygenStoreId = draft.stores.front().id;
				processor.waterStoreId = draft.stores.front().id;
				processor.consumableStoreId = draft.stores.front().id;
				processor.nutrientStoreId = draft.stores.front().id;
				processor.recoveredWaterStoreId = draft.stores.front().id;
				processor.co2StoreId = draft.stores.front().id;
				processor.residueStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				processor.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.microbialProcessors.size() + 1;; ++candidate)
			{
				processor.id = "microbialprocessor" + std::to_string(candidate);
				auto const taken = std::any_of(draft.microbialProcessors.begin(), draft.microbialProcessors.end(),
					[&](core::MicrobialProcessorSettings const& existing) { return existing.id == processor.id; });
				if (!taken) break;
			}
			draft.microbialProcessors.push_back(std::move(processor));
		}},
	{"Water collector", "Water collector",
		[](core::StationSystemsSettings const& draft) { return draft.waterCollectors.size() < core::MaxStationWaterCollectors; },
		[](core::StationSystemsSettings& draft) {
			core::WaterCollectorSettings collector;
			collector.thermalClass = "moderate";
			collector.maxRateKgPerS = 0.001;
			collector.targetRelativeHumidity = 0.5;
			if (!draft.atmospheres.empty())
				collector.atmosphereId = draft.atmospheres.back().id;
			if (!draft.coolantLoops.empty())
				collector.coolingLoopId = draft.coolantLoops.back().id;
			if (!draft.stores.empty())
				collector.destStoreId = draft.stores.front().id;
			for (size_t candidate = draft.waterCollectors.size() + 1;; ++candidate)
			{
				collector.id = "collector" + std::to_string(candidate);
				auto const taken = std::any_of(draft.waterCollectors.begin(), draft.waterCollectors.end(),
					[&](core::WaterCollectorSettings const& existing) { return existing.id == collector.id; });
				if (!taken) break;
			}
			draft.waterCollectors.push_back(std::move(collector));
		}},
	{"Water treatment", "Water treatment",
		[](core::StationSystemsSettings const& draft) { return draft.waterTreatments.size() < core::MaxStationWaterTreatments; },
		[](core::StationSystemsSettings& draft) {
			core::WaterTreatmentSettings treatment;
			treatment.throughputKgPerS = 0.002;
			treatment.recoveryRatio = 0.85;
			treatment.specificEnergyKWhPerKg = 0.5;
			treatment.consumablePerKg = 0.01;
			if (!draft.stores.empty())
			{
				treatment.sourceStoreId = draft.stores.front().id;
				treatment.treatedStoreId = draft.stores.front().id;
				treatment.concentrateStoreId = draft.stores.front().id;
				treatment.consumableStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.waterTreatments.size() + 1;; ++candidate)
			{
				treatment.id = "treatment" + std::to_string(candidate);
				auto const taken = std::any_of(draft.waterTreatments.begin(), draft.waterTreatments.end(),
					[&](core::WaterTreatmentSettings const& existing) { return existing.id == treatment.id; });
				if (!taken) break;
			}
			draft.waterTreatments.push_back(std::move(treatment));
		}},
	{"Electrolyzer", "Electrolyzer",
		[](core::StationSystemsSettings const& draft) { return draft.electrolyzers.size() < core::MaxStationElectrolyzers; },
		[](core::StationSystemsSettings& draft) {
			core::ElectrolyzerSettings cell;
			cell.thermalClass = "low";
			cell.rateKgPerS = 0.001;
			cell.specificEnergyKWhPerKgWater = 4.5;
			cell.maxSinkTemperatureC = 60.0;
			if (!draft.stores.empty())
			{
				cell.waterStoreId = draft.stores.front().id;
				cell.oxygenStoreId = draft.stores.front().id;
				cell.hydrogenStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				cell.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.electrolyzers.size() + 1;; ++candidate)
			{
				cell.id = "electrolyzer" + std::to_string(candidate);
				auto const taken = std::any_of(draft.electrolyzers.begin(), draft.electrolyzers.end(),
					[&](core::ElectrolyzerSettings const& existing) { return existing.id == cell.id; });
				if (!taken) break;
			}
			draft.electrolyzers.push_back(std::move(cell));
		}},
	{"CO2 capture", "CO2 capture machine",
		[](core::StationSystemsSettings const& draft) { return draft.co2Captures.size() < core::MaxStationCo2Captures; },
		[](core::StationSystemsSettings& draft) {
			core::Co2CaptureSettings capture;
			capture.maxRateKgPerS = 0.001;
			capture.specificEnergyKWhPerKg = 0.5;
			capture.consumablePerKg = 0.0;
			if (!draft.stores.empty())
			{
				capture.captureStoreId = draft.stores.front().id;
				capture.consumableStoreId = draft.stores.front().id;
			}
			if (!draft.atmospheres.empty())
				capture.atmosphereId = draft.atmospheres.front().id;
			for (size_t candidate = draft.co2Captures.size() + 1;; ++candidate)
			{
				capture.id = "co2capture" + std::to_string(candidate);
				auto const taken = std::any_of(draft.co2Captures.begin(), draft.co2Captures.end(),
					[&](core::Co2CaptureSettings const& existing) { return existing.id == capture.id; });
				if (!taken) break;
			}
			draft.co2Captures.push_back(std::move(capture));
		}},
	{"Atmosphere", "Atmosphere",
		[](core::StationSystemsSettings const& draft) { return draft.atmospheres.size() < core::MaxStationAtmospheres; },
		[](core::StationSystemsSettings& draft) {
			core::AtmosphereSettings atmosphere;
			atmosphere.volumeM3 = 100.0;
			atmosphere.startingOxygenKg = 20.0;
			atmosphere.startingNitrogenKg = 80.0;
			atmosphere.startingTemperatureC = 20.0;
			atmosphere.thermalCapacityKJPerK = 100.0;
			atmosphere.minPressureKPa = 80.0;
			atmosphere.maxPressureKPa = 120.0;
			// Prefer coupling to the last drafted reservoir so a freshly
			// drafted atmosphere is thermally live by default.
			if (!draft.thermalReservoirs.empty())
				atmosphere.reservoirId = draft.thermalReservoirs.back().id;
			for (size_t candidate = draft.atmospheres.size() + 1;; ++candidate)
			{
				atmosphere.id = "compartment" + std::to_string(candidate);
				auto const taken = std::any_of(draft.atmospheres.begin(), draft.atmospheres.end(),
					[&](core::AtmosphereSettings const& existing) { return existing.id == atmosphere.id; });
				if (!taken) break;
			}
			draft.atmospheres.push_back(std::move(atmosphere));
		}},
	{"Atmosphere pathway", "Atmosphere pathway",
		[](core::StationSystemsSettings const& draft) { return draft.atmospherePathways.size() < core::MaxStationPathways; },
		[](core::StationSystemsSettings& draft) {
			// A new opening starts with both ends free, one square metre and
			// wide open. Wiring it is the drag; this is the thing the drag
			// lands on.
			core::AtmospherePathwaySettings pathway;
			pathway.areaM2 = 1.0;
			pathway.openFraction = 1.0;
			for (size_t candidate = draft.atmospherePathways.size() + 1;; ++candidate)
			{
				pathway.id = "hatch" + std::to_string(candidate);
				auto const taken = std::any_of(draft.atmospherePathways.begin(), draft.atmospherePathways.end(),
					[&](core::AtmospherePathwaySettings const& existing) { return existing.id == pathway.id; });
				if (!taken) break;
			}
			draft.atmospherePathways.push_back(std::move(pathway));
		}},
	{"Thermal link", "Thermal link",
		[](core::StationSystemsSettings const& draft) { return draft.thermalLinks.size() < core::MaxStationThermalLinks; },
		[](core::StationSystemsSettings& draft) {
			// A new wall starts with both ends free and a live conductance:
			// zero would be a dead wall the validator refuses, so the
			// default is a number that actually conducts. Wiring it is the
			// drag; this is the thing the drag lands on.
			core::ThermalLinkSettings link;
			link.conductanceKWPerK = 1.0;
			for (size_t candidate = draft.thermalLinks.size() + 1;; ++candidate)
			{
				link.id = "link" + std::to_string(candidate);
				auto const taken = std::any_of(draft.thermalLinks.begin(), draft.thermalLinks.end(),
					[&](core::ThermalLinkSettings const& existing) { return existing.id == link.id; });
				if (!taken) break;
			}
			draft.thermalLinks.push_back(std::move(link));
		}},
	{"Airlock cycle", "Airlock cycle",
		[](core::StationSystemsSettings const& draft) { return draft.airlockCycles.size() < core::MaxStationAirlockCycles; },
		[](core::StationSystemsSettings& draft) {
			core::AirlockCycleSettings cycle;
			cycle.gasMassKg = 1.0;
			cycle.recurrenceSeconds = 60.0;
			cycle.recoveredFraction = 0.95;
			if (!draft.atmospheres.empty())
				cycle.atmosphereId = draft.atmospheres.back().id;
			for (size_t candidate = draft.airlockCycles.size() + 1;; ++candidate)
			{
				cycle.id = "airlockCycle" + std::to_string(candidate);
				auto const taken = std::any_of(draft.airlockCycles.begin(), draft.airlockCycles.end(),
					[&](core::AirlockCycleSettings const& existing) { return existing.id == cycle.id; });
				if (!taken) break;
			}
			draft.airlockCycles.push_back(std::move(cycle));
		}},
	{"Delivery", "Delivery",
		[](core::StationSystemsSettings const& draft) { return draft.deliveries.size() < core::MaxStationResourceDeliveries; },
		[](core::StationSystemsSettings& draft) {
			core::ResourceDeliverySettings delivery;
			delivery.quantity = 10.0;
			if (!draft.stores.empty())
			{
				delivery.storeId = draft.stores.front().id;
				delivery.unit = draft.stores.front().unit;
			}
			for (size_t candidate = draft.deliveries.size() + 1;; ++candidate)
			{
				delivery.id = "delivery" + std::to_string(candidate);
				auto const taken = std::any_of(draft.deliveries.begin(), draft.deliveries.end(),
					[&](core::ResourceDeliverySettings const& existing) { return existing.id == delivery.id; });
				if (!taken) break;
			}
			draft.deliveries.push_back(std::move(delivery));
		}},
	{"Waste ejection", "Waste ejection",
		[](core::StationSystemsSettings const& draft) { return draft.wasteEjections.size() < core::MaxStationWasteEjections; },
		[](core::StationSystemsSettings& draft) {
			core::WasteEjectionSettings ejection;
			ejection.quantity = 10.0;
			if (!draft.stores.empty())
			{
				ejection.storeId = draft.stores.front().id;
				ejection.unit = draft.stores.front().unit;
			}
			for (size_t candidate = draft.wasteEjections.size() + 1;; ++candidate)
			{
				ejection.id = "ejection" + std::to_string(candidate);
				auto const taken = std::any_of(draft.wasteEjections.begin(), draft.wasteEjections.end(),
					[&](core::WasteEjectionSettings const& existing) { return existing.id == ejection.id; });
				if (!taken) break;
			}
			draft.wasteEjections.push_back(std::move(ejection));
		}},
	{"Outage", "Outage",
		[](core::StationSystemsSettings const& draft) { return draft.outages.size() < core::MaxStationInstallationOutages; },
		[](core::StationSystemsSettings& draft) {
			core::InstallationOutageSettings outage;
			outage.capacityFraction = 0.5;
			outage.firstOccurrenceSeconds = 60.0;
			outage.durationSeconds = 30.0;
			outage.recurrenceSeconds = 0.0;
			// Default the target to the last-registered wired installation so a
			// freshly drafted outage points at something that exists.
			if (!draft.generators.empty())
				outage.installationId = draft.generators.back().id;
			else if (!draft.coolantLoops.empty())
				outage.installationId = draft.coolantLoops.back().id;
			else if (!draft.chillers.empty())
				outage.installationId = draft.chillers.back().id;
			for (size_t candidate = draft.outages.size() + 1;; ++candidate)
			{
				outage.id = "outage" + std::to_string(candidate);
				auto const taken = std::any_of(draft.outages.begin(), draft.outages.end(),
					[&](core::InstallationOutageSettings const& existing) { return existing.id == outage.id; });
				if (!taken) break;
			}
			draft.outages.push_back(std::move(outage));
		}},
	{"Maintenance", "Maintenance job",
		[](core::StationSystemsSettings const& draft) { return draft.maintenanceJobs.size() < core::MaxStationMaintenanceJobs; },
		[](core::StationSystemsSettings& draft) {
			core::MaintenanceJobSettings job;
			job.firstOccurrenceSeconds = 60.0;
			job.durationSeconds = 30.0;
			// Point the freshly drafted job at an installation that exists.
			if (!draft.generators.empty())
				job.installationId = draft.generators.back().id;
			else if (!draft.coolantLoops.empty())
				job.installationId = draft.coolantLoops.back().id;
			else if (!draft.chillers.empty())
				job.installationId = draft.chillers.back().id;
			// And at a store that exists, so a single click gives a valid
			// reusable-tool requirement; consumed streams need a waste store.
			if (!draft.stores.empty())
			{
				core::MaintenanceRequirementSettings requirement;
				requirement.storeId = draft.stores.front().id;
				requirement.quantity = 1.0;
				requirement.consumed = false;
				job.requirements.push_back(std::move(requirement));
			}
			for (size_t candidate = draft.maintenanceJobs.size() + 1;; ++candidate)
			{
				job.id = "maint" + std::to_string(candidate);
				auto const taken = std::any_of(draft.maintenanceJobs.begin(),
					draft.maintenanceJobs.end(),
					[&](core::MaintenanceJobSettings const& existing) { return existing.id == job.id; });
				if (!taken) break;
			}
			draft.maintenanceJobs.push_back(std::move(job));
		}},
}};
}

void renderAddEntryCombo(core::StationSystemsSettings& draft)
{
	ImGui::SetNextItemWidth(256);
	if (!ImGui::BeginCombo("Add entry", "Choose type...", ImGuiComboFlags_HeightLarge)) return;
	std::string_view group;
	for (auto const& type : core::stationPlaceableTypes())
	{
		auto creator = std::find_if(kEntryCreators.begin(), kEntryCreators.end(),
			[&](auto const& entry) { return entry.nodeType == type.nodeType; });
		if (creator == kEntryCreators.end()) continue;
		if (group != type.group)
		{
			group = type.group;
			ImGui::SeparatorText(type.group.data());
		}
		bool const available = creator->canAdd(draft);
		ImGui::BeginDisabled(!available);
		if (ImGui::Selectable(creator->label)) creator->add(draft);
		ImGui::EndDisabled();
		if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Entry limit reached");
	}
	ImGui::EndCombo();
}

}
