#include "support/RoutingPopulation.h"
#include "support/Restoration.h"
#include <iostream>

size_t getHeadlessWorkingSetBytes();
size_t getHeadlessPeakWorkingSetBytes();

void runRestorationBenchmark(std::filesystem::path const& input, unsigned cycles)
{
	restoration_support::verify(input, cycles, [](unsigned cycle, double reloadMs, double resetMs)
	{
		std::cout << "restoration-cycle=" << cycle << " reload-ms=" << reloadMs
			<< " reset-ms=" << resetMs << " working-set-MiB="
			<< getHeadlessWorkingSetBytes() / (1024.0 * 1024.0)
			<< " peak-working-set-MiB=" << getHeadlessPeakWorkingSetBytes() / (1024.0 * 1024.0) << '\n';
	});
}

void writeRoutingScaleWorld(std::filesystem::path const& output)
{
	(void)routing_support::populationRoutingRun(output, true, &std::cout, getHeadlessWorkingSetBytes);
}
