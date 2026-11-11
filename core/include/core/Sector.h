#pragma once

#include <string>
#include <vector>
#include <set>
#include <functional>
#include <optional>

#include "core/Platform.h"
#include "core/Area.h"
#include "core/SectorType.h"
#include "core/SectorEnd.h"
#include "core/SectorSurfaceVariant.h"
#include "core/SectorObject.h"
#include "core/SectorPosition.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/WindowSectorObject.h"
#include "core/Marker.h"


namespace core
{
	class SectorObjectVertex;

	typedef std::function<bool(std::shared_ptr<SectorObject>, std::shared_ptr<SectorObject>)> SectorObjectSortFunction;

	class PF_CORE_API Sector : public Area
	{
		friend class World;
		friend class Graph;
		// Committing a traversal transfers sector membership at the destination
		// endpoint with an explicitly authored, non-default position, which only
		// the private SectorPosition overload carries. The coordinator therefore
		// shares World's friendship here rather than widening Sector's public
		// surface (ADR 0004).
		friend class SimulationCoordinator;
		// The marker store creates and removes Marker sector objects on the
		// owning World's behalf and shares World's friendship here rather
		// than widening Sector's public surface (ADR 0022).
		friend class MarkerStore;
		// The world builder creates SectorObjects on the owning World's
		// behalf and shares World's friendship here rather than widening
		// Sector's public surface (#688, ADR 0022).
		friend class WorldBuilder;

	private:

		SectorType mType;

		uint32_t mLayerIndex;

		uint32_t mIndex;

		uint32_t mCellsWide, mLevelsHigh;

		float mTopLevelHeight;

		std::string mName;

		uint32_t mCapacity;

		bool mLightsOn;

		// The cells of this surface that wear a variant of the type's tile
		// rather than its base, in the Sector-local frame `SectorSurfaceVariant`
		// describes (#1040). Artwork only: nothing here is read by traversal,
		// capacity or any simulation state, and a Sector that never had a
		// variant recorded holds an empty list and draws what it always drew.
		std::vector<SectorSurfaceVariant> mSurfaceVariants;

		std::vector<std::shared_ptr<SectorObject>> mObjects;

		std::set<Agent*> mAgents;

	protected:

		std::vector<SectorEnd> mEnds;

	private:

		[[nodiscard]] std::shared_ptr<SectorObject> _getObject(uint32_t index);

		SectorPosition findFreeAgentPosition(Agent const* agent) const;

		// Following functions to be called by World
		uint32_t createDoor(std::shared_ptr<const Sector> sector, std::shared_ptr<const Sector> backSector,
			uint32_t x, uint32_t y, uint32_t cellsWide,
			Door::Height height = Door::Height::Regular, uint32_t* vertexIdentifier = nullptr);

		void addDoor(std::shared_ptr<DoorSectorObject> door);

		uint32_t createWindow(std::shared_ptr<const Sector> sector, std::shared_ptr<const Sector> backSector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, uint32_t* vertexIdentifier = nullptr, bool boothWindow = false);

		uint32_t addWindow(std::shared_ptr<WindowSectorObject> window);

		uint32_t createPhysicalControl(std::shared_ptr<const Sector> sector, std::string const& name, uint32_t x, uint32_t y, float xOffset, float yOffset, uint32_t flags, uint32_t* vertexIdentifier = nullptr);

		uint32_t createWalkway(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t* vertexIdentifier = nullptr);

		uint32_t createMarker(std::shared_ptr<const Sector> sector, MarkerId id,
			std::string name, MarkerProperties properties, uint32_t x, uint32_t y,
			float xOffset, uint32_t* vertexIdentifier = nullptr);

		bool removeSectorObject(uint32_t index);
		void trimTrailingObjectTombstones();

		uint32_t createForceBridge(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t size, int fromSide, bool extensible, bool startExtended);

		uint32_t createLadder(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, bool extensible, bool startExtended, uint32_t levelsHigh, uint32_t* vertexIdentifier = nullptr);

		uint32_t createPlatformLift(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, std::vector<uint32_t> const& stopOffsets, uint32_t* vertexIdentifier = nullptr);

		uint32_t createBulkheadDoor(std::shared_ptr<const Sector> sector, std::shared_ptr<const Sector> rightLocation, uint32_t levelIndex, int side);

		void addBulkheadDoor(std::shared_ptr<BulkheadDoorSectorObject> door, uint32_t levelIndex, int side);

		// Replaces this Sector's surface variant entries from its own
		// construction record. World-only: the entries are authored on the
		// record, and the live list is a projection of it that a rebuild
		// re-derives rather than something to be edited in place (#1040).
		void setSurfaceVariants(std::vector<SectorSurfaceVariant> entries);

		void setEndType(uint32_t levelIndex, int side, SectorEndType type);

		void addEndWall(uint32_t levelIndex, int side);

		void removeEndWall(uint32_t levelIndex, int side);

		void enterAgent(Agent* agent, SectorPosition const& pos, bool authored = true);

		// Advance device and object state without moving agents. World uses
		// this to keep resource work in the first deterministic tick phase.
		void advanceResources(float frameTime);

		// This is designed to be subclassed if required.
		virtual void updateImpl(float /* frameTime */) {}

	protected:

		uint32_t addSectorObject(std::shared_ptr<SectorObject> object);

	public:

		Sector(SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height, std::string const& name, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, uint32_t capacity);

		virtual ~Sector() = default;

		[[nodiscard]] SectorType getType() const;

		[[nodiscard]] uint32_t getLayerIndex() const;

		[[nodiscard]] uint32_t getIndex() const;

		[[nodiscard]] uint32_t getCellX0() const;

		[[nodiscard]] uint32_t getCellX1() const;

		[[nodiscard]] uint32_t getCellY0() const;

		[[nodiscard]] uint32_t getCellY1() const;

		[[nodiscard]] uint32_t getCellsWide() const;

		[[nodiscard]] uint32_t getLevelsHigh() const;

		[[nodiscard]] float getLevelHeight(uint32_t levelIndex) const;

		[[nodiscard]] float getTopLevelHeight() const;

		// The top-level height actually used for geometry, rendering, and Agent
		// clearance. A one-cell-high Room may override the standard height with a
		// scale; every other Sector returns the authored top-level height unchanged.
		[[nodiscard]] virtual float getEffectiveTopLevelHeight() const { return mTopLevelHeight; }

		// Whether this Sector is a Room in the height-authoring sense: an
		// occupiable Location that is neither a Corridor nor a Facade. Only Rooms
		// may carry a height override.
		[[nodiscard]] virtual bool isRoom() const { return false; }

		[[nodiscard]] std::string const& getName() const;

		[[nodiscard]] uint32_t getCapacity() const;

		// The variant entries this Sector wears, in the order they were
		// recorded. Sector-local: `column` from the Sector's left edge,
		// `level` from its floor line, the same frame the per-cell surface
		// loop resolves in (#1040).
		[[nodiscard]] std::vector<SectorSurfaceVariant> const& getSurfaceVariants() const;

		// The variant one Sector-local cell was recorded as wearing, or
		// nothing where the cell wears the type's base tile. A cell with no
		// entry is the ordinary case, so this answers with an optional rather
		// than a sentinel a caller has to remember the meaning of.
		[[nodiscard]] std::optional<uint32_t> surfaceVariantAt(uint32_t column,
			uint32_t level) const;

		[[nodiscard]] SectorEndType getEndType(uint32_t levelIndex, int side) const;

		[[nodiscard]] virtual std::string getDescription() const = 0;

		[[nodiscard]] uint32_t getNumObjects() const;

		[[nodiscard]] std::shared_ptr<SectorObject> getObject(uint32_t index) const;

		[[nodiscard]] std::vector<std::shared_ptr<SectorObject>> getSortedObjects(SectorObjectSortFunction sortFunc) const;

		[[nodiscard]] std::shared_ptr<const Object> getObjectAtPosition(float x, float y,
			std::shared_ptr<const SectorObject>* sectorObject = nullptr) const;

		[[nodiscard]] virtual bool sectorSupportsObjectType(SectorObjectType type) const = 0;

		// Whether this sector may act as the back side of a look-through (a Window on the
		// Layer in front may look into it). Distinct from hosting: a Background may host
		// nothing yet still be looked into. Defaults to the hosting rule.
		[[nodiscard]] virtual bool sectorSupportsObjectAsLookTarget(SectorObjectType type) const;

		[[nodiscard]] bool areLightsOn() const;

		[[nodiscard]] std::set<Agent*> const& getAgents() const;

		bool lightsOn();

		bool lightsOff();

		bool toggleLights();

		void enterAgent(Agent* agent, uint32_t levelIndex, float xOffset);

		void enterAgent(Agent* agent, std::shared_ptr<const Vertex> vertex = {}, Vector2 const& offset = { 0, 0 });

		std::shared_ptr<Edge> exitAgent(Agent* agent);

		void update(float frameTime);
	};

} // core
