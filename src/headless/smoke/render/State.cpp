#include "Checks.h"

#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "core/Agent.h"

extern UISettings gUISettings;
extern core::Agent* gSelectedAgent;

struct render_smoke::State::Saved
{
	UISettings settings = gUISettings;
	core::Agent* selection = gSelectedAgent;
};

render_smoke::State::State() : saved_(std::make_unique<Saved>())
{
	gUISettings = UISettings{};
	gSelectedAgent = nullptr;
	clearObjectTileset();
	clearSectorTileset();
}

render_smoke::State::~State()
{
	clearObjectTileset();
	clearSectorTileset();
	gUISettings = saved_->settings;
	gSelectedAgent = saved_->selection;
}
