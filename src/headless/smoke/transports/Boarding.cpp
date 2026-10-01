#include "Checks.h"
#include "LiftBoarding.h"

void registerBoarding(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "liftBoardingFull", [](smoke::Context const& context) {
		headless::checkLiftBoarding(context.fixture("resources/test-worlds/lift-test-1.world.yaml").string().c_str(), false);
	} });
	checks.push_back({ "liftBoardingReduced", [](smoke::Context const& context) {
		headless::checkLiftBoarding(context.fixture("resources/test-worlds/lift-test-1.world.yaml").string().c_str(), true);
	} });
	checks.push_back({ "liftCrossingsFull", [](smoke::Context const& context) {
		headless::checkLiftCrossings(context.fixture("resources/test-worlds/lift-test-1.world.yaml").string().c_str(), false);
	} });
	checks.push_back({ "liftCrossingsReduced", [](smoke::Context const& context) {
		headless::checkLiftCrossings(context.fixture("resources/test-worlds/lift-test-1.world.yaml").string().c_str(), true);
	} });
}
