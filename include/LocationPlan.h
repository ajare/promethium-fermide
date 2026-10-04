#pragma once

#include <functional>
#include <memory>
#include "core/World.h"
#include "WorldDrawList.h"

// Transient, read-only editor view. Never owns Furniture or catalogue data.
class LocationPlan
{
public:
	using Presenter = std::function<void(WorldDrawList const&, ImVec2, ImVec2)>;
	bool open(std::shared_ptr<core::World> const& world,
		std::shared_ptr<const core::Sector> const& location, uint32_t worldLevel);
	void close();
	void renderSelectionAction(std::shared_ptr<core::World> const& world,
		std::shared_ptr<const core::Sector> const& selection, uint32_t worldLevel);
	void render(std::shared_ptr<core::World> const& world, Presenter const& present);

private:
	std::shared_ptr<const core::Sector> target(std::shared_ptr<core::World> const& world);
	std::weak_ptr<core::World> mWorld;
	std::shared_ptr<const core::Sector> mLocation;
	uint32_t mDepthRows{4};
	uint32_t mWorldLevel{};
	bool mOpen{};
	bool mFocus{};
};

// Screen-space grid geometry; Local depth is ordering, never World height.
// The viewport is also intersected with the caller's clip by the recorder.
void renderLocationPlanGrid(WorldDrawList& commands, core::Sector const& location,
	ImVec2 viewportPosition, ImVec2 viewportSize, uint32_t depthRows = 4,
	core::World const* world = nullptr, uint32_t worldLevel = 0);

// Derived afresh from the current catalogue; no cached instance/vertex pointers.
uint32_t locationPlanDepthRows(core::World const& world, core::Sector const& location,
	uint32_t worldLevel);
