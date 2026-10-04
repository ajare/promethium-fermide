#pragma once

#include <functional>
#include <memory>
#include "core/World.h"
#include "WorldDrawList.h"
#include "DocumentEdit.h"

// Transient editor view. Never owns Furniture or catalogue data.
class LocationPlan
{
public:
	using Presenter = std::function<void(WorldDrawList const&, ImVec2, ImVec2)>;
	bool open(std::shared_ptr<core::World> const& world,
		std::shared_ptr<const core::Sector> const& location, uint32_t worldLevel);
	void close();
	void renderSelectionAction(std::shared_ptr<core::World> const& world,
		std::shared_ptr<const core::Sector> const& selection, uint32_t worldLevel);
	void render(std::shared_ptr<core::World> const& world, Presenter const& present,
		DocumentHistory& history = gWorldDocumentHistory);
	bool isOpen(std::shared_ptr<core::World> const& world);
	// Called by the existing canvas palette. Returns mouse consumption.
	bool renderPaletteRow(std::shared_ptr<core::World> const& world,
		WorldDrawList& commands, ImVec2 trayTopLeft, bool hovered);

private:
	std::shared_ptr<const core::Sector> target(std::shared_ptr<core::World> const& world);
	std::weak_ptr<core::World> mWorld;
	std::shared_ptr<const core::Sector> mLocation;
	uint32_t mDepthRows{4};
	uint32_t mWorldLevel{};
	bool mOpen{};
	bool mFocus{};
	std::weak_ptr<const core::FurnitureCatalogue> mRowCatalogue, mDragCatalogue;
	std::string mDragKey, mDeleteDiagnostic;
	size_t mPage{};
	bool mDragArmed{}, mDragging{};
	uint64_t mMoveId{};
	core::FurnitureInstance mMoveOriginal;
	float mMoveOffsetX{};
	int mMoveOffsetDepth{};
	void cancelDrag();
};

// Screen-space grid geometry; Local depth is ordering, never World height.
// The viewport is also intersected with the caller's clip by the recorder.
void renderLocationPlanGrid(WorldDrawList& commands, core::Sector const& location,
	ImVec2 viewportPosition, ImVec2 viewportSize, uint32_t depthRows = 4,
	core::World const* world = nullptr, uint32_t worldLevel = 0, uint64_t selectedId = 0);

void renderLocationPlanPreview(WorldDrawList& commands, core::Sector const& location,
	core::FurnitureDefinition const& definition, float x, int depth, bool valid,
	ImVec2 viewportPosition, ImVec2 viewportSize, uint32_t depthRows);

// Derived afresh from the current catalogue; no cached instance/vertex pointers.
uint32_t locationPlanDepthRows(core::World const& world, core::Sector const& location,
	uint32_t worldLevel);
