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
	ImGui::TextWrapped("Slice 2b: authored finite Resource stores and bounded transfers; no production or consumption yet. Transfers move only qualified material above protected reserves, respect destination capacity, and allocate competing requests by priority then declaration order. Observation interval is a provisional display/event setting, not a separate simulation clock.");
	auto const authored = world->getStationSystemsSettings();
	ImGui::Text("Authored start conditions: %s, observation interval %.2f s, %zu resource store(s), %zu transfer(s)",
		authored.enabled ? "enabled" : "disabled", authored.observationIntervalSeconds,
		authored.stores.size(), authored.transfers.size());
	ImGui::SeparatorText("Draft settings (seconds; provisional model assumptions)");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	ImGui::Checkbox("Enable Station systems", &mDraft.enabled);
	ImGui::InputFloat("Observation interval (seconds)", &mDraft.observationIntervalSeconds, 0.1f, 1.0f, "%.2f");
	for (size_t index = 0; index < mDraft.stores.size(); ++index)
	{
		auto& store = mDraft.stores[index];
		auto const row = std::to_string(index);
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
			mDraft.stores.erase(mDraft.stores.begin() + static_cast<std::ptrdiff_t>(index));
		ImGui::PopItemWidth();
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
	for (size_t index = 0; index < mDraft.transfers.size(); ++index)
	{
		auto& transfer = mDraft.transfers[index];
		auto const row = std::to_string(index);
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
			mDraft.transfers.erase(mDraft.transfers.begin() + static_cast<std::ptrdiff_t>(index));
		ImGui::PopItemWidth();
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
		ImGui::Text("%s (%s): usable %.3f, held %.3f of capacity %.3f | initial usable %.3f, initial held %.3f | protected %.3f | last tick in %.3f, out %.3f",
			store.id.c_str(), store.unit.c_str(), store.currentQualified, store.currentUnqualified,
			store.capacity, store.startingQualified, store.startingUnqualified,
			store.protectedReserve, store.lastTickIn, store.lastTickOut);
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
	ImGui::SeparatorText("Boundary balances (residual must stay within tolerance)");
	if (snapshot.balances.empty()) ImGui::TextDisabled("No Resource stores authored.");
	for (auto const& balance : snapshot.balances)
		ImGui::Text("Balance %s: starting %.3f, current %.3f, residual %.9f (tolerance %.1e)",
			balance.unit.c_str(), balance.totalStarting, balance.totalCurrent,
			balance.residual, snapshot.balanceTolerance);
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
