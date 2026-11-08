#include "State.h"

#include "AgentGroupsPanel.h"
#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "TagsPanel.h"
#include "UISettings.h"
#include "core/Log.h"
#include "core/TransactionalFileWriter.h"

struct ImFont;
namespace core { class Agent; class Vertex; class Sector; class SectorObject; }
extern UISettings gUISettings;
extern core::Agent* gHoveredAgent;
extern core::Agent* gSelectedAgent;
extern std::shared_ptr<const core::Vertex> gSelectedVertex;
extern std::shared_ptr<const core::Sector> gSelectedSector;
extern std::shared_ptr<const core::SectorObject> gHoveredSectorObject;
extern std::shared_ptr<const core::SectorObject> gSelectedSectorObject;
extern ImFont* gAgentIconFont;

namespace
{
	// The history the headless editor checks commit into.
	//
	// Production has no process-wide history at all: the shipped editor's
	// history is the one its open document's authoring handle owns, and the
	// global these checks used to share with it is gone (#975). These checks
	// drive the same panels the GUI drives, but against a bare
	// `core::World` with no `editor::EditorDocument` open, so they need a
	// history of their own - and it has to be reachable through the same
	// seam the panels use, otherwise the checks would not be driving the
	// seam they claim to cover.
	DocumentHistory gHarnessHistory;

	// Installed once per check binary, before any check runs. This is the
	// only place the resolver is ever replaced, and it is in the test tier:
	// a shipped build answers through the open document and nothing can
	// point it at a stack nobody owns.
	struct HarnessHistoryResolver
	{
		HarnessHistoryResolver()
		{
			setDocumentHistoryResolver(
				[]() -> DocumentHistory& { return gHarnessHistory; });
		}
	} gHarnessHistoryResolver;

	void reset()
	{
		resetAgentGroupsPanelState();
		resetTagsPanelState();
		resetBehavioursPanelState();
		// Cleared through the seam rather than by naming `gHarnessHistory`,
		// so the isolation this fixture provides covers whatever the seam is
		// currently answering with, not just the object it happens to answer
		// with today.
		currentDocumentHistory().clear();
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		(void)core::consumeLogMessages();
		clearObjectTileset();
		clearAgentTileset();
		clearFurnitureTileset();
		clearSectorTileset();
	}
}

struct editor_smoke::State::Saved
{
	UISettings settings = gUISettings;
	core::Agent* hoveredAgent = gHoveredAgent;
	core::Agent* selectedAgent = gSelectedAgent;
	decltype(gSelectedVertex) vertex = gSelectedVertex;
	decltype(gSelectedSector) sector = gSelectedSector;
	decltype(gHoveredSectorObject) hoveredObject = gHoveredSectorObject;
	decltype(gSelectedSectorObject) selectedObject = gSelectedSectorObject;
	ImFont* iconFont = gAgentIconFont;
};

editor_smoke::State::State() : saved_(std::make_unique<Saved>())
{
	reset();
	gUISettings = UISettings{};
	gHoveredAgent = nullptr;
	gSelectedAgent = nullptr;
	gSelectedVertex.reset();
	gSelectedSector.reset();
	gHoveredSectorObject.reset();
	gSelectedSectorObject.reset();
	gAgentIconFont = nullptr;
}

editor_smoke::State::~State()
{
	reset();
	gUISettings = saved_->settings;
	gHoveredAgent = saved_->hoveredAgent;
	gSelectedAgent = saved_->selectedAgent;
	gSelectedVertex = saved_->vertex;
	gSelectedSector = saved_->sector;
	gHoveredSectorObject = saved_->hoveredObject;
	gSelectedSectorObject = saved_->selectedObject;
	gAgentIconFont = saved_->iconFont;
}
