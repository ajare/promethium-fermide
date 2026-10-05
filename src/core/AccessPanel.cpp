#include "core/AccessPanel.h"
#include "core/Sector.h"
#include "core/SectorObjectVertex.h"
#include <cmath>

namespace core
{
	std::vector<AccessPanel::Action> AccessPanel::getExposedActions() const
	{
		switch (getType())
		{
		case Type::Empty: return {Action::Close};
		}
		return {};
	}

	std::vector<AccessPanel::Action> AccessPanel::getActions() const
	{
		return getState() == State::Open ? getExposedActions()
			: mOpen ? std::vector<Action>{Action::Close} : std::vector<Action>{Action::Open};
	}

	bool AccessPanel::speedIsValid(std::optional<float> speed)
	{
		return !speed || (std::isfinite(*speed) && *speed > 0);
	}

	char const* AccessPanel::getStateName() const
	{
		switch (getState())
		{
		case State::Closed: return "Closed";
		case State::Opening: return "Opening";
		case State::Open: return "Open";
		case State::Closing: return "Closing";
		}
		return "Closed";
	}

	bool AccessPanel::geometryIsValid(AccessPanelGeometry g)
	{
		return std::isfinite(g.width) && std::isfinite(g.height) && std::isfinite(g.yOffset)
			&& g.width >= 0 && g.width <= 1 && g.height >= 0 && g.height <= 1
			&& g.yOffset >= 0 && g.yOffset <= 1 && double(g.yOffset) + double(g.height) <= 1;
	}

	AccessPanel::AccessPanel(uint32_t x, uint32_t y, uint32_t level, AccessPanelGeometry g)
		: Object(float(x) + (1 - g.width) * 0.5f, float(y) + g.yOffset, g.width, g.height),
		mGeometry(g), mCellX(x), mLevelOffset(level) {}

	void AccessPanel::getSelectionShape(Vector2& min, Vector2& max) const
	{
		getFullShape(min, max);
		if (mGeometry.width == 0 || mGeometry.height == 0)
		{
			auto centre = (min + max) * 0.5f;
			min = centre - Vector2{0.04f, 0.04f}; max = centre + Vector2{0.04f, 0.04f};
		}
	}

	bool AccessPanelSectorObject::pointInside(float x, float y) const
	{
		Vector2 min, max; getPanel()->getSelectionShape(min, max);
		return x >= min.x && x <= max.x && y >= min.y && y <= max.y;
	}

	void AccessPanel::configure(uint32_t y, AccessPanelGeometry g)
	{
		mGeometry = g;
		setPosition({float(mCellX) + (1 - g.width) * 0.5f, float(y) + g.yOffset});
		setSize({g.width, g.height});
	}

	AccessPanelSectorObject::AccessPanelSectorObject(std::shared_ptr<const Sector> sector,
		uint32_t x, uint32_t y, uint32_t level, AccessPanelGeometry g)
		: SectorObject(SectorObjectType::AccessPanel, sector, x, y, 1, 1,
			std::make_shared<AccessPanel>(x, y, level, g), nullptr) {}

	std::shared_ptr<const AccessPanel> AccessPanelSectorObject::getPanel() const
	{
		return std::static_pointer_cast<AccessPanel>(_getObject());
	}

	std::shared_ptr<Vertex> AccessPanelSectorObject::createVertex(std::shared_ptr<SectorObject> object,
		std::shared_ptr<Sector> sector, void*) const
	{
		auto vertex = std::make_shared<SectorObjectVertex>(VertexSubType::Interactable, sector, object,
			float(getCellX() - sector->getCellX()) + 0.5f, float(getCellY() - sector->getCellY()));
		vertex->setObject(object->_getObject());
		return vertex;
	}
}
