#pragma once

#include "core/Object.h"
#include "core/EntityId.h"
#include <vector>
#include <optional>
#include "core/Defines.h"
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
		friend class SimulationCoordinator;
		AccessPanelId mId;
		InteractionPointId mOpenControl, mCloseControl;
		bool mOpen{false}; // Target, independent of physical progress.
		float mProgress{0};
		std::optional<float> mSpeedOverride;
		AccessPanelGeometry mGeometry;
		uint32_t mCellX, mLevelOffset;
		void configure(uint32_t cellY, AccessPanelGeometry geometry);
	public:
		enum class Type { Empty };
		enum class State { Closed, Opening, Open, Closing };
		static constexpr float DefaultSpeed = 0.5f;
		static bool speedIsValid(std::optional<float> speed);
		float getSpeed() const { return mSpeedOverride.value_or(DefaultSpeed); }
		std::optional<float> getSpeedOverride() const { return mSpeedOverride; }
		float getProgress() const { return mProgress; }
		char const* getStateName() const;
		enum class Action { Open, Close };
		AccessPanel(uint32_t cellX, uint32_t cellY, uint32_t levelOffset, AccessPanelGeometry geometry);
		static bool geometryIsValid(AccessPanelGeometry geometry);
		// Editor indicator only; collision and persistence always use authored bounds.
		void getSelectionShape(Vector2& min, Vector2& max) const;
		AccessPanelGeometry getGeometry() const { return mGeometry; }
		uint32_t getCellX() const { return mCellX; }
		uint32_t getLevelOffset() const { return mLevelOffset; }
		Type getType() const { return Type::Empty; }
		State getState() const { return mOpen ? (mProgress == 1 ? State::Open : State::Opening)
			: (mProgress == 0 ? State::Closed : State::Closing); }
		AccessPanelId getId() const { return mId; }
		InteractionPointId getControl(Action action) const { return action == Action::Open ? mOpenControl : mCloseControl; }
		// Common opening is independent of the type's exposed functionality.
		std::vector<Action> getActions() const;
		std::vector<Action> getExposedActions() const;
		std::string getDescription() const override { return std::string(getStateName()) + " Empty Access panel"; }
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
