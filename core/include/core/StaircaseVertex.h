#pragma once

#include "core/Platform.h"
#include "core/Staircase.h"
#include "core/Vertex.h"

namespace core
{
	class PF_CORE_API StaircaseVertex : public Vertex
	{
		std::shared_ptr<Staircase> mStaircase;
	public:
		StaircaseVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Staircase> staircase,
			float xOffset, float yOffset);
		StaircaseVertex(uint32_t id, std::shared_ptr<Sector> sector,
			std::shared_ptr<Staircase> staircase, float xOffset, float yOffset);
		[[nodiscard]] std::string getDescription() const override;
		[[nodiscard]] std::shared_ptr<Vertex> copyWithoutEdges() override;
	};
}
