#include "support/LiftBoarding.h"
#include <iostream>

void runLiftCrossingRepro(char const* filename)
{
	headless::checkLiftCrossings(filename, false);
	headless::checkLiftCrossings(filename, true);
	std::cout << "PASS: Lift boarding stays within the landing doorway in full and two-Agent runs\n";
}

// Replay authored paths, including opposing landing queues. A capacity-reserved
// boarder must reach its boarding position and retain the admitted run direction.
void runLiftBoardingRepro(char const* filename)
{
	headless::checkLiftBoarding(filename, false);
	headless::checkLiftBoarding(filename, true);
	std::cout << "PASS: Lift demand drained in full and reduced boarding regressions\n";
}
