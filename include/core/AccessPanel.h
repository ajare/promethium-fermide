#pragma once

#include "core/Object.h"
#include "core/SectorObject.h"

namespace core
{
	struct AccessPanelGeometry
	{
		float width{0.5f}, height{0.25f}, yOffset{0.25f};
		bool operator==(AccessPanelGeometry const&) const = default;
	};

	// Authored geometry and cell ownership are independent of runtime state.
	class AccessPanel : public Object
	{
		friend class World;
		AccessPanelGeometry mGeometry;
		uint32_t mCellX, mLevelOffset;
		void configure(uint32_t cellY, AccessPanelGeometry geometry);
	public:
		enum class Type { Empty };
		enum class State { Closed };
		AccessPanel(uint32_t cellX, uint32_t cellY, uint32_t levelOffset, AccessPanelGeometry geometry);
		static bool geometryIsValid(AccessPanelGeometry geometry);
		// Editor indicator only; collision and persistence always use authored bounds.
		void getSelectionShape(Vector2& min, Vector2& max) const;
		AccessPanelGeometry getGeometry() const { return mGeometry; }
		uint32_t getCellX() const { return mCellX; }
		uint32_t getLevelOffset() const { return mLevelOffset; }
		Type getType() const { return Type::Empty; }
		State getState() const { return State::Closed; }
		std::string getDescription() const override { return "Closed Empty Access panel"; }
	};

	class AccessPanelSectorObject : public SectorObject
	{
	public:
		AccessPanelSectorObject(std::shared_ptr<const Sector> sector, uint32_t cellX,
			uint32_t cellY, uint32_t levelOffset, AccessPanelGeometry geometry);
		std::shared_ptr<const AccessPanel> getPanel() const;
		bool pointInside(float x, float y) const override;
		std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object,
			std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};
}
