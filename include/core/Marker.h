#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/EntityId.h"
#include "core/Object.h"
#include "core/Vector2.h"


namespace core
{

	enum class MarkerProperty : uint32_t
	{
		BlocksPathing = 1u << 0
	};

	using MarkerProperties = uint32_t;

	constexpr MarkerProperties markerPropertyBit(MarkerProperty property)
	{
		return static_cast<MarkerProperties>(property);
	}

	class Marker : public Object
	{
		friend class World;
		friend class MarkerSectorObject;

		MarkerId mId;
		std::string mName;
		MarkerProperties mProperties;
		uint32_t mCellX, mCellY;
		float mOffset;

		void setName(std::string name) { mName = std::move(name); }
		void setProperties(MarkerProperties properties) { mProperties = properties; }

		Marker(MarkerId id, std::string name, MarkerProperties properties,
			uint32_t cellX, uint32_t cellY, float xOffset);

	public:
		static constexpr size_t MaxNameBytes{ 63 };

		MarkerId getId() const { return mId; }
		MarkerId getMarkerId() const { return mId; }
		std::string const& getName() const { return mName; }
		MarkerProperties getProperties() const { return mProperties; }
		bool hasProperty(MarkerProperty property) const
		{
			return (mProperties & markerPropertyBit(property)) != 0;
		}

		static std::string trimName(std::string const& value);
		static bool nameIsValid(std::string const& trimmed, std::string* diagnostic);

		uint32_t getCellX() const;

		uint32_t getCellY() const;

		float getOffset() const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;
	};

} // core
