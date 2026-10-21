#include "StationSystemsWindow.h"
#include "StationSystemsEntryEditor.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationProtection.h"
#include <filesystem>
#include "core/World.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>

namespace
{
	// In-memory draft snapshots deliberately accept incomplete configurations.
	// Applying/validating a draft and persisting a World are separate operations.
	struct StationDraftSnapshot : DocumentSnapshotContext
	{
		core::StationSystemsSettings settings;
		core::StationConfigurationDocument layout;
		explicit StationDraftSnapshot(core::StationSystemsSettings const& value) : settings(value) {}
	};
}

DocumentSnapshot StationSystemsWindow::captureDraft() const
{
	auto snapshot = mDraftHistory.capture({});
	auto state = std::make_shared<StationDraftSnapshot>(mDraft);
	mFlow.exportLayout(state->layout);
	snapshot.context = std::move(state);
	return snapshot;
}

void StationSystemsWindow::commitDraftChanges(bool force)
{
	if (!mPendingDraftEdit || (!force && (ImGui::IsAnyItemActive()
		|| (mPendingLayoutEdit && ImGui::IsMouseDown(ImGuiMouseButton_Left))))) return;
	auto const before = std::static_pointer_cast<StationDraftSnapshot>(mPendingDraftEdit->context);
	if (before->settings != mDraft) station_flow::pruneConnectionProperties(mDraft);
	core::StationConfigurationDocument layout;
	mFlow.exportLayout(layout);
	if (before->settings != mDraft || (mPendingLayoutEdit
		&& (before->layout.positions != layout.positions || before->layout.panX != layout.panX
			|| before->layout.panY != layout.panY || before->layout.zoom != layout.zoom)))
		mDraftHistory.commit(std::move(mPendingDraftEdit));
	mPendingDraftEdit.reset();
	mPendingLayoutEdit = false;
}

void StationSystemsWindow::renderDraftHistory()
{
	commitDraftChanges();
	auto restore = [&](DocumentSnapshot const& snapshot) {
		auto state = std::dynamic_pointer_cast<StationDraftSnapshot>(snapshot.context);
		if (!state) return false;
		mDraft = state->settings;
		mFlow.restoreLayout(state->layout);
		mPendingLayoutEdit = false;
		mConfirmReset = false;
		mDiagnostic.clear();
		return true;
	};
	bool const undoKey = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z);
	bool const redoKey = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y);
	ImGui::BeginDisabled(!mDraftHistory.canUndo());
	bool const undo = ImGui::Button("Undo configuration");
	ImGui::EndDisabled(); ImGui::SameLine();
	ImGui::BeginDisabled(!mDraftHistory.canRedo());
	bool const redo = ImGui::Button("Redo configuration");
	ImGui::EndDisabled();
	bool didHistory = false;
	if (undo || undoKey) didHistory = mDraftHistory.undo(captureDraft(), restore);
	if (redo || redoKey) didHistory = mDraftHistory.redo(captureDraft(), restore) || didHistory;
	// A rewind/restore changes the current state out from under the pending
	// pre-edit snapshot; resync it to the restored draft so the end-of-frame
	// commit never mistakes the restore for a fresh edit and wipes the redo stack.
	if (didHistory) mPendingDraftEdit = captureDraft();
	else if (!mPendingDraftEdit) mPendingDraftEdit = captureDraft();
}

void StationSystemsWindow::render(std::shared_ptr<core::World> const& world, DocumentHistory& history)
{
	if (!visible) return;
	if (ImGui::Begin("Station Systems", &visible)) renderContents(world, history);
	ImGui::End();
}

bool StationSystemsWindow::syncWorld(std::shared_ptr<core::World> const& world)
{
	if (!world)
	{
		mWorld.reset(); mConfirmReset = false; mPendingActivation.reset();
		mFlowSnapshot.reset();
		mObservationRevision = {~uint64_t{0}, 0};
		mPendingLayoutEdit = false;
		mFlow.reset(); mDraftHistory.clear(); mPendingDraftEdit.reset();
		ImGui::TextDisabled("No World is open."); return false;
	}
	if (mWorld.lock() != world->getLifetimeToken().lock())
	{
		mWorld = world->getLifetimeToken(); mDraft = world->getStationSystemsSettings();
		mFlowSnapshot.reset();
		mObservationRevision = {~uint64_t{0}, 0};
		mPendingLayoutEdit = false;
		mDiagnostic.clear(); mConfirmReset = false; mPendingActivation.reset();
		mFlow.reset(); mDraftHistory.clear(); mPendingDraftEdit.reset();
		mDraftHistory.markSaved();
		mFilepath.clear(); mHasSavedDocument = false; mSaveFailed = false;
	}
	return true;
}

bool StationSystemsWindow::draftDirty() const
{
	if (mSaveFailed) return true;
	core::StationConfigurationDocument document;
	document.settings = mDraft;
	mFlow.exportLayout(document);
	if (!mHasSavedDocument)
		return mDraftHistory.isModified() || document.settings != core::StationSystemsSettings{}
			|| !document.positions.empty() || document.panX != 24 || document.panY != 24
			|| document.zoom != 0.8f;
	return document.settings != mSavedDocument.settings
		|| document.positions != mSavedDocument.positions
		|| document.panX != mSavedDocument.panX || document.panY != mSavedDocument.panY
		|| document.zoom != mSavedDocument.zoom;
}

bool StationSystemsWindow::saveDraft(bool saveAs)
{
	std::string path = mFilepath;
	if (saveAs || path.empty())
	{
		if (!mFilePicker) return false;
		std::optional<std::string> chosen;
		try { chosen = mFilePicker(true); }
		catch (std::exception const& error) { mDiagnostic = error.what(); return false; }
		if (!chosen) return false;
		path = *chosen;
	}
	core::StationConfigurationDocument document;
	document.settings = mDraft;
	mFlow.exportLayout(document);
	try
	{
		core::saveStationConfiguration(path, document);
		mFilepath = std::move(path);
		mSavedDocument = std::move(document); mHasSavedDocument = true;
		mDraftHistory.markSaved(); mSaveFailed = false;
		mDiagnostic.clear();
		try { mRecent.add(mFilepath); }
		catch (std::exception const& error) { mDiagnostic = error.what(); }
		return true;
	}
	catch (std::exception const& error) { mDiagnostic = error.what(); mSaveFailed = true; return false; }
}

void StationSystemsWindow::openDraft(std::string const& path)
{
	core::StationConfigurationDocument candidate;
	if (!core::loadStationConfiguration(path, candidate, &mDiagnostic))
	{
		try { (void)mRecent.removeUnavailable(path); } catch (std::exception const&) {}
		return;
	}
	mDraft = candidate.settings;
	mFlow.importLayout(candidate);
	mPendingLayoutEdit = false;
	mDraftHistory.clear(); mPendingDraftEdit.reset(); mDraftHistory.markSaved();
	mFilepath = path;
	mSavedDocument = std::move(candidate); mHasSavedDocument = true; mSaveFailed = false;
	try { mRecent.add(path); } catch (std::exception const& error) { mDiagnostic = error.what(); }
}

void StationSystemsWindow::loadConfigurationResource(StationConfigurationResource const& resource)
{
	// Copy into the draft, never mutate the shared resource or apply to World.
	mDraft = resource.document().settings;
	mFlow.importLayout(resource.document());
	mPendingLayoutEdit = true;
	mDiagnostic.clear();
}

void StationSystemsWindow::newDraft()
{
	mDraft = core::StationSystemsSettings{};
	mPendingLayoutEdit = false;
	mFlow.reset(); mDraftHistory.clear(); mPendingDraftEdit.reset();
	mDraftHistory.markSaved(); mFilepath.clear(); mHasSavedDocument = false;
	mSaveFailed = false; mDiagnostic.clear();
}

void StationSystemsWindow::renderFileToolbar()
{
	if (ImGui::Button("New configuration"))
	{
		commitDraftChanges(true);
		if (draftDirty()) { mPendingNew = true; mConfirmDiscard = true; }
		else newDraft();
	}
	ImGui::SameLine();
	if (ImGui::Button("Open configuration"))
	{
		if (mFilePicker)
		{
			std::optional<std::string> chosen;
			try { chosen = mFilePicker(false); }
			catch (std::exception const& error) { mDiagnostic = error.what(); }
			if (chosen)
			{
				commitDraftChanges(true);
				if (draftDirty()) { mPendingOpen = *chosen; mConfirmDiscard = true; }
				else openDraft(*chosen);
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Save configuration")) { commitDraftChanges(true); saveDraft(false); }
	ImGui::SameLine();
	if (ImGui::Button("Save configuration as...")) { commitDraftChanges(true); saveDraft(true); }
	ImGui::SameLine(); ImGui::TextDisabled("%s%s", mFilepath.empty() ? "Untitled" : mFilepath.c_str(),
		draftDirty() ? " *" : "");

	ImGui::SetNextItemWidth(256);
	if (ImGui::BeginCombo("Recent configurations", "Open recent..."))
	{
		for (auto const& path : mRecent.entries())
			if (ImGui::MenuItem(path.c_str()))
			{
				commitDraftChanges(true);
				if (draftDirty()) { mPendingOpen = path; mConfirmDiscard = true; }
				else openDraft(path);
				break;
			}
		ImGui::EndCombo();
	}
	if (mConfirmDiscard)
	{
		ImGui::TextWrapped("Discard unsaved Station configuration changes?");
		if (ImGui::Button("Discard configuration changes"))
		{
			mConfirmDiscard = false;
			if (mPendingNew) newDraft();
			else if (mPendingOpen) openDraft(*mPendingOpen);
			mPendingNew = false; mPendingOpen.reset();
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep configuration"))
		{ mConfirmDiscard = false; mPendingNew = false; mPendingOpen.reset(); }
	}
	if (!mDiagnostic.empty()) ImGui::TextWrapped("Configuration: %s", mDiagnostic.c_str());
}

void StationSystemsWindow::prepareObservation(core::World const& world)
{
	auto const revision = world.getStationSystemsObservationRevision();
	if (revision.run != mObservationRevision.run || mObservationCatalogue.empty())
	{
		mObservationCatalogue = world.getStationSystemsObservationCatalogue();
	}
	if ((revision.run != mObservationRevision.run || mPreparedChart != mSelectedChart)
		&& std::none_of(mObservationCatalogue.begin(), mObservationCatalogue.end(),
			[&](auto const& chart) { return chart.id == mSelectedChart; }))
	{ mSelectedChart = "power"; mObservationFallback = true; }
	if (revision == mObservationRevision && mPreparedChart == mSelectedChart) return;
	mFlowChart = world.queryStationSystemsChart(mSelectedChart);
	mFlowRecording = world.getStationSystemsRecordingStatus();
	mObservationRevision = revision; mPreparedChart = mSelectedChart;
	++mObservationPreparations;
	auto const selected = std::find_if(mObservationCatalogue.begin(), mObservationCatalogue.end(),
		[&](auto const& chart) { return chart.id == mSelectedChart; });
	auto const unitSuffix = std::string(" (") + core::stationUnitLabel(selected->unit) + ")";
	mObservationAxis = selected->label.ends_with(unitSuffix) ? selected->label : selected->label + unitSuffix;
	mObservationLabels.clear();
	for (auto const& line : mFlowChart.series) mObservationLabels.push_back(line.label + "###" + line.id);
}

void StationSystemsWindow::renderObservationPlot(float height, core::World const& world)
{
	prepareObservation(world);
	auto const& charts = mObservationCatalogue;
	auto const found = std::find_if(charts.begin(), charts.end(), [&](auto const& chart) {
		return chart.id == mSelectedChart;
	});
	auto const* selected = found == charts.end() ? &charts.front() : &*found;
	if (found == charts.end())
	{
		mSelectedChart = selected->id;
		mObservationFallback = true;
		prepareObservation(world);
	}
	if (mObservationFallback) ImGui::TextDisabled("Selected property unavailable; showing Power.");
	ImGui::TextDisabled("Applied simulation: %s", world.getName().c_str());
	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##Station observation property", selected->label.c_str()))
	{
		std::string group;
		for (auto const& chart : charts)
		{
			if (!chart.propertyId.empty() && chart.id != chart.propertyId) continue;
			if (group != chart.group) { ImGui::TextDisabled("%s", chart.group.c_str()); group = chart.group; }
			ImGui::PushID(chart.id.c_str());
			if (ImGui::Selectable(chart.label.c_str(), chart.id == mSelectedChart))
			{
				mSelectedChart = chart.id;
				selected = &chart;
				mObservationFallback = false;
				prepareObservation(world);
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (!selected->propertyId.empty())
	{
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo("Breakdown", selected->breakdown.c_str()))
		{
			for (auto const& chart : charts) if (chart.propertyId == selected->propertyId)
			{
				if (ImGui::Selectable(chart.breakdown.c_str(), chart.id == mSelectedChart))
				{
					mSelectedChart = chart.id; selected = &chart;
					prepareObservation(world);
				}
			}
			ImGui::EndCombo();
		}
	}
	if (std::any_of(mFlowChart.series.begin(), mFlowChart.series.end(), [](auto const& s) { return s.optionalRequested; }))
		ImGui::Checkbox("Show requested", &mShowRequested);
	ImGui::TextUnformatted(selected->title.c_str());
	if (!selected->accountingNote.empty()) ImGui::TextWrapped("Accounting coverage: %s", selected->accountingNote.c_str());
	bool const empty = std::all_of(mFlowChart.series.begin(), mFlowChart.series.end(),
		[](auto const& line) { return line.times.empty(); });
	if (!mFlowSnapshot || !mFlowSnapshot->enabled)
		ImGui::TextDisabled("Simulation disabled; no live history.");
	else if (mFlowRecording.limited)
		ImGui::TextDisabled("RECORDING LIMIT: complete catalogue cannot fit; no metrics recorded.");
	else if (empty)
		ImGui::TextDisabled("No samples yet; run the simulation.");
	else if (std::none_of(mFlowChart.series.begin(), mFlowChart.series.end(), [](auto const& line) {
		return std::any_of(line.values.begin(), line.values.end(), [](double v) { return std::isfinite(v); });
	}))
		ImGui::TextDisabled("Unavailable: incomplete observations or zero denominator.");
	else
		ImGui::TextDisabled("%sSimulation time; stride %.2f s%s%s", mFlowWasPaused ? "Paused; " : "",
			mFlowChart.sampleIntervalSeconds, mFlowChart.truncated ? " (interval summaries; history merged or dropped)" : "",
			mFlowChart.incomplete ? " (incomplete coverage)" : "");

	if (!empty)
		ImGui::TextDisabled("Retained %.2f - %.2f s | %zu samples maximum | %zu bytes | coarsening %u%s",
			mFlowRecording.retainedFromSeconds, mFlowRecording.retainedUntilSeconds,
			mFlowRecording.sampleLimit, mFlowRecording.recordingBytes, mFlowRecording.coarseningLevel,
			mFlowChart.incomplete ? " | incomplete accounting or observations" : " | no gaps");

	// A private context also makes this production view usable by CPU-only
	// editor workflows, without depending on the Launcher's global context.
	auto* previous = ImPlot::GetCurrentContext();
	if (!mPowerPlotContext)
		mPowerPlotContext = std::shared_ptr<ImPlotContext>(ImPlot::CreateContext(),
			[](ImPlotContext* context) { ImPlot::DestroyContext(context); });
	ImPlot::SetCurrentContext(mPowerPlotContext.get());
	if (ImPlot::BeginPlot("##Station power history", { -1, std::max(130.0f,
		height - 3 * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetFrameHeightWithSpacing()) }, ImPlotFlags_NoMenus))
	{
		ImPlot::SetupAxes("Simulation time (s)", mObservationAxis.c_str(),
			ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
		// ImPlot draws text directly, bypassing ImGui's normal text logging.
		// Include chart labels in the same read-only UI log as other widgets.
		ImGui::LogText("Simulation time (s) | %s\n", mObservationAxis.c_str());
		size_t lineIndex = 0;
		for (auto const& line : mFlowChart.series)
		{
			auto const* label = mObservationLabels[lineIndex++].c_str();
			if (line.optionalRequested && !mShowRequested) continue;
			// Colour and legend identity derive from the metric, not its vector position.
			auto const colour = core::stationSeriesColourKey(line.id);
			ImPlot::SetNextLineStyle(ImPlot::GetColormapColor(static_cast<int>(colour % ImPlot::GetColormapSize())), 2);
			ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2);
			ImGui::LogText("%s\n", line.label.c_str());
			if (line.stacked)
			{
				ImPlot::SetNextFillStyle(ImPlot::GetColormapColor(static_cast<int>(colour % ImPlot::GetColormapSize())), 0.55f);
				ImPlot::PlotShaded(label, line.times.data(), line.lower.data(), line.upper.data(), static_cast<int>(line.times.size()));
			}
			else ImPlot::PlotLine(label, line.times.data(), line.values.data(), static_cast<int>(line.times.size()));
		}
		ImPlot::EndPlot();
	}
	ImPlot::SetCurrentContext(previous);
}

void StationSystemsWindow::renderFlowContents(std::shared_ptr<core::World> const& world)
{
	if (!syncWorld(world)) return;
	// Groups keep the existing control IDs (tables introduce an ID scope).
	// Narrow the header's work region only; restore it before the canvas.
	auto* host = ImGui::GetCurrentWindow();
	auto const workRight = host->WorkRect.Max.x;
	auto const contentRight = host->ContentRegionRect.Max.x;
	auto const headerOrigin = ImGui::GetCursorScreenPos();
	float const width = ImGui::GetContentRegionAvail().x;
	float const controlsWidth = std::max(1.0f, width * 0.62f);
	ImGui::BeginGroup();
	ImGui::BeginGroup();
	host->WorkRect.Max.x = headerOrigin.x + controlsWidth;
	host->ContentRegionRect.Max.x = host->WorkRect.Max.x;
	renderFileToolbar();
	renderDraftHistory();
	bool const paused = world->isSimulationPaused();
	auto const& applied = world->getStationSystemsSettings();
	double const now = ImGui::GetTime();
	// Poll the authoritative snapshot at 4 Hz. Reset, configuration changes,
	// and pausing bypass the throttle so old-run values cannot linger.
	if (!mFlowSnapshot || now < mFlowRefreshTime
		|| (now - mFlowRefreshTime >= 0.25 && world->getSimulationTick() != mFlowSnapshot->worldTick)
		|| world->getStationSystemsObservationRevision().run != mObservationRevision.run || paused != mFlowWasPaused
		|| world->getSimulationTick() < mFlowSnapshot->worldTick)
	{
		mFlowSnapshot = world->getStationSystemsSnapshot();
		mFlowRefreshTime = now;
		mFlowWasPaused = paused;
	}
	mFlow.render(mDraft, [this](core::StationSystemsSettings& draft) {
		renderFlowInspectorContents(draft);
	}, [this] { commitDraftChanges(true); }, [this] {
		commitDraftChanges(true);
		mPendingDraftEdit = captureDraft();
	}, [this] {
		if (mPendingLayoutEdit) return;
		commitDraftChanges(true);
		mPendingDraftEdit = captureDraft();
		mPendingLayoutEdit = true;
	}, &*mFlowSnapshot, mDraft == applied, paused, [&] {
		float const height = std::max(180.0f, ImGui::GetCursorScreenPos().y - headerOrigin.y);
		host->WorkRect.Max.x = workRight;
		host->ContentRegionRect.Max.x = contentRight;
		ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::SetCursorScreenPos({headerOrigin.x + controlsWidth + ImGui::GetStyle().ItemSpacing.x,
			headerOrigin.y});
		ImGui::BeginGroup();
		renderObservationPlot(height, *world);
		ImGui::EndGroup();
		ImGui::EndGroup();
	});
	// Placement is one action, including on the mouse-down frame. An
	// inspector field edit is not: while a field is still being edited the
	// pending snapshot stays open, so typing "1250" is one commit at field
	// commit, not four (#653). The deactivation frame lands here once no
	// item is active.
	commitDraftChanges();
}

void StationSystemsWindow::renderFlowInspectorContents(core::StationSystemsSettings& draft)
{
	auto const selected = mFlow.selectedNode();
	if (!selected)
	{
		ImGui::TextDisabled("Nothing selected.");
		ImGui::TextDisabled("Click a node on the canvas to edit its settings.");
		return;
	}
	ImGui::Text("Node: %s '%s'", selected->type.c_str(), selected->id.c_str());
	auto const* editor = station_editor::findStationEntryEditor(selected->type);
	if (!editor)
	{
		// A type with no extracted editor says so, by name, rather than
		// showing nothing and leaving the user guessing whether the node or
		// the editor is broken.
		ImGui::TextDisabled("No settings editor for '%s' nodes yet.", selected->type.c_str());
		ImGui::TextDisabled("Editing for this type arrives with its entry-editor extraction.");
		return;
	}
	if (selected->index < 0)
	{
		ImGui::TextDisabled("This node does not map to a single editable entry.");
		return;
	}
	ImGui::Separator();
	// The same seam the form tab uses (#652), fields only: removal stays a
	// list-surface operation, and the inspector owns no list.
	editor->renderEntry(draft, size_t(selected->index));
	ImGui::Separator();
	ImGui::TextDisabled("Edits change the Configuration draft only. Nothing is applied to the World from here.");
}

void StationSystemsWindow::renderContents(std::shared_ptr<core::World> const& world, DocumentHistory& history)
{
	if (!syncWorld(world)) return;
	renderDraftHistory();
	// Activation is a World-wide authored gate, not a draft-only checkbox or
	// a side effect of opening this tab. Disabled Worlds own no station runtime.
	ImGui::BeginDisabled(!world->isSimulationPaused());
	auto active = world->getStationSystemsSettings().enabled;
	auto activate = [&](bool enabled, bool confirm)
	{
		auto settings = world->getStationSystemsSettings();
		settings.enabled = enabled;
		auto before = captureDocumentSnapshot(world, history);
		if (world->configureStationSystems(settings, confirm, &mDiagnostic))
		{
			commitDocumentEdit(std::move(before), history);
			mDraft.enabled = enabled;
			mPendingActivation.reset();
			mConfirmReset = false;
		}
	};
	if (ImGui::Checkbox("Activate Station systems", &active))
	{
		if (world->getSimulationTick()) mPendingActivation = active;
		else activate(active, false);
	}
	if (mPendingActivation)
	{
		ImGui::TextWrapped("Changing activation resets the complete World simulation, including Agent movement. Confirm?");
		if (ImGui::Button("Confirm activation reset")) activate(*mPendingActivation, true);
		ImGui::SameLine();
		if (ImGui::Button("Cancel activation reset")) mPendingActivation.reset();
	}
	ImGui::EndDisabled();
	if (!world->isSimulationPaused())
		ImGui::TextDisabled("Pause the World to change Station systems activation.");
	if (!world->getStationSystemsSettings().enabled)
		ImGui::TextDisabled("Inactive: no Station systems simulation runs.");
	ImGui::Text("Active World: %s", world->getName().c_str());
	ImGui::Text("World status: %s", world->isSimulationPaused() ? "Paused" : "Running");
	ImGui::Text("World elapsed: %.3f seconds (tick %llu)",
		world->getSimulationTick() * static_cast<double>(core::World::getFixedTimestep()),
		static_cast<unsigned long long>(world->getSimulationTick()));
	ImGui::Text("Document: %s", world->isModified() ? "unsaved changes" : "saved");
	ImGui::TextWrapped("Slice 5: authored finite Resource stores, bounded transfers, compound-resource conversions, solar generation, External equipment load supply, rechargeable batteries, thermal reservoirs with radiator rejection, coolant loops with freeze-protection heating, and chillers supplying class-matched cold services. Transfers move only qualified material above protected reserves, respect destination capacity, and allocate competing requests by priority then declaration order. Compound stores declare constituent compositions on a dry-mass basis; conversions are validated to never create tracked constituents. Solar output follows installed capacity and the illumination/eclipse profile; zero capacity disables an array. External loads declare demand, operating profile, heat, thermal class, material exchanges, a protected critical floor, priority and allowed curtailment; allocated power never exceeds generation plus battery output and cooling-infeasible loads are not admitted. Batteries separate nameplate and usable energy: the reserve floor (nameplate minus usable) is never dischargeable, charge and discharge efficiencies book their losses, discharge covers deficits on the same allocation pool as generation, and charging takes only surplus. Thermal reservoirs integrate temperature from their effective capacity under absorbed and routed equipment heat; radiators reject heat by the Stefan-Boltzmann law at the emitting temperature against the authored sink, emissivity and view factor. A load's emitted heat is booked exactly once: routed into its class-compatible reservoir or exported directly, never both. Declared temperature limits are reported, never clamped. Coolant loops hold a finite, loop-specific coolant inventory (coolant is never potable or nutrient water, and loops never pool by class (#752)); the pump circulates only the coolant the loop actually holds, so an underfilled loop flows less and a dry loop flows nothing, and the freeze-protection heater thermostats the loop up to its setpoint within its capacity. Pump work and heater heat enter the loop exactly once and reach the reservoir only through the circulation-gated heat-only exchanger; the loop draw rides the same electrical pool as external loads. Chillers supply a cold service of their declared class through a class-matched coolant-loop evaporator and reservoir condenser: the delivered duty is capped by the loop's actual circulation, gated at the chilled setpoint, and refused while the sink is at or above the maximum condensing temperature; work at the configured COP plus auxiliaries rides the same electrical pool, and condenser heat equals extracted heat plus the work and auxiliaries actually supplied, booked once. Fuelled generators convert a finite energy-unit fuel inventory into bus electrical output at the declared efficiency: output is bounded by the installed rating and the ramp limit, starting only completes once the startup energy has actually been drawn from the shared pool while the class-matched sink is below its maximum sink temperature, and after output stops the configured shutdown heat and its cooling requirement persist for the decay period; waste, startup and decay heat route exactly once into the sink and the fuel ledger closes over fuel = electrical + waste. Within a tick, generators and fuel-cell backup units produce first, then power allocates, then coolant loops and chillers run, then compartment atmospheres cycle, crews metabolise and compartments exchange, then thermal integrates, then transfers, then conversions. Compartment atmospheres model an ideal-gas mixture in a fixed volume: absolute and partial pressures derive from the gas inventory and temperature, humidity caps at Magnus saturation with the excess condensing and releasing its latent heat into the compartment, occupancy is an authored load assignment whose oxygen draw is capped by the inventory (CO2 and metabolic heat scale with what was actually consumed), leakage is a constant total-mass rate capped by the inventory, and scheduled airlock cycles remove authored mass with an explicit recovered-versus-lost split; every gram leaving a compartment is booked as leaked, recovered or lost, compartments never mix automatically, and declared pressure and gas-quality limits are reported, never clamped. Crews are an authored population with an activity profile, never derived from World Agents: metabolism draws food and water from compound stores and oxygen from the compartment, and returns CO2, moisture and metabolic heat to the compartment and urine and organic waste to their own finite compound stores. One uniform metabolic scale per tick - the scarcest supply or tightest waste headroom - throttles every pathway together, so absent material never generates nominal waste; requested versus actual is explicit per pathway, the authored coefficients must close over mass, constituents and energy, and the consumed mass the returns do not carry away is reported as explicit retained body mass. Observation interval is a provisional display/event setting, not a separate simulation clock.");
	auto const authored = world->getStationSystemsSettings();
	ImGui::Text("Authored start conditions: %s, step ratio %u World tick(s), dispatch %s, observation interval %.2f s, %zu resource store(s), %zu transfer(s), %zu conversion(s), %zu solar array(s), %zu external load(s), %zu battery(ies), %zu resupply delivery(ies), %zu thermal reservoir(s), %zu radiator(s), %zu coolant loop(s), %zu chiller(s), %zu generator(s), %zu atmosphere compartment(s), %zu airlock cycle(s), %zu crew(s), %zu water collector(s), %zu water treatment(s), %zu electrolyzer(s), %zu CO2 capture machine(s), %zu fuel-cell backup unit(s), %zu thermal buffer(s), %zu terminal-waste ejection(s), %zu Sabatier reactor(s), %zu methane processor(s), %zu microbial processor(s), %zu crop population(s), %zu algae culture(s), %zu installation outage(s) and %zu maintenance job(s)",
		authored.enabled ? "enabled" : "disabled", authored.systemsStepWorldTicks,
		authored.dispatchMode == core::StationDispatchMode::Automatic ? "automatic" : "manual",
		authored.observationIntervalSeconds,
		authored.stores.size(), authored.transfers.size(), authored.conversions.size(),
		authored.solarArrays.size(), authored.equipmentLoads.size(), authored.batteries.size(),
		authored.deliveries.size(), authored.thermalReservoirs.size(), authored.radiators.size(),
		authored.coolantLoops.size(), authored.chillers.size(), authored.generators.size(),
		authored.atmospheres.size(), authored.airlockCycles.size(), authored.crews.size(),
		authored.waterCollectors.size(), authored.waterTreatments.size(),
		authored.electrolyzers.size(), authored.co2Captures.size(),
		authored.fuelCells.size(), authored.thermalBuffers.size(),
		authored.wasteEjections.size(),
		authored.sabatierReactors.size(), authored.methaneProcessors.size(),
		authored.microbialProcessors.size(), authored.cropPopulations.size(),
		authored.algaeCultures.size(), authored.outages.size(),
		authored.maintenanceJobs.size());
	// #418: the authored trend retention, in simulation seconds.
	ImGui::Text("Trend retention: %.0f s (bounded per-series samples, downsampled on long runs)",
		authored.trendRetentionSeconds);
	// Selection previews a managed document without editing the draft. Only the
	// explicit load action copies its settings and layout into draft history.
	ImGui::SeparatorText("Reference configurations");
	StationConfigurationResource const* selectedConfiguration = nullptr;
	for (auto const& resource : mConfigurationResources)
		if (resource->getQualifiedName() == mSelectedConfiguration)
			selectedConfiguration = resource.get();
	ImGui::BeginDisabled(mConfigurationResources.empty());
	if (ImGui::BeginCombo("Configuration resource",
		selectedConfiguration ? mSelectedConfiguration.c_str() : "Select a configuration"))
	{
		for (auto const& resource : mConfigurationResources)
		{
			auto const name = resource->getQualifiedName();
			bool const selected = name == mSelectedConfiguration;
			if (ImGui::Selectable(name.c_str(), selected))
			{
				mSelectedConfiguration = name;
				selectedConfiguration = resource.get();
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	if (selectedConfiguration)
	{
		ImGui::PushID(mSelectedConfiguration.c_str());
		if (auto const& reference = selectedConfiguration->reference())
		{
			auto const& doc = *reference;
			ImGui::Text("%s - %.0f simulated s scenario", doc.name.c_str(), doc.scenarioSeconds);
			ImGui::TextWrapped("%s", doc.summary.c_str());
			ImGui::TextWrapped("Scenario notes: %s", doc.scenarioNotes.c_str());
			if (!doc.provenance.empty() && ImGui::TreeNodeEx("Coefficient provenance",
				ImGuiTreeNodeFlags_NoAutoOpenOnLog))
			{
				for (auto const& entry : doc.provenance)
					ImGui::TextWrapped("%s = %s %s - %s", entry.coefficient.c_str(),
						entry.value.c_str(), entry.units.c_str(), entry.source.c_str());
				ImGui::TreePop();
			}
			if (!doc.assumptions.empty() && ImGui::TreeNodeEx("Provisional assumptions",
				ImGuiTreeNodeFlags_NoAutoOpenOnLog))
			{
				for (auto const& [topic, text] : doc.assumptions)
					if (ImGui::TreeNodeEx(topic.c_str(), ImGuiTreeNodeFlags_NoAutoOpenOnLog))
					{
						ImGui::TextWrapped("%s", text.c_str());
						ImGui::TreePop();
					}
				ImGui::TreePop();
			}
			auto expectations = [](char const* label, auto const& entries) {
				if (!entries.empty() && ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoAutoOpenOnLog))
				{
					for (auto const& entry : entries)
						ImGui::TextWrapped("%s: %s", entry.item.c_str(), entry.detail.c_str());
					ImGui::TreePop();
				}
			};
			expectations("Expected limiting services", doc.limitingServices);
			expectations("Expected key event ordering", doc.eventOrdering);
			expectations("Expected bounded outcome ranges", doc.outcomeRanges);
			if (doc.scenarioKind == "stability")
				ImGui::TextWrapped("Declared residual tolerance: %.0e relative per ledger scale (core::StationBalanceTolerance) - every closing conservation ledger must stay inside it at every point of the run; the audit reports any residual beyond it, never absorbs it.",
					core::StationBalanceTolerance);
			ImGui::TextWrapped("Qualification: %s", doc.qualification.c_str());
		}
		else
		{
			// Plain configuration examples carry their descriptions in the leading
			// YAML comments rather than the reference document metadata fields.
			std::string description;
			std::string_view text = selectedConfiguration->text();
			while (!text.empty())
			{
				auto const end = text.find('\n');
				auto line = text.substr(0, end);
				if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
				if (!line.empty() && line.front() != '#') break;
				if (!line.empty())
				{
					line.remove_prefix(1);
					if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
					description.append(line);
					description += '\n';
				}
				if (end == std::string_view::npos) break;
				text.remove_prefix(end + 1);
			}
			if (description.empty()) ImGui::TextDisabled("No description provided.");
			else ImGui::TextWrapped("%s", description.c_str());
		}
		ImGui::PopID();
	}
	else ImGui::TextDisabled("No reference configuration selected.");
	ImGui::BeginDisabled(!selectedConfiguration || !world->isSimulationPaused());
	if (ImGui::Button("Load into draft")) loadConfigurationResource(*selectedConfiguration);
	ImGui::EndDisabled();
	if (!world->isSimulationPaused())
		ImGui::TextDisabled("Pause the World to load the selected configuration into the draft.");
	ImGui::SeparatorText("Draft settings (seconds; provisional model assumptions)");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	station_editor::renderAddEntryCombo(mDraft);
	ImGui::InputScalar("Station step ratio (World ticks)", ImGuiDataType_U32,
		&mDraft.systemsStepWorldTicks);
	auto const stepSeconds = static_cast<double>(mDraft.systemsStepWorldTicks)
		* static_cast<double>(core::World::getFixedTimestep());
	ImGui::Text("Configured Station step: %u World tick(s), %.6f s",
		mDraft.systemsStepWorldTicks, stepSeconds);
	ImGui::Text("Certified protection ratio: 1 to %u World tick(s)",
		core::StationProtectionCertifiedStepRatio);
	if (mDraft.systemsStepWorldTicks <= core::StationProtectionCertifiedStepRatio)
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
			"Within certified protection ratio.");
	else
		ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
			"BEYOND CERTIFIED protection ratio: configured %u World tick(s) exceeds %u; results are not certified.",
			mDraft.systemsStepWorldTicks, core::StationProtectionCertifiedStepRatio);
	ImGui::TextDisabled("Whole World ticks per Station step (1 to %u).", core::CORE_MAX_SYSTEMS_STEP_WORLD_TICKS);
	ImGui::InputFloat("Observation interval (seconds)", &mDraft.observationIntervalSeconds, 0.1f, 1.0f, "%.2f");
	// #418: the authored trend retention. Trends are sampled on the same
	// observation cadence (no second clock) and this bounds the retained
	// simulation-time window; the per-series sample budget is fixed, so
	// long or accelerated runs downsample rather than grow storage.
	ImGui::InputFloat("Trend retention (seconds)", &mDraft.trendRetentionSeconds, 60.0f, 600.0f, "%.0f");
	// #415: the dispatch controller mode. Manual keeps the authored per-source
	// commanded outputs authoritative; Automatic lets the dispatcher command
	// generators and fuel-cell backups each tick to cover the net load the sun
	// leaves, so batteries bridge transients instead of funding sustained
	// deficits. No new clock: the controller runs on the World tick.
	int dispatchModeIndex = mDraft.dispatchMode == core::StationDispatchMode::Automatic ? 1 : 0;
	if (ImGui::RadioButton("Dispatch: manual (authored commands)", &dispatchModeIndex, 0))
		mDraft.dispatchMode = core::StationDispatchMode::Manual;
	ImGui::SameLine();
	if (ImGui::RadioButton("Dispatch: automatic (bridge losses, start backup, protect reserves)", &dispatchModeIndex, 1))
		mDraft.dispatchMode = core::StationDispatchMode::Automatic;
	// #652: Store and Transfer editing renders through the shared entry
	// editor seam so the Flow inspector reuses the exact same widgets,
	// labels and deferred-removal behavior against this same draft.
	station_editor::renderStoreList(mDraft);
	ImGui::SeparatorText("Draft transfers (qualified material, same-unit stores)");
	station_editor::renderTransferList(mDraft);
	ImGui::SeparatorText("Draft conversions (compound material; declared yields may lose but never create constituents)");
	for (size_t index = 0; index < mDraft.conversions.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeConversion = station_editor::renderConversionEntry(mDraft, mDraft.conversions[index],
			"Conversion " + row, "Remove conversion " + row);
		if (removeConversion)
			mDraft.conversions.erase(mDraft.conversions.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft solar arrays (kW; illumination is a fraction of full sun, 0..1)");
	for (size_t index = 0; index < mDraft.solarArrays.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeArray = station_editor::renderSolarEntry(mDraft, mDraft.solarArrays[index],
			"Solar " + row, "Remove solar array " + row);
		if (removeArray)
			mDraft.solarArrays.erase(mDraft.solarArrays.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft external equipment loads (kW; heat and material exchanges scale with delivered power)");
	station_editor::renderEquipmentLoadList(mDraft);
	ImGui::SeparatorText("Draft batteries (kWh; usable energy is the cyclable slice above the reserve floor = nameplate - usable)");
	station_editor::renderBatteryList(mDraft);
	ImGui::SeparatorText("Draft resupply deliveries (store units at simulation seconds; recurrence 0 is a one-off)");
	for (size_t index = 0; index < mDraft.deliveries.size(); )
	{
		auto& delivery = mDraft.deliveries[index];
		auto const row = std::to_string(index);
		bool const removeDelivery = station_editor::renderDeliveryEntry(mDraft, delivery,
			"Delivery " + row, "Remove delivery " + row);
		if (removeDelivery)
			mDraft.deliveries.erase(mDraft.deliveries.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft terminal-waste ejections (#403; scheduled removals of disposal-ready (qualified) material only; held/untreated waste stays behind; what is ejected is booked as external loss; recurrence 0 is a one-off)");
	for (size_t index = 0; index < mDraft.wasteEjections.size(); )
	{
		auto& ejection = mDraft.wasteEjections[index];
		auto const row = std::to_string(index);
		bool const removeEjection = station_editor::renderWasteEjectionEntry(mDraft, ejection,
			"Waste ejection " + row, "Remove waste ejection " + row);
		if (removeEjection)
			mDraft.wasteEjections.erase(mDraft.wasteEjections.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft thermal reservoirs (kJ/K, degrees C; limits are declared service limits, absorbed heat is environmental)");
	// #656: the reservoir's settings live in the shared entry editor so
	// the Flow inspector renders the exact same fields against the same
	// draft; the form keeps only the section heading.
	station_editor::renderThermalReservoirList(mDraft);
	ImGui::SeparatorText("Draft radiators (m2; rejection = emissivity x view factor x area x sigma x (T^4 - T_sink^4))");
	// #657: the radiator's settings live in the shared entry editor so
	// the Flow inspector renders the exact same fields against the same
	// draft; the form keeps only the section heading.
	station_editor::renderRadiatorList(mDraft);
	ImGui::SeparatorText("Draft coolant loops (kg coolant; finite loop-specific classless inventory, heat-only reservoir coupling; zero starting coolant disables the loop)");
	for (size_t index = 0; index < mDraft.coolantLoops.size(); )
	{
		auto& loop = mDraft.coolantLoops[index];
		auto const row = std::to_string(index);
		bool const removeLoop = station_editor::renderCoolantLoopEntry(mDraft, loop,
			"Coolant loop " + row, "Remove coolant loop " + row);
		if (removeLoop)
			mDraft.coolantLoops.erase(mDraft.coolantLoops.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft chillers (kW cold service; class-matched loop evaporator and reservoir condenser, COP and auxiliaries; zero rated capacity disables the chiller)");
	for (size_t index = 0; index < mDraft.chillers.size(); )
	{
		auto& chiller = mDraft.chillers[index];
		auto const row = std::to_string(index);
		bool const removeChiller = station_editor::renderChillerEntry(mDraft, chiller,
			"Chiller " + row, "Remove chiller " + row);
		if (removeChiller)
			mDraft.chillers.erase(mDraft.chillers.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft generators (kW/kWh; fuel is an energy-unit Resource store; startup energy and a qualified cooling sink gate the start; zero installed capacity disables the source)");
	station_editor::renderGeneratorList(mDraft);
	ImGui::SeparatorText("Draft installation outages (#413; a scheduled capacity reduction of one registered installation - generator, coolant loop or chiller; the capacity fraction is the share of rated capacity left available during the window, [0,1); recurrence 0 is a one-off, a positive recurrence repeats the window and must be at least as long as it; overlapping outages on one installation take the deepest reduction; restoration only lifts the ceiling and never replenishes any inventory)");
	for (size_t index = 0; index < mDraft.outages.size(); )
	{
		auto& outage = mDraft.outages[index];
		auto const row = std::to_string(index);
		bool const removeOutage = station_editor::renderOutageEntry(mDraft, outage,
			"Outage " + row, "Remove outage " + row);
		if (removeOutage)
			mDraft.outages.erase(mDraft.outages.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft maintenance jobs (#414; scheduled timed work on a registered installation - completion must be paid for with declared tools, spare parts and consumables; reusable tools are reserved and returned, consumed material lands in the declared waste store; the installation is isolated while the work is active; recurrence 0 is a one-off, a positive recurrence must be at least the duration; absent prerequisites never allow free completion)");
	for (size_t index = 0; index < mDraft.maintenanceJobs.size(); )
	{
		auto& job = mDraft.maintenanceJobs[index];
		auto const row = std::to_string(index);
		bool const removeJob = station_editor::renderMaintenanceEntry(mDraft, job,
			"Maintenance job " + row, "Remove maintenance job " + row);
		if (removeJob)
			mDraft.maintenanceJobs.erase(
				mDraft.maintenanceJobs.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft atmospheres (m3/kg/C/kPa; ideal-gas partial pressures; occupancy is an authored load assignment; vapour caps at saturation and condenses with latent heat; leak is a constant total-mass rate; limits are reported, never clamped)");
	for (size_t index = 0; index < mDraft.atmospheres.size(); )
	{
		auto const row = std::to_string(index);
		if (station_editor::renderAtmosphereEntry(mDraft, mDraft.atmospheres[index],
			"Atmosphere " + row, "Remove atmosphere " + row))
			mDraft.atmospheres.erase(mDraft.atmospheres.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
	}
	ImGui::SeparatorText("Draft airlock cycles (kg/seconds; scheduled removals on the shared scenario schedule order; the recovered fraction is retained in the receiver, the remainder is lost through the outer hatch)");
	for (size_t index = 0; index < mDraft.airlockCycles.size(); )
	{
		auto const row = std::to_string(index);
		if (station_editor::renderAirlockCycleEntry(mDraft, mDraft.airlockCycles[index],
			"Airlock cycle " + row, "Remove airlock cycle " + row))
			mDraft.airlockCycles.erase(mDraft.airlockCycles.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
	}
	ImGui::SeparatorText("Draft crews (authored population; per-crew metabolic rates per second at full activity; food/water/urine/waste are the referenced compound store's units, gases kg/s, heat kW; crew are never derived from World Agents)");
	for (size_t index = 0; index < mDraft.crews.size(); )
	{
		auto& crew = mDraft.crews[index];
		auto const row = std::to_string(index);
		bool removeCrew = station_editor::renderCrewEntry(mDraft, crew,
			"Crew " + row, "Remove crew " + row);
		if (removeCrew)
			mDraft.crews.erase(mDraft.crews.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft water collectors (#402; a collector may serve a compartment of any class - condensate is just water (#752); the cold surface needs coolant circulation and must be colder than the compartment air; latent heat routes into the coolant loop once; destination must be a plain mass store)");
	for (size_t index = 0; index < mDraft.waterCollectors.size(); )
	{
		auto const row = std::to_string(index);
		if (station_editor::renderWaterCollectorEntry(mDraft, mDraft.waterCollectors[index],
			"Collector " + row, "Remove collector " + row))
			mDraft.waterCollectors.erase(mDraft.waterCollectors.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
	}
	ImGui::SeparatorText("Draft water treatments (#402; raw water splits into treated water plus retained concentrate for declared electricity and consumables; full rated power qualifies the output, curtailed power holds it unqualified; water stores must be plain mass stores, the consumable store may be compound)");
	for (size_t index = 0; index < mDraft.waterTreatments.size(); )
	{
		auto const row = std::to_string(index);
		if (station_editor::renderWaterTreatmentEntry(mDraft, mDraft.waterTreatments[index],
			"Treatment " + row, "Remove treatment " + row))
			mDraft.waterTreatments.erase(mDraft.waterTreatments.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
	}
	ImGui::SeparatorText("Draft electrolyzers (#404; qualified water splits into oxygen plus hydrogen at the declared stoichiometry; the electrical dose must cover the chemical energy the hydrogen retains, the difference is process heat into the class-matched sink; oxygen goes to a gas store OR a compartment, never both; product stores cap production instead of venting)");
	for (size_t index = 0; index < mDraft.electrolyzers.size(); )
	{
		auto const row = std::to_string(index);
		if (station_editor::renderElectrolyzerEntry(mDraft, mDraft.electrolyzers[index],
			"Electrolyzer " + row, "Remove electrolyzer " + row))
			mDraft.electrolyzers.erase(mDraft.electrolyzers.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
	}
	ImGui::SeparatorText("Draft CO2 capture machines (#404; a compartment's carbon dioxide is scrubbed into a finite capture store as inventory; capture is finite on rate, available CO2, store headroom and consumable; the unmet atmospheric service stays visible; a tick with no power scrubs nothing)");
	for (size_t index = 0; index < mDraft.co2Captures.size(); )
	{
		auto const row = std::to_string(index);
		if (station_editor::renderCo2CaptureEntry(mDraft, mDraft.co2Captures[index],
			"CO2 capture " + row, "Remove CO2 capture " + row))
			mDraft.co2Captures.erase(mDraft.co2Captures.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
	}
	ImGui::SeparatorText("Draft fuel-cell backup units (#406; dispatchable power from two dedicated, finite reactant stores; output is bounded by the installed rating, the setpoint, the reactants drawable above their protected reserves and the product water store's headroom; waste heat needs a class-matched qualified sink; product water lands in the treatment destination, never drinking water directly)");
	station_editor::renderFuelCellList(mDraft);
	ImGui::SeparatorText("Draft Sabatier reactors (#405; captured CO2 plus hydrogen convert into methane plus water at the declared stoichiometry; the reaction is exothermic and the dose plus released heat route into the class-matched sink; the water destination is the raw-water store treatment qualifies; product stores cap conversion instead of venting)");
	station_editor::renderSabatierList(mDraft);
	ImGui::SeparatorText("Draft methane processors (#405; methane splits into recovered hydrogen plus solid carbon at the declared stoichiometry; the reaction is endothermic and the electrical dose must cover the declared reaction heat demand, the difference is process heat into the class-matched sink; the recovered hydrogen returns to the hydrogen store as recycled feed - the reactor consumes twice as much per methane, so the loop keeps a net hydrogen demand; the carbon destination is the configured useful store or terminal waste)");
	station_editor::renderMethaneProcessorList(mDraft);
	ImGui::SeparatorText("Draft microbial processors (#407/#408; the digestion closes on mass - feed + oxygen + process water = nutrients + recovered water + CO2 + residue - with the residue as the balance term; the culture runs only inside the declared operating window; the condition/recovery coefficients below are provisional biological assumptions: starvation, consumable exhaustion, cold stress and heat stress degrade the aggregate culture condition, which throttles processing; recovery is bounded by the declared rate and, when a replacement biological stock store is declared, by the stock drawn above its protected reserve)");
	for (size_t index = 0; index < mDraft.microbialProcessors.size(); )
	{
		auto& processor = mDraft.microbialProcessors[index];
		auto const row = std::to_string(index);
		bool removeProcessor = station_editor::renderMicrobialProcessorEntry(mDraft, processor,
			"Microbial processor " + row, "Remove microbial processor " + row);
		if (removeProcessor)
			mDraft.microbialProcessors.erase(mDraft.microbialProcessors.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft crop populations (#409/#410; dry-mass living biomass on a declared composition with no moisture and no atmospheric nitrogen; growth is funded by delivered lamp light only after the essential circulation draw, plus CO2, irrigation water and each tracked nutrient, and only inside the declared growing window; respiration burns dry organic for oxygen and returns CO2, vapour and heat; the canopy stops at declared capacity and never photosynthesises for free in darkness or without power; the harvest contract (trigger, fraction, edible share, food and waste stores) removes real living material and preserves its declared constituents - destinations must be mass stores whose declared composition matches the crop's; 0 trigger disables the harvest - all biological coefficients are provisional assumptions)");
	for (size_t index = 0; index < mDraft.cropPopulations.size(); )
	{
		auto& crop = mDraft.cropPopulations[index];
		auto const row = std::to_string(index);
		bool removeCrop = station_editor::renderCropPopulationEntry(mDraft, crop,
			"Crop " + row, "Remove crop " + row);
		if (removeCrop)
			mDraft.cropPopulations.erase(mDraft.cropPopulations.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft algae cultures (#411; a separate Biological population with its own living biomass, active culture volume and density-per-litre capacity, coefficients and condition - never shared with crops; growth is funded by delivered lamp light only after the essential culture circulation draw, plus CO2, culture water and each tracked nutrient, and only inside the declared growing window; respiration burns dry organic for oxygen and returns CO2, vapour and heat; the culture stops at capacity and never photosynthesises for free in darkness or without power; the harvest contract (trigger, fraction, food and recovery shares, food/recovery/waste stores) removes real living material and partitions it conservatively into the three declared destinations - each destination's surplus cascades to the next and what none can hold stays living; all three destinations must be mass stores whose declared composition matches the culture's; 0 trigger disables the harvest - all biological coefficients are provisional assumptions)");
	for (size_t index = 0; index < mDraft.algaeCultures.size(); )
	{
		auto& algae = mDraft.algaeCultures[index];
		auto const row = std::to_string(index);
		bool removeAlgae = station_editor::renderAlgaeCultureEntry(mDraft, algae,
			"Algae " + row, "Remove algae " + row);
		if (removeAlgae)
			mDraft.algaeCultures.erase(mDraft.algaeCultures.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
	ImGui::SeparatorText("Draft thermal buffers (kJ; finite phase-change store that rides through a cooling deficit)");
	for (size_t index = 0; index < mDraft.thermalBuffers.size(); )
	{
		auto const row = std::to_string(index);
		bool const remove = station_editor::renderThermalBufferEntry(mDraft,
			mDraft.thermalBuffers[index], "Buffer " + row, "Remove buffer " + row);
		if (remove) mDraft.thermalBuffers.erase(mDraft.thermalBuffers.begin() + static_cast<std::ptrdiff_t>(index));
		else ++index;
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
	commitDraftChanges();
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
			store.id.c_str(), store.unit.c_str(),
			store.currentQualified, store.currentUnqualified,
			store.capacity, store.startingQualified, store.startingUnqualified,
			store.protectedReserve, store.lastTickIn, store.lastTickOut);
		if (store.composition)
			ImGui::Text("  compound (dry-mass basis): moisture %.4f | nutrient N %.4f, atmospheric N %.4f, P %.4f, K %.4f, other minerals %.4f | %.1f kJ/kg dry",
				store.composition->moisture, store.composition->nutrientNitrogen,
				store.composition->atmosphericNitrogen, store.composition->phosphorus,
				store.composition->potassium, store.composition->otherMinerals,
				store.composition->energyKJPerKgDry);
		for (auto const& [name, concentration] : store.concentrations)
			ImGui::Text("  live %s concentration: %.6g kg/kg", name.c_str(), concentration);
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
		if (transfer.mode == core::ResourceTransferMode::Scheduled)
		{
			ImGui::Text("%s: Scheduled %s -> %s | %s | requested %.3f, moved %.3f, unmet %.3f %s (priority %d)",
				transfer.id.c_str(), transfer.sourceId.c_str(), transfer.destinationId.c_str(),
				transfer.hasNextOccurrence ? "waiting for next occurrence" : transfer.lastUnmet > 0 ? "completed one-off (blocked/partial)" : "completed one-off",
				transfer.lastRequested, transfer.lastCompleted, transfer.lastUnmet, transfer.unit.c_str(), transfer.priority);
			ImGui::Text("  occurrence count %llu | last outcome: %s | %s",
				static_cast<unsigned long long>(transfer.occurrencesFired),
				transfer.occurrencesFired == 0 ? "none" : transfer.lastUnmet == 0 ? "full"
					: transfer.lastCompleted == 0 ? "blocked" : "partial", transfer.limitingReason.c_str());
			ImGui::Text("  cumulative requested %.3f, moved %.3f, unmet %.3f %s",
				transfer.cumulativeRequested, transfer.cumulativeMoved, transfer.cumulativeUnmet, transfer.unit.c_str());
			if (transfer.hasNextOccurrence) ImGui::Text("  next occurrence at %.6f s", transfer.nextOccurrenceSeconds);
			continue;
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
		// #417: the load's recovery stage. A purely discretionary load that
		// fell below its minimum operable supply is held Shed until the
		// dwell, hysteresis, cooling and firm-source gate reopens it; a
		// Restoring load is drawing its startup surge. Loads with a
		// protected floor are never recovery-managed and always show Active.
		if (load.recoveryStage != core::StationLoadRecoveryStage::Active)
		{
			char const* stage = load.recoveryStage == core::StationLoadRecoveryStage::Shed
				? "SHED" : "RESTORING";
			ImGui::Text("  recovery %s: %.1f s remaining, surge %.3f kW%s%s",
				stage, load.recoverySecondsRemaining, load.recoverySurgeKW,
				!load.recoveryHoldReason.empty() ? " | held: " : "",
				!load.recoveryHoldReason.empty() ? load.recoveryHoldReason.c_str() : "");
		}
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
	// #415: the dispatch controller's own account for this tick.
	ImGui::Text("Dispatch (%s): demand %.3f kW, target %.3f kW, committed %.3f kW, shortfall %.3f kW | alarms: backup %s, reserve %s",
		snapshot.power.dispatchMode == core::StationDispatchMode::Automatic ? "automatic" : "manual",
		snapshot.power.dispatchDemandKW, snapshot.power.dispatchTargetKW,
		snapshot.power.dispatchCommittedKW, snapshot.power.dispatchShortfallKW,
		snapshot.power.backupEngaged ? "ENGAGED" : "off",
		snapshot.power.reserveExhausted ? "EXHAUSTED" : "ok");
	// #416: the protected critical service admission for this tick.
	// Containment circulation, biological circulation and authored
	// critical cooling are drawn ahead of every discretionary external
	// load from the actual pool; anything the pool cannot deliver stays
	// visible here as unmet critical minimum, never reclassified as
	// permissible curtailment. Shutdown-cooling entries show the standing
	// decay-heat obligation that books even when generation output stops.
	ImGui::SeparatorText("Protected critical services (#416; drawn ahead of discretionary loads from actual services, not nameplate totals)");
	ImGui::Text("Protected totals: demand %.3f kW, supplied %.3f kW, unmet critical %.3f kW | alarm: %s",
		snapshot.power.protectedDemandKW, snapshot.power.protectedSuppliedKW,
		snapshot.power.protectedUnmetKW,
		snapshot.power.protectedUnmet ? "UNMET CRITICAL MINIMA" : "ok");
	if (snapshot.power.protectedServices.empty())
		ImGui::TextDisabled("No protected service demand this tick (no critical loops, biology or critical chillers operating).");
	for (auto const& service : snapshot.power.protectedServices)
	{
		ImGui::Text("%s '%s': protected demand %.3f kW, supplied %.3f kW, unmet %.3f kW%s%s",
			service.kind.c_str(), service.id.c_str(), service.requestedKW,
			service.suppliedKW, service.unmetKW,
			service.unmetKW > 1e-12 ? " | reason: " : "",
			service.unmetKW > 1e-12 ? service.reason.c_str() : "");
	}
	// #417: the recovery staging summary. While any purely discretionary
	// load is Shed or Restoring the station is in a recovery stage; the
	// startup surge those loads draw is held out of battery charging so
	// depleted reserves refill only from surplus the recovery does not
	// need, never overloading a service that is still coming back.
	ImGui::SeparatorText("Service recovery staging (#417; shed loads restart in safe stages via declared dwell, hysteresis, cooling and firm-source gates; surge held out of reserve charging)");
	ImGui::Text("Recovery: %s | restoring %d load(s), surge this tick %.3f kW",
		snapshot.power.recoveryPending ? "PENDING (station in a recovery stage)" : "steady state",
		snapshot.power.recoveringCount, snapshot.power.recoverySurgeKW);
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
			delivery.arrivesQualified ? "arrives usable" : "arrives held unqualified");
		if (delivery.exhausted)
			ImGui::Text("  one-off schedule spent: no further arrivals");
		else
			ImGui::Text("  next arrival at %.3f s (%s)", delivery.nextOccurrenceSeconds,
				delivery.recurrenceSeconds > 0.0
					? ("recurring every " + std::to_string(delivery.recurrenceSeconds) + " s").c_str()
					: "one-off");
	}
	ImGui::SeparatorText("Terminal-waste ejections (live; only disposal-ready (qualified) material is ejected, held/untreated waste stays behind, ejected material is booked as external loss; shortfalls are outcomes, never failures)");
	if (snapshot.wasteEjections.empty()) ImGui::TextDisabled("No terminal-waste ejections authored.");
	else if (snapshot.hasNextWasteEjection)
		ImGui::Text("Next ejection due at %.3f s", snapshot.nextWasteEjectionSeconds);
	for (auto const& ejection : snapshot.wasteEjections)
	{
		char const* status = "idle";
		switch (ejection.status)
		{
		case core::StationTransferStatus::Full: status = "ejected"; break;
		case core::StationTransferStatus::Partial: status = "partially-ejected"; break;
		case core::StationTransferStatus::Rejected: status = "nothing-ejectable"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s <- %s (%s): %s | last at %.3f s: %.3f of %.3f ejected, %.3f not ejectable | total ejected %.3f, unmet %.3f | %llu occurrence(s) | priority %d",
			ejection.id.c_str(), ejection.storeId.c_str(), ejection.unit.c_str(), status,
			ejection.lastOccurrenceSeconds, ejection.lastEjected, ejection.lastScheduled,
			ejection.lastUnmet, ejection.ejectedTotal, ejection.unmetTotal,
			static_cast<unsigned long long>(ejection.occurrencesCompleted), ejection.priority);
		ImGui::Text("  eligibility: %s | qualified/held last %.3f / %.3f; total %.3f / %.3f",
			ejection.eligibility == core::WasteEjectionEligibility::HeldOnly ? "held only"
				: ejection.eligibility == core::WasteEjectionEligibility::QualifiedAndHeld ? "qualified then held" : "qualified only",
			ejection.lastQualifiedEjected, ejection.lastHeldEjected,
			ejection.qualifiedEjectedTotal, ejection.heldEjectedTotal);
		if (ejection.exhausted)
			ImGui::Text("  one-off schedule spent: no further ejections");
		else
			ImGui::Text("  next ejection at %.3f s (%s)", ejection.nextOccurrenceSeconds,
				ejection.recurrenceSeconds > 0.0
					? ("recurring every " + std::to_string(ejection.recurrenceSeconds) + " s").c_str()
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
	ImGui::Text("Thermal totals: absorbed %.3f kW, routed %.3f kW, rejected %.3f kW, circulation %.3f kg/s, freeze heating %.3f kW, cold provided %.3f kW, cold unmet %.3f kW, condenser heat %.3f kW | buffers stored %.3f kJ, available %.3f kJ, charging %.3f kW, discharging %.3f kW",
		snapshot.thermal.totalAbsorbedKW, snapshot.thermal.totalRoutedKW,
		snapshot.thermal.totalRejectedKW, snapshot.thermal.totalCirculationKgPerS,
		snapshot.thermal.totalFreezeHeatingKW, snapshot.thermal.totalCoolingProvidedKW,
		snapshot.thermal.totalCoolingUnmetKW, snapshot.thermal.totalCondenserHeatKW,
		snapshot.thermal.totalBufferStoredKJ, snapshot.thermal.totalBufferAvailableCapacityKJ,
		snapshot.thermal.totalBufferChargeKW, snapshot.thermal.totalBufferDischargeKW);
	ImGui::SeparatorText("Thermal buffers (live; finite phase-change store riding through the host reservoir's cooling deficit; charge capped by the deficit, discharge by the rejection surplus; access circulation-gated or passive; full = no headroom left to absorb, depleted = nothing left to return)");
	if (snapshot.thermalBuffers.empty()) ImGui::TextDisabled("No thermal buffers authored.");
	for (auto const& buffer : snapshot.thermalBuffers)
	{
		char const* status = "idle";
		switch (buffer.status)
		{
		case core::StationTransferStatus::Full: status = "full"; break;
		case core::StationTransferStatus::Partial: status = "partial"; break;
		case core::StationTransferStatus::Rejected: status = "blocked"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s -> %s [%s]: %s | stored %.3f of %.3f kJ (%.0f%% full) | buffer %.3f C (band %.3f..%.3f C) | charge %.3f of %.3f kW, discharge %.3f of %.3f kW | access %s%s%s",
			buffer.id.c_str(), buffer.reservoirId.c_str(), buffer.accessMode.c_str(), status,
			buffer.storedEnergyKJ, buffer.capacityKJ, buffer.fillFraction * 100.0,
			buffer.bufferTemperatureC, buffer.minUsableTemperatureC, buffer.maxUsableTemperatureC,
			buffer.lastChargeKW, buffer.maxChargeKW, buffer.lastDischargeKW, buffer.maxDischargeKW,
			buffer.accessible ? "open" : "gated",
			buffer.full ? " | EXHAUSTED (no headroom)" : "",
			buffer.depleted ? " | DEPLETED (nothing to return)" : "");
		ImGui::Text("  energy ledger (kJ): starting %.3f, charged %.3f, discharged %.3f, current %.3f, residual %.9f (tolerance %.1e) | limiter: %s",
			buffer.startingEnergyKJ, buffer.chargedEnergyKJ, buffer.dischargedEnergyKJ,
			buffer.storedEnergyKJ, buffer.residualKJ, snapshot.balanceTolerance,
			buffer.lastLimiter.empty() ? "-" : buffer.lastLimiter.c_str());
	}
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
		ImGui::Text("%s -> %s: %s | coolant %.3f of %.3f kg (%.0f%% full) | flow %.3f of %.3f kg/s (circulation %.1f%%) | pump %.3f of %.3f kW | heater %.3f of %.3f kW",
			loop.id.c_str(), loop.reservoirId.c_str(), status,
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
	ImGui::SeparatorText("Installation outages (live; the window is open on the shared World time base, the target's effective availability is the deepest reduction among its open outages, and a restoration only lifts the ceiling - no inventory is replenished)");
	if (snapshot.outages.empty()) ImGui::TextDisabled("No installation outages authored.");
	for (auto const& outage : snapshot.outages)
	{
		auto const recurrence = outage.recurrenceSeconds > 0.0
			? std::to_string(outage.recurrenceSeconds) + " s (recurring)"
			: std::string("0 (one-off)");
		ImGui::Text("%s on %s '%s': capacity fraction %.3f | first %.3f s, duration %.3f s, recurrence %s | priority %d",
			outage.id.c_str(), outage.installationKind.c_str(), outage.installationId.c_str(),
			outage.capacityFraction, outage.firstOccurrenceSeconds, outage.durationSeconds,
			recurrence.c_str(), outage.priority);
		ImGui::Text("  %s | started %llu, completed %llu | last start %.3f s, last restore %.3f s | target availability now %.3f",
			outage.active ? "WINDOW OPEN" : "window closed",
			static_cast<unsigned long long>(outage.occurrencesStarted),
			static_cast<unsigned long long>(outage.occurrencesCompleted),
			outage.lastStartSeconds, outage.lastRestoreSeconds,
			outage.installationAvailability);
		if (outage.exhausted)
			ImGui::Text("  exhausted: the one-off outage has restored and will not recur");
		else if (outage.hasNextOccurrence)
			ImGui::Text("  next occurrence at %.3f s", outage.nextOccurrenceSeconds);
	}
	ImGui::SeparatorText("Maintenance jobs (live; work runs only while every prerequisite is reserved; the target installation is isolated while active; consumed material is booked into the waste store and reusable tools are returned - restoration never replenishes supplies)");
	if (snapshot.maintenanceJobs.empty()) ImGui::TextDisabled("No maintenance jobs authored.");
	for (auto const& job : snapshot.maintenanceJobs)
	{
		auto const phaseName = static_cast<size_t>(job.phase) < core::StationMaintenancePhaseNames.size()
			? core::StationMaintenancePhaseNames[static_cast<size_t>(job.phase)] : "?";
		ImGui::Text("%s on %s '%s': %s%s | progress %.3f / %.3f s",
			job.id.c_str(), job.installationKind.c_str(), job.installationId.c_str(),
			phaseName, job.isolated ? " [ISOLATED]" : "",
			job.progressSeconds, job.durationSeconds);
		if (!job.blockReason.empty())
			ImGui::Text("  blocking: %s", job.blockReason.c_str());
		for (auto const& requirement : job.requirements)
		{
			auto const line = std::string("  ") + requirement.storeId + " "
				+ std::to_string(requirement.quantity) + " " + requirement.unit
				+ (requirement.consumed ? " consumed" : " reusable")
				+ ": " + (requirement.reserved ? "reserved" : "not reserved")
				+ (requirement.lastConsumed > 0.0
					? " | last spent " + std::to_string(requirement.lastConsumed)
					: std::string());
			ImGui::Text("%s", line.c_str());
		}
		ImGui::Text("  started %llu, completed %llu, missed %llu | last start %.3f s, last complete %.3f s",
			static_cast<unsigned long long>(job.occurrencesStarted),
			static_cast<unsigned long long>(job.occurrencesCompleted),
			static_cast<unsigned long long>(job.occurrencesMissed),
			job.lastStartSeconds, job.lastCompleteSeconds);
		if (job.exhausted)
			ImGui::Text("  exhausted: the schedule has no occurrence left");
		else if (job.hasNextOccurrence)
			ImGui::Text("  next window at %.3f s", job.nextOccurrenceSeconds);
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
		ImGui::Text("%s -> %s: %s | pressure %.3f kPa (start %.3f) | O2 %.3f, N2 %.3f, CO2 %.3f, H2O %.3f kg (start %.3f/%.3f/%.3f/%.3f) | temp %.3f C (start %.3f C)",
			atmosphere.id.c_str(),
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
		ImGui::Text("  exterior pathway losses (kg): O2 %.6f, N2 %.6f, CO2 %.6f, vapour %.6f",
			atmosphere.exteriorLostKg[0], atmosphere.exteriorLostKg[1],
			atmosphere.exteriorLostKg[2], atmosphere.exteriorLostKg[3]);
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
		ImGui::Text("%s (%s -> %s via loop '%s', class %s): %s | cold surface %s, circulation %.3f",
			collector.id.c_str(), collector.atmosphereId.c_str(),
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
		ImGui::Text("%s (%s -> %s + %s, consumables from %s): %s | scale %.3f%s%s",
			treatment.id.c_str(), treatment.sourceStoreId.c_str(),
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
		ImGui::Text("  gates: filtration %s | treatment evidence %s",
			treatment.filtrationCompleted ? "ran" : "not run",
			treatment.treatmentEvidenced ? "complete (full dose)" : "incomplete");
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
		ImGui::Text("%s (%s -> %s + %s%s): %s | scale %.3f%s%s",
			cell.id.c_str(), cell.waterStoreId.c_str(),
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
		ImGui::Text("%s (%s -> %s%s): %s | scale %.3f%s%s",
			capture.id.c_str(), capture.atmosphereId.c_str(),
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
		ImGui::Text("%s (%s + %s -> %s): %s | commanded %.3f kW%s%s",
			cell.id.c_str(),
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
	ImGui::SeparatorText("Sabatier reactors (live; the conversion closes on mass - CO2 + hydrogen = methane + water - and on energy - supplied + released = routed; the exothermic reaction heat rides into the sink with the dose; a full product store caps conversion, a hot sink refuses the reactor)");
	for (auto const& reactor : snapshot.sabatierReactors)
	{
		char const* status = "idle";
		switch (reactor.status)
		{
		case core::StationTransferStatus::Full: status = "converting at full rate"; break;
		case core::StationTransferStatus::Partial: status = "converting below rated (feeds or headroom short)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing converted)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s (%s + %s -> %s + %s): %s | scale %.3f%s%s",
			reactor.id.c_str(),
			reactor.co2StoreId.c_str(), reactor.hydrogenStoreId.c_str(),
			reactor.methaneStoreId.c_str(), reactor.waterStoreId.c_str(),
			status, reactor.scale,
			reactor.limiter.empty() ? "" : " | limited by: ",
			reactor.limiter.empty() ? "" : reactor.limiter.c_str());
		ImGui::Text("  last tick: CO2 %.6f kg + H2 %.6f kg -> CH4 %.6f kg + water %.6f kg | power %.4f/%.4f kW | unmet conversion %.6f kg | reaction heat %.4f kW | cooling %s%s",
			reactor.lastCo2ConsumedKg, reactor.lastHydrogenConsumedKg,
			reactor.lastMethaneProducedKg, reactor.lastWaterProducedKg,
			reactor.lastPowerSuppliedKW, reactor.lastPowerDemandKW,
			reactor.lastUnmetConversionKg, reactor.lastReactionHeatKW,
			reactor.coolingAvailable ? "qualified" : "NOT qualified",
			reactor.blockReason.empty() ? "" : (std::string(" - ") + reactor.blockReason).c_str());
		ImGui::Text("  cumulative: CO2 %.4f kg | H2 %.4f kg | CH4 %.4f kg | water %.4f kg | energy %.2f kJ supplied + %.2f kJ released heat routed %.2f kJ | residual mass %.2e kg / energy %.2e kJ",
			reactor.co2ConsumedKg, reactor.hydrogenConsumedKg,
			reactor.methaneProducedKg, reactor.waterProducedKg,
			reactor.energySuppliedKJ, reactor.reactionHeatKJ, reactor.heatRoutedKJ,
			reactor.residualKg, reactor.residualKJ);
		if (reactor.disabled)
			ImGui::Text("  zero rated conversion: no production");
	}
	ImGui::SeparatorText("Methane processors (live; the split closes on mass - methane = hydrogen + carbon - and on energy - supplied = absorbed + routed; the recovered hydrogen returns to the hydrogen store as recycled feed, never free supply; a full destination caps processing, a hot sink refuses the processor)");
	for (auto const& processor : snapshot.methaneProcessors)
	{
		char const* status = "idle";
		switch (processor.status)
		{
		case core::StationTransferStatus::Full: status = "processing at full rate"; break;
		case core::StationTransferStatus::Partial: status = "processing below rated (feed or headroom short)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing processed)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s (%s -> %s + %s): %s | scale %.3f%s%s",
			processor.id.c_str(),
			processor.methaneStoreId.c_str(), processor.hydrogenStoreId.c_str(),
			processor.carbonStoreId.c_str(),
			status, processor.scale,
			processor.limiter.empty() ? "" : " | limited by: ",
			processor.limiter.empty() ? "" : processor.limiter.c_str());
		ImGui::Text("  last tick: CH4 %.6f kg -> H2 %.6f kg + C %.6f kg | power %.4f/%.4f kW | unmet processing %.6f kg | reaction heat absorbed %.4f kW | cooling %s%s",
			processor.lastMethaneConsumedKg, processor.lastHydrogenReturnedKg,
			processor.lastCarbonProducedKg,
			processor.lastPowerSuppliedKW, processor.lastPowerDemandKW,
			processor.lastUnmetProcessingKg, processor.lastReactionHeatKW,
			processor.coolingAvailable ? "qualified" : "NOT qualified",
			processor.blockReason.empty() ? "" : (std::string(" - ") + processor.blockReason).c_str());
		ImGui::Text("  cumulative: CH4 %.4f kg | H2 returned %.4f kg | carbon %.4f kg | energy %.2f kJ supplied (absorbed %.2f + routed %.2f) | residual mass %.2e kg / energy %.2e kJ",
			processor.methaneConsumedKg, processor.hydrogenReturnedKg,
			processor.carbonProducedKg,
			processor.energySuppliedKJ, processor.reactionHeatKJ, processor.heatRoutedKJ,
			processor.residualKg, processor.residualKJ);
		if (processor.disabled)
			ImGui::Text("  zero rated processing: no service");
	}
	ImGui::SeparatorText("Microbial processors (live; the digestion closes on mass - feed + oxygen + process water = nutrients + recovered water + CO2 + residue - with the residue as the balance term, and on energy - supplied + released = routed; recovered N/P/K/minerals are funded by the feed's bound nutrients only, atmospheric N never substitutes; the reaction heat is capped by the feed's chemical energy; the culture runs only inside the declared operating window; saturated destinations backpressure the whole stage; the aggregate culture condition degrades under starvation, consumable exhaustion and temperature stress and throttles processing, recovering only through the declared bounded rate and replacement stock - provisional biological assumptions)");
	for (auto const& processor : snapshot.microbialProcessors)
	{
		char const* status = "idle";
		switch (processor.status)
		{
		case core::StationTransferStatus::Full: status = "digesting at full rate"; break;
		case core::StationTransferStatus::Partial: status = "digesting below rated (feeds, destinations or energy short)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing digested)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s (%s -> %s + %s + %s + %s): %s | scale %.3f%s%s",
			processor.id.c_str(),
			processor.feedStoreId.c_str(), processor.nutrientStoreId.c_str(),
			processor.recoveredWaterStoreId.c_str(), processor.co2StoreId.c_str(),
			processor.residueStoreId.c_str(),
			status, processor.scale,
			processor.limiter.empty() ? "" : " | limited by: ",
			processor.limiter.empty() ? "" : processor.limiter.c_str());
		ImGui::Text("  last tick: feed %.6f kg + O2 %.6f kg + water %.6f kg + consumable %.6f -> nutrients %.6f kg + recovered water %.6f kg + CO2 %.6f kg + residue %.6f kg | power %.4f/%.4f kW | unmet %.6f kg | reaction heat %.4f kW | temperature %s%s",
			processor.lastFeedConsumedKg, processor.lastOxygenConsumedKg,
			processor.lastWaterConsumedKg, processor.lastConsumableUsed,
			processor.lastNutrientProducedKg, processor.lastRecoveredWaterKg,
			processor.lastCo2ProducedKg, processor.lastResidueProducedKg,
			processor.lastPowerSuppliedKW, processor.lastPowerDemandKW,
			processor.lastUnmetProcessingKg, processor.lastReactionHeatKW,
			processor.temperatureEligible ? "eligible" : "NOT eligible",
			processor.blockReason.empty() ? "" : (std::string(" - ") + processor.blockReason).c_str());
		ImGui::Text("  cumulative: feed %.4f kg | O2 %.4f kg | water %.4f kg | consumable %.4f | nutrients %.4f kg (N %.4f + P %.4f + K %.4f + minerals %.4f) | recovered water %.4f kg | CO2 %.4f kg | residue %.4f kg | energy %.2f kJ supplied + %.2f kJ released heat routed %.2f kJ | residual mass %.2e kg / energy %.2e kJ",
			processor.feedConsumedKg, processor.oxygenConsumedKg,
			processor.waterConsumedKg, processor.consumablesUsed,
			processor.nutrientProducedKg, processor.nitrogenRecoveredKg,
			processor.phosphorusRecoveredKg, processor.potassiumRecoveredKg,
			processor.mineralsRecoveredKg, processor.recoveredWaterKg,
			processor.co2ProducedKg, processor.residueProducedKg,
			processor.energySuppliedKJ, processor.reactionHeatKJ, processor.heatRoutedKJ,
			processor.residualKg, processor.residualKJ);
		ImGui::Text("  condition %.3f | %s | last change %+.6f | replacement stock: %s consumed %.6f units this tick (%.4f total)",
			processor.condition,
			processor.lastStressors.empty() ? (processor.recovering
				? "benign, recovering" : "benign, at full condition")
				: ("stressed by " + processor.lastStressors).c_str(),
			processor.lastConditionDelta,
			processor.stockStoreId.empty() ? "none configured,"
				: ("drawn from '" + processor.stockStoreId + "',").c_str(),
			processor.lastStockConsumedUnits, processor.stockConsumedUnits);
		if (processor.disabled)
			ImGui::Text("  zero rated feed: no service");
	}
	ImGui::SeparatorText("Crop populations (live; dry-mass living biomass beside declared capacity; growth is funded by delivered lamp light only after the essential circulation draw, plus CO2, irrigation water and each tracked nutrient, and only inside the declared growing window; respiration burns dry organic for oxygen and returns CO2, vapour and heat; the canopy stops at capacity and never photosynthesises for free in darkness or without power; harvest removes real living material at the declared trigger, preserving its declared constituents into the food and waste stores - full destinations constrain the harvest or route the edible surplus through the waste path, and a canopy whose living organic content is respired away dies, its residue routed to waste or booked as explicit loss - provisional biological assumptions)");
	if (snapshot.cropPopulations.empty()) ImGui::TextDisabled("No crop populations authored.");
	for (auto const& crop : snapshot.cropPopulations)
	{
		char const* status = "idle";
		switch (crop.status)
		{
		case core::StationTransferStatus::Full: status = "growing at full funded rate"; break;
		case core::StationTransferStatus::Partial: status = "growing below potential (resources or capacity short)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing grew)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s]: %s | living %.4f / capacity %.4f kg (%.1f%%) | growth scale %.3f%s%s",
			crop.id.c_str(), crop.atmosphereId.c_str(), status,
			crop.livingBiomassKg, crop.capacityKg, crop.capacityFraction * 100.0,
			crop.lastGrowthScale,
			crop.lastGrowthLimiter.empty() ? "" : " | limited by: ",
			crop.lastGrowthLimiter.empty() ? "" : crop.lastGrowthLimiter.c_str());
		ImGui::Text("  last tick: growth %.6f kg (potential %.6f) | CO2 %.6f kg + water %.6f kg + N %.6f + P %.6f + K %.6f + minerals %.6f kg drawn | O2 released %.6f kg | transpired %.6f kg | light %.4f/%.4f kW | circulation %.4f/%.4f kW | respiration %.6f/%.6f kg (O2 drawn %.6f kg, CO2 returned %.6f kg, vapour %.6f kg, heat %.4f kW)%s | heat into compartment %.4f kW | temperature %s%s",
			crop.lastGrowthKg, crop.lastGrowthPotentialKg,
			crop.lastCo2DrawnKg, crop.lastAssimilationWaterKg,
			crop.lastNitrogenDrawnKg, crop.lastPhosphorusDrawnKg,
			crop.lastPotassiumDrawnKg, crop.lastMineralsDrawnKg,
			crop.lastOxygenReleasedKg, crop.lastTranspiredKg,
			crop.lastLightSuppliedKW, crop.lastLightRequestedKW,
			crop.lastCirculationSuppliedKW, crop.lastCirculationRequestedKW,
			crop.lastRespirationKg, crop.lastRespirationRequestedKg,
			crop.lastRespirationO2DrawnKg, crop.lastRespirationCo2ReturnedKg,
			crop.lastRespirationWaterKg, crop.lastRespirationHeatKW,
			crop.lastOxygenLimited ? " [O2-limited respiration]" : "",
			crop.lastHeatIntoCompartmentKW,
			crop.temperatureEligible ? "eligible" : "NOT eligible",
			crop.blockReason.empty() ? "" : (std::string(" - ") + crop.blockReason).c_str());
		ImGui::Text("  cumulative: growth %.4f kg | CO2 %.4f kg | water %.4f kg | N %.4f + P %.4f + K %.4f + minerals %.4f kg | O2 released %.4f kg | transpired %.4f kg | respiration %.4f kg (O2 %.4f kg, CO2 %.4f kg, vapour %.4f kg) | light %.2f kJ + circulation %.2f kJ | chemical fixed %.2f kJ | respiration heat %.2f kJ | heat into compartment %.2f kJ | residual mass %.2e kg / energy %.2e kJ",
			crop.growthKg, crop.co2DrawnKg, crop.waterDrawnKg,
			crop.nitrogenDrawnKg, crop.phosphorusDrawnKg,
			crop.potassiumDrawnKg, crop.mineralsDrawnKg,
			crop.oxygenReleasedKg, crop.transpiredKg,
			crop.respirationKg, crop.respirationO2DrawnKg,
			crop.respirationCo2ReturnedKg, crop.respirationWaterKg,
			crop.lightEnergyKJ, crop.circulationEnergyKJ,
			crop.chemicalEnergyFixedKJ, crop.respirationHeatKJ,
			crop.heatIntoCompartmentKJ,
			crop.residualKg, crop.residualKJ);
		ImGui::Text("  harvest: trigger %.4f kg (fraction %.3f, edible %.3f) -> food '%s' / waste '%s' | last: requested %.6f kg, harvested %.6f kg (food %.6f, waste %.6f, surplus edible routed %.6f, constrained %.6f)%s | cumulative: harvested %.4f kg (food %.4f, waste %.4f, surplus routed %.4f) | dead residue: %.4f kg to waste, %.4f kg lost uncontained%s",
			crop.harvestTriggerKg, crop.harvestFraction, crop.edibleFraction,
			crop.foodStoreId.empty() ? "-" : crop.foodStoreId.c_str(),
			crop.wasteStoreId.empty() ? "-" : crop.wasteStoreId.c_str(),
			crop.lastHarvestRequestedKg, crop.lastHarvestedKg,
			crop.lastFoodAddedKg, crop.lastWasteAddedKg,
			crop.lastSurplusEdibleToWasteKg, crop.lastHarvestConstrainedKg,
			crop.lastHarvestLimiter.empty() ? "" : (" | " + crop.lastHarvestLimiter).c_str(),
			crop.harvestedKg, crop.foodProducedKg, crop.wasteProducedKg,
			crop.surplusEdibleToWasteKg,
			crop.deadResidueToWasteKg, crop.deadResidueLostKg,
			crop.dead ? " | POPULATION DEAD: the living organic content was respired away; nothing recreates it for free" : "");
		if (crop.disabled)
			ImGui::Text("  zero growing area: no service");
	}
	ImGui::SeparatorText("Algae cultures (live; a separate Biological population with its own living biomass beside its own culture-volume capacity; growth is funded by delivered lamp light only after the essential culture circulation draw, plus CO2, culture water and each tracked nutrient, and only inside the declared growing window; respiration burns dry organic for oxygen and returns CO2, vapour and heat; the culture stops at capacity and never photosynthesises for free in darkness or without power; harvest removes real living material at the declared trigger and partitions it conservatively into the food, recovery and waste stores - each destination's surplus cascades to the next, and what none can hold stays living; a culture whose living organic content is respired away dies, its residue routed to waste or booked as explicit loss - provisional biological assumptions)");
	if (snapshot.algaeCultures.empty()) ImGui::TextDisabled("No algae cultures authored.");
	for (auto const& algae : snapshot.algaeCultures)
	{
		char const* status = "idle";
		switch (algae.status)
		{
		case core::StationTransferStatus::Full: status = "growing at full funded rate"; break;
		case core::StationTransferStatus::Partial: status = "growing below potential (resources or capacity short)"; break;
		case core::StationTransferStatus::Rejected: status = "refused (nothing grew)"; break;
		case core::StationTransferStatus::Idle: break;
		}
		ImGui::Text("%s [%s]: %s | living %.4f / culture capacity %.4f kg (%.1f%%) | growth scale %.3f%s%s",
			algae.id.c_str(), algae.atmosphereId.c_str(), status,
			algae.livingBiomassKg, algae.capacityKg, algae.capacityFraction * 100.0,
			algae.lastGrowthScale,
			algae.lastGrowthLimiter.empty() ? "" : " | limited by: ",
			algae.lastGrowthLimiter.empty() ? "" : algae.lastGrowthLimiter.c_str());
		ImGui::Text("  last tick: growth %.6f kg (potential %.6f) | CO2 %.6f kg + water %.6f kg + N %.6f + P %.6f + K %.6f + minerals %.6f kg drawn | O2 released %.6f kg | evaporated %.6f kg | light %.4f/%.4f kW | circulation %.4f/%.4f kW | respiration %.6f/%.6f kg (O2 drawn %.6f kg, CO2 returned %.6f kg, vapour %.6f kg, heat %.4f kW)%s | heat into compartment %.4f kW | temperature %s%s",
			algae.lastGrowthKg, algae.lastGrowthPotentialKg,
			algae.lastCo2DrawnKg, algae.lastAssimilationWaterKg,
			algae.lastNitrogenDrawnKg, algae.lastPhosphorusDrawnKg,
			algae.lastPotassiumDrawnKg, algae.lastMineralsDrawnKg,
			algae.lastOxygenReleasedKg, algae.lastEvaporatedKg,
			algae.lastLightSuppliedKW, algae.lastLightRequestedKW,
			algae.lastCirculationSuppliedKW, algae.lastCirculationRequestedKW,
			algae.lastRespirationKg, algae.lastRespirationRequestedKg,
			algae.lastRespirationO2DrawnKg, algae.lastRespirationCo2ReturnedKg,
			algae.lastRespirationWaterKg, algae.lastRespirationHeatKW,
			algae.lastOxygenLimited ? " [O2-limited respiration]" : "",
			algae.lastHeatIntoCompartmentKW,
			algae.temperatureEligible ? "eligible" : "NOT eligible",
			algae.blockReason.empty() ? "" : (std::string(" - ") + algae.blockReason).c_str());
		ImGui::Text("  cumulative: growth %.4f kg | CO2 %.4f kg | water %.4f kg | N %.4f + P %.4f + K %.4f + minerals %.4f kg | O2 released %.4f kg | evaporated %.4f kg | respiration %.4f kg (O2 %.4f kg, CO2 %.4f kg, vapour %.4f kg) | light %.2f kJ + circulation %.2f kJ | chemical fixed %.2f kJ | respiration heat %.2f kJ | heat into compartment %.2f kJ | residual mass %.2e kg / energy %.2e kJ",
			algae.growthKg, algae.co2DrawnKg, algae.waterDrawnKg,
			algae.nitrogenDrawnKg, algae.phosphorusDrawnKg,
			algae.potassiumDrawnKg, algae.mineralsDrawnKg,
			algae.oxygenReleasedKg, algae.evaporatedKg,
			algae.respirationKg, algae.respirationO2DrawnKg,
			algae.respirationCo2ReturnedKg, algae.respirationWaterKg,
			algae.lightEnergyKJ, algae.circulationEnergyKJ,
			algae.chemicalEnergyFixedKJ, algae.respirationHeatKJ,
			algae.heatIntoCompartmentKJ,
			algae.residualKg, algae.residualKJ);
		ImGui::Text("  harvest: trigger %.4f kg (fraction %.3f, food %.3f + recovery %.3f) -> food '%s' / recovery '%s' / waste '%s' | last: requested %.6f kg, harvested %.6f kg (food %.6f, recovery %.6f, waste %.6f, food surplus routed %.6f, recovery surplus routed %.6f, constrained %.6f)%s | cumulative: harvested %.4f kg (food %.4f, recovery %.4f, waste %.4f) | dead residue: %.4f kg to waste, %.4f kg lost uncontained%s",
			algae.harvestTriggerKg, algae.harvestFraction, algae.foodFraction,
			algae.recoveryFraction,
			algae.foodStoreId.empty() ? "-" : algae.foodStoreId.c_str(),
			algae.recoveryStoreId.empty() ? "-" : algae.recoveryStoreId.c_str(),
			algae.wasteStoreId.empty() ? "-" : algae.wasteStoreId.c_str(),
			algae.lastHarvestRequestedKg, algae.lastHarvestedKg,
			algae.lastFoodAddedKg, algae.lastRecoveryAddedKg, algae.lastWasteAddedKg,
			algae.lastFoodSurplusToRecoveryKg, algae.lastRecoverySurplusToWasteKg,
			algae.lastHarvestConstrainedKg,
			algae.lastHarvestLimiter.empty() ? "" : (" | " + algae.lastHarvestLimiter).c_str(),
			algae.harvestedKg, algae.foodProducedKg, algae.recoveryProducedKg,
			algae.wasteProducedKg,
			algae.deadResidueToWasteKg, algae.deadResidueLostKg,
			algae.dead ? " | CULTURE DEAD: the living organic content was respired away; nothing recreates it for free" : "");
		if (algae.disabled)
			ImGui::Text("  zero culture volume: no service");
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
	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::CollapsingHeader("Resource endurance (observed; the estimate is the explicit static current-rate assumption: the net rate observed over the last declared observation interval is assumed to continue - no future profiles or consumption changes are forecast; near-zero, changing or uncertain rates are reported as no-change/unstable/bounded, never as an infinite countdown)"))
	{
		// #419: read-only endurance explanation per Resource store and
		// battery. The inventory split distinguishes the physical total,
		// the usable/qualified contents and the protected reserve; the
		// rate is observed on the existing cadence; the next authored
		// delivery is shown as context, not as a rescue. Rendering never
		// ticks or mutates anything.
		if (snapshot.endurance.empty())
			ImGui::TextDisabled("No Resource stores or batteries authored.");
		for (auto const& entry : snapshot.endurance)
		{
			auto const basisIndex = std::min(static_cast<size_t>(entry.basis),
				core::StationEnduranceBasisNames.size() - 1);
			auto const stabilityIndex = std::min(static_cast<size_t>(entry.stability),
				core::StationRateStabilityNames.size() - 1);
			ImGui::Text("%s '%s' (%s): physical %.3f = usable %.3f + held %.3f | protected reserve %.3f | drawable above reserve %.3f | [%s, rate %s]",
				entry.kind.c_str(), entry.id.c_str(), entry.unit.c_str(),
				entry.physicalTotal, entry.usableQualified,
				entry.physicalTotal - entry.usableQualified,
				entry.protectedReserve, entry.drawableUsable,
				core::StationEnduranceBasisNames[basisIndex],
				core::StationRateStabilityNames[stabilityIndex]);
			switch (entry.basis)
			{
			case core::StationEnduranceBasis::Unknown:
				ImGui::Text("  no completed observation window yet: no rate and no estimate - never an infinite countdown");
				break;
			case core::StationEnduranceBasis::Depleted:
				ImGui::Text("  no drawable inventory above the protected reserve: endurance 0 s");
				break;
			case core::StationEnduranceBasis::NoNetChange:
				ImGui::Text("  no net depletion observed over the %.1f s window ending t=%.1f s: no countdown",
					entry.observationWindowSeconds, entry.observedUntilSeconds);
				break;
			case core::StationEnduranceBasis::Growing:
				ImGui::Text("  drawable inventory growing at %.6f %s/s (window ending t=%.1f s): no depletion countdown",
					entry.observedNetRatePerSecond, entry.unit.c_str(), entry.observedUntilSeconds);
				break;
			case core::StationEnduranceBasis::Depleting:
				if (entry.stability == core::StationRateStability::Unstable
					&& entry.boundMaxSeconds <= 0.0)
					ImGui::Text("  UNSTABLE rate: bounded at least %.1f s at the fastest observed drain (%.6f %s/s, window ending t=%.1f s); no upper bound claimed - the previous window was not draining",
						entry.boundMinSeconds, entry.observedNetRatePerSecond,
						entry.unit.c_str(), entry.observedUntilSeconds);
				else if (entry.stability == core::StationRateStability::Unstable)
					ImGui::Text("  UNSTABLE rate: bounded %.1f s (fastest observed drain) .. %.1f s (slowest observed drain); the estimate shown is the conservative lower bound, window ending t=%.1f s",
						entry.boundMinSeconds, entry.boundMaxSeconds, entry.observedUntilSeconds);
				else if (entry.stability == core::StationRateStability::Unknown)
					ImGui::Text("  est %.1f s at the observed static rate (%.6f %s/s over the %.1f s window ending t=%.1f s); single observation: no stability claim yet",
						entry.estimateSeconds, entry.observedNetRatePerSecond,
						entry.unit.c_str(), entry.observationWindowSeconds, entry.observedUntilSeconds);
				else
					ImGui::Text("  est %.1f s at the observed static rate (%.6f %s/s over the %.1f s window ending t=%.1f s; stable across consecutive windows)",
						entry.estimateSeconds, entry.observedNetRatePerSecond,
						entry.unit.c_str(), entry.observationWindowSeconds, entry.observedUntilSeconds);
				break;
			}
			if (entry.kind == "battery" && entry.maxDrawPowerKW > 0.0)
				ImGui::Text("  power constraint: max discharge %.3f kW bounds how fast this energy can be drawn; the endurance uses the observed energy rate, never the power rating",
					entry.maxDrawPowerKW);
			if (entry.hasNextDelivery)
				ImGui::Text("  next delivery: %.3f %s at t=%.1f s (%s)",
					entry.nextDeliveryQuantity, entry.unit.c_str(),
					entry.nextDeliverySeconds,
					entry.nextDeliveryArrivesQualified ? "arrives usable" : "arrives held unqualified");
			else if (entry.kind == "store")
				ImGui::Text("  no authored delivery pending for this store");
		}
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::CollapsingHeader("Scenario forecast (bounded projection: the declared-profile scenario continues authored demand/production profiles and scheduled deliveries to a fixed horizon; projected shortfalls and inventory at resupply are scenario results under those assumptions, never guarantees - biology and process-chain recovery are flagged and never counted)"))
	{
		// #420: read-only scenario forecast per Resource store. The
		// projection is computed from the committed state at the
		// snapshot instant over the declared horizon and segment cap;
		// rendering never ticks, mutates or re-runs anything.
		auto const& forecast = snapshot.forecast;
		if (!forecast.available)
			ImGui::TextWrapped("No scenario forecast: %s", forecast.unavailableReason.empty()
				? (snapshot.enabled ? "Projection is unavailable." : "Station systems are disabled.")
				: forecast.unavailableReason.c_str());
		else
		{
			ImGui::TextWrapped("Scenario: %s", forecast.scenarioLabel.c_str());
			ImGui::TextWrapped("Uncertainty: %s", forecast.uncertaintyLabel.c_str());
			// #604: when the budget truncates, show exactly how far the
			// projection is complete instead of only warning that it is
			// truncated: everything strictly before the complete-through
			// time is fully modelled, the range beyond is unavailable.
			if (forecast.truncated)
				ImGui::Text("Projected from t=%.1f s: BUDGET-TRUNCATED, complete only through t=%.1f s (%u segments); the range beyond is unavailable and no store result should be read as covering the full horizon",
					forecast.asOfSeconds, forecast.completeThroughSeconds,
					forecast.segmentsComputed);
			else
				ImGui::Text("Projected from t=%.1f s over a %.0f s horizon (%u segments)",
					forecast.asOfSeconds, forecast.horizonSeconds,
					forecast.segmentsComputed);
			if (forecast.stores.empty())
				ImGui::TextDisabled("No Resource stores authored.");
			for (auto const& entry : forecast.stores)
			{
				if (!entry.unavailableReason.empty())
				{
					ImGui::TextWrapped("Store '%s': forecast unavailable: %s", entry.storeId.c_str(), entry.unavailableReason.c_str());
					continue;
				}
				auto const basisIndex = std::min(static_cast<size_t>(entry.basis),
					core::StationForecastBasisNames.size() - 1);
				ImGui::Text("store '%s' (%s): drawable %.3f of physical %.3f | projected net %+.6f %s/s | [%s]",
					entry.storeId.c_str(), entry.unit.c_str(),
					entry.drawableAtStart, entry.physicalAtStart,
					entry.netRateAtStart, entry.unit.c_str(),
					core::StationForecastBasisNames[basisIndex]);
				if (entry.hasShortfall)
				{
					ImGui::Text("  PROJECTED SHORTFALL: drawable inventory reaches zero at t=%.1f s",
						entry.shortfallOnsetSeconds);
					if (!entry.limitingServiceId.empty())
						ImGui::Text("    limiting service: %s '%s'",
							entry.limitingServiceKind.c_str(), entry.limitingServiceId.c_str());
					if (entry.unmetOutflowTotal > 0.0)
						ImGui::Text("    uncovered outflow while short: %.3f %s (demand that could not be drawn, reported not deleted)",
							entry.unmetOutflowTotal, entry.unit.c_str());
				}
				else if (entry.basis == core::StationForecastBasis::Incomplete)
					ImGui::Text("  no shortfall projected before t=%.1f s; beyond that the forecast is budget-truncated and unavailable (drawable at the complete-through bound: %.3f %s)",
						forecast.completeThroughSeconds,
						entry.projectedDrawableAtHorizon, entry.unit.c_str());
				else
					ImGui::Text("  no shortfall projected inside the horizon; drawable at horizon end: %.3f %s",
						entry.projectedDrawableAtHorizon, entry.unit.c_str());
				for (auto const& arrival : entry.arrivals)
					ImGui::Text("  resupply '%s' at t=%.1f s: %.3f %s scheduled, %.3f accepted / %.3f refused (capacity) | projected drawable at arrival: %.3f %s (%s)",
						arrival.deliveryId.c_str(), arrival.arrivalSeconds,
						arrival.scheduledQuantity, entry.unit.c_str(),
						arrival.acceptedQuantity, arrival.rejectedQuantity,
						arrival.projectedDrawableBefore, entry.unit.c_str(),
						arrival.rescuesStore
							? (arrival.arrivesQualified ? "lands before depletion, usable" : "lands before depletion, held unqualified")
							: "lands only after the projected shortfall");
				if (entry.biologyRecoveryPossible)
					ImGui::TextWrapped("  UNCERTAIN: crop/algae/microbial recovery could replenish this store; provisional biology is not counted as guaranteed recovery");
				if (entry.processProductionPossible)
					ImGui::Text("  NOT PROJECTED: process-chain production (treatment/electrolysis/conversion/capture) could add to this store but is outside the forecast scope");
				if (entry.processConsumptionPossible)
					ImGui::TextWrapped("  NOT PROJECTED: this store feeds consuming pathways the forecast does not project (process feeds/consumables, consumed maintenance spares, or generator fuel under automatic dispatch); the live store can run short earlier than shown");
				if (entry.biologyConsumptionPossible)
					ImGui::TextWrapped("  NOT PROJECTED: crop/algae/microbial uptake can drain this store; provisional biology consumption is outside the forecast scope");
			}
		}
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::CollapsingHeader("Resource/service trends (bounded)"))
	{
		// #418: read-only presentation of the bounded trend series. The
		// series carry units, simulation timestamps, requested versus
		// actual values and subsystem identity; truncation and the
		// effective stride are shown so the plot never implies a
		// complete history. Rendering never ticks or mutates anything.
		auto const trends = world->getStationSystemsTrends();
		if (trends.empty())
			ImGui::TextDisabled("No trend samples yet: trends are sampled at the observation cadence while the World runs.");
		ImGui::TextDisabled("Legend: grey = requested, green = actual (units per series)");
		for (auto const& series : trends)
		{
			ImGui::Text("%s %s (%s)", series.subsystem.c_str(),
				series.id.empty() ? "(station-wide)" : ("'" + series.id + "'").c_str(),
				series.unit.c_str());
			if (series.samples.empty())
			{
				ImGui::Text("  no samples yet");
				continue;
			}
			ImGui::Text("  %zu sample(s), effective stride %.2f s, window t=%.1f s .. t=%.1f s",
				series.samples.size(), series.sampleIntervalSeconds,
				series.samples.front().elapsedSeconds, series.samples.back().elapsedSeconds);
			if (series.downsampleLevel > 0)
				// #603: the level is reported through a finite double
				// factor, never an unchecked 32-bit shift that would be
				// undefined once the level reaches the operand width.
				ImGui::Text("  downsampled x%.0f (level %u): each retained sample covers %.2f s of simulation time; adjacent samples were merged pairwise to stay within the storage bound",
					std::ldexp(1.0, static_cast<int>(series.downsampleLevel)),
					series.downsampleLevel, series.sampleIntervalSeconds);
			if (series.truncated)
				ImGui::Text("  TRUNCATED: history before t=%.1f s was merged or dropped; this is not the complete run history",
					series.samples.front().elapsedSeconds);
			auto const width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
			auto const height = 48.0f;
			auto const origin = ImGui::GetCursorScreenPos();
			ImGui::Dummy({width, height});
			auto* draw = ImGui::GetWindowDrawList();
			draw->AddRect(origin, {origin.x + width, origin.y + height}, IM_COL32(90, 90, 90, 255));
			double minimum = series.samples.front().requested;
			double maximum = series.samples.front().requested;
			for (auto const& sample : series.samples)
			{
				minimum = std::min({minimum, sample.requested, sample.actual});
				maximum = std::max({maximum, sample.requested, sample.actual});
			}
			auto const span = maximum - minimum < 1e-12 ? 1.0 : maximum - minimum;
			auto const count = series.samples.size();
			// #603: the time axis is the samples' own simulation
			// timestamps, not their indices, so the plot stays honest
			// whatever spacing the retained history actually carries.
			auto const firstTime = series.samples.front().elapsedSeconds;
			auto const lastTime = series.samples.back().elapsedSeconds;
			auto const px = [width, firstTime, lastTime](double elapsed)
			{
				return lastTime <= firstTime ? width * 0.5f
					: width * static_cast<float>((elapsed - firstTime) / (lastTime - firstTime));
			};
			auto const py = [height, minimum, span](double value)
			{
				return height - 2.0f - static_cast<float>((value - minimum) / span) * (height - 4.0f);
			};
			for (size_t index = 1; index < count; ++index)
			{
				draw->AddLine({origin.x + px(series.samples[index - 1].elapsedSeconds), origin.y + py(series.samples[index - 1].requested)},
					{origin.x + px(series.samples[index].elapsedSeconds), origin.y + py(series.samples[index].requested)},
					IM_COL32(170, 170, 170, 255));
				draw->AddLine({origin.x + px(series.samples[index - 1].elapsedSeconds), origin.y + py(series.samples[index - 1].actual)},
					{origin.x + px(series.samples[index].elapsedSeconds), origin.y + py(series.samples[index].actual)},
					IM_COL32(80, 200, 120, 255));
			}
			if (count == 1)
			{
				draw->AddCircleFilled({origin.x + px(series.samples[0].elapsedSeconds), origin.y + py(series.samples[0].requested)},
					2.0f, IM_COL32(170, 170, 170, 255));
				draw->AddCircleFilled({origin.x + px(series.samples[0].elapsedSeconds), origin.y + py(series.samples[0].actual)},
					2.0f, IM_COL32(80, 200, 120, 255));
			}
		}
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::CollapsingHeader("Long-run stability audit (read-only: finite values, capacity bounds, conservation ledgers and bounded history over the public snapshot; a violation is a model regression reported with its magnitude, never a clamped or relaxed state)"))
	{
		// #424: the stability audit over the public snapshot, events,
		// trends and diagnostics. Rendering never ticks, mutates or
		// re-runs anything: it reads the same audit the acceptance tests
		// assert on, so the window and the regression seam can never
		// disagree about what "stable" means.
		auto const audit = core::auditStationStability(snapshot,
			world->getStationSystemsEvents(), world->getStationSystemsTrends(),
			world->getStationSystemsDiagnostics());
		if (!audit.enabled)
			ImGui::TextDisabled("No Station systems to audit: the Station is disabled.");
		else
		{
			if (audit.clean)
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
					"STABLE at t=%.1f s: %zu finite-value checks, %zu capacity-bound checks and %zu conservation ledgers all inside the declared tolerance (worst ledger at %.3fx tolerance); history bounded.",
					audit.elapsedSeconds, audit.finiteChecks, audit.boundChecks,
					audit.ledgerChecks, audit.maxResidualRatio);
			else
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
					"STABILITY VIOLATIONS at t=%.1f s: %zu non-finite, %zu bound breaches, %zu ledger residuals, %zu software failures.",
					audit.elapsedSeconds, audit.nonFiniteFindings.size(),
					audit.boundViolations.size(), audit.conservationViolations.size(),
					audit.softwareFailures);
			ImGui::Text("Finite-value checks: %zu | capacity-bound checks: %zu | conservation ledgers: %zu (worst %.3fx tolerance)",
				audit.finiteChecks, audit.boundChecks, audit.ledgerChecks,
				audit.maxResidualRatio);
			ImGui::Text("Event history: %zu retained of %zu bound | trend series: %zu holding %zu samples of %zu per-series bound%s",
				audit.eventsRetained, audit.eventHistoryLimit, audit.trendSeries,
				audit.trendSamplesRetained, audit.trendSampleLimit,
				audit.trendsTruncated ? " (truncated: older history merged or dropped)" : "");
			for (auto const& finding : audit.nonFiniteFindings)
				ImGui::TextWrapped("NON-FINITE: %s", finding.c_str());
			for (auto const& violation : audit.boundViolations)
				ImGui::TextWrapped("BOUND VIOLATION: %s", violation.c_str());
			for (auto const& violation : audit.conservationViolations)
				ImGui::TextWrapped("CONSERVATION: %s", violation.c_str());
			if (audit.softwareFailures > 0)
				ImGui::TextWrapped("SOFTWARE FAILURES: %zu reported by the runtime (see the diagnostics above).",
					audit.softwareFailures);
		}
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::CollapsingHeader("Incident history (bounded: newest 256 events; older events are discarded)"))
		for (auto const& event : world->getStationSystemsEvents())
			ImGui::TextWrapped("Tick %llu (t=%.1f s) [%s]: %s",
				static_cast<unsigned long long>(event.worldTick),
				static_cast<double>(event.worldTick) * static_cast<double>(core::World::getFixedTimestep()),
				event.category.empty() ? "station" : event.category.c_str(),
				event.message.c_str());
}
