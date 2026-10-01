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
	void reset()
	{
		resetAgentGroupsPanelState();
		resetTagsPanelState();
		resetBehavioursPanelState();
		gWorldDocumentHistory.clear();
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		(void)core::consumeLogMessages();
		clearObjectTileset();
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
