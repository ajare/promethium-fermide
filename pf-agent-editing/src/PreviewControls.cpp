#include "PreviewControls.h"

#include "game/WorldRuntime.h"

namespace editor
{
	namespace
	{
		// The runtime a control request may act on, or null. A null handle,
		// a handle to a document the editor has since replaced, and a
		// document whose runtime cannot be reached all answer the same way:
		// there is nothing here to control. Resolving through one place is
		// what keeps the pause and the reset from drifting into refusing
		// differently.
		game::WorldRuntime* controllableRuntime(
			std::shared_ptr<EditorDocument> const& document) noexcept
		{
			if (!document || document != currentDocument()) return nullptr;
			try
			{
				return &document->runtime();
			}
			catch (...)
			{
				return nullptr;
			}
		}
	}

	bool previewIsControllable(
		std::shared_ptr<EditorDocument> const& document) noexcept
	{
		return controllableRuntime(document) != nullptr;
	}

	SimulationControl setPreviewPaused(
		std::shared_ptr<EditorDocument> const& document, bool paused)
	{
		SimulationControl result;
		auto* runtime = controllableRuntime(document);
		if (!runtime) return result;
		result.handled = true;
		if (paused)
		{
			runtime->pauseSimulation();
			result.paused = true;
			return result;
		}
		if (runtime->isSimulationPaused() && !runtime->resumeSimulation())
		{
			// The resume was refused, so the preview stays paused and the
			// caller needs the reason to show. Which diagnostic applies is
			// the World's own judgement, not this function's.
			result.paused = true;
			auto const& world = runtime->world();
			result.diagnostic = world.agentBehaviourConfigurationsAreValid()
				? world.getTopologyDiagnostic()
				: world.getAgentBehaviourDependencyDiagnostic();
			return result;
		}
		result.paused = false;
		return result;
	}

	SimulationControl resetPreview(
		std::shared_ptr<EditorDocument> const& document)
	{
		SimulationControl result;
		auto* runtime = controllableRuntime(document);
		if (!runtime) return result;
		result.handled = true;
		runtime->resetSimulation();
		result.paused = runtime->isSimulationPaused();
		return result;
	}
}
