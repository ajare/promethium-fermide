#include "WorldChecks.h"
#include "Restoration.h"

namespace persistence
{
	void restorationPreservesStatePathsAndLifetimes(smoke::Context const& context)
	{
		restoration_support::verify(
			context.fixture("resources/test-worlds/door-test-1.world.yaml"), 5);
	}
}
