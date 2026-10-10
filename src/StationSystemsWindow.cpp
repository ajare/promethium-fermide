#include "StationSystemsWindow.h"
#include "core/World.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"

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
	ImGui::TextWrapped("Lifecycle slice only: no resource production, consumption or endurance model yet. Observation interval is a provisional display/event setting, not a separate simulation clock.");
	auto const authored = world->getStationSystemsSettings();
	ImGui::Text("Authored start conditions: %s, observation interval %.2f s",
		authored.enabled ? "enabled" : "disabled", authored.observationIntervalSeconds);
	ImGui::SeparatorText("Draft settings (seconds; provisional model assumptions)");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	ImGui::Checkbox("Enable Station systems", &mDraft.enabled);
	ImGui::InputFloat("Observation interval (seconds)", &mDraft.observationIntervalSeconds, 0.1f, 1.0f, "%.2f");
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
