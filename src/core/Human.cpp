#include "core/Human.h"
#include "core/AgentType.h"

namespace core
{
	Human::Human(std::string const& name) : Agent(name)
	{
		// Route to the bundled Human definition rather than a compiled baseline:
		// the same validated, frozen values as a script-backed Human observes.
		auto const definition = bundledHumanAgentType();
		setTypeIdentity(definition.typeId, definition.displayName,
			definition.resourceName, bundledHumanBaseline());
	}
}
