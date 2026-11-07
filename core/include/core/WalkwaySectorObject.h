#pragma once

#include <cstdint>
#include <memory>

#include "core/Platform.h"
#include "core/Walkway.h"
#include "core/SectorObject.h"


namespace core
{

	class PF_CORE_API WalkwaySectorObject : public SectorObject
	{
	public:

		WalkwaySectorObject(uint32_t cellX, uint32_t cellY, std::shared_ptr<const Sector> sector, uint32_t* vertexIdentifer = nullptr);

		~WalkwaySectorObject() = default;

		std::shared_ptr<const Walkway> getWalkway() const;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
