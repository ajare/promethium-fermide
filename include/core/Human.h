#pragma once

#include "core/Agent.h"

namespace core
{
	// Built-in physical identity. Shared properties and simulation participation
	// remain in Agent; there is deliberately no conversion operation.
	class Human final : public Agent
	{
	public:
		explicit Human(std::string const& name) : Agent(name) {}
		char const* getTypeName() const override { return "Human"; }
		AgentPhysicalBaseline const& getPhysicalBaseline() const override
		{
			// Match the historical expressions exactly (including float rounding).
			static constexpr AgentPhysicalBaseline baseline{
				0.4f, (0.7f - 0.2f) - 0.05f, 0.25f,
				0.5f, 0.25f, 0.35f, 0.45f, 0.6f, 0.6f, 0.3f, 0.5f };
			return baseline;
		}
	};
}
