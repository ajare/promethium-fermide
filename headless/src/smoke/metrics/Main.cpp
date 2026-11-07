#include "Checks.h"

int main(int argc, char** argv)
{
	constexpr smoke::Check checks[] = {
		{ "metrics", runMetricsChecks },
	};
	return smoke::main("metrics", checks, argc, argv);
}
