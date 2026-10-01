#include "Formats.h"
#include "Smoke.h"

namespace
{
	constexpr smoke::Check checks[] = {
		{ "yaml-primitives", persistence::stringYamlRoundTripsPrimitiveValues },
		{ "binary-contract", persistence::binarySerializerHonoursTheSerializerContract },
		{ "yaml-file", persistence::fileYamlRoundTrips },
		{ "transactional-bytes", persistence::transactionalWriterPreservesOpaqueBytes },
		{ "world-document-formats", persistence::worldDocumentsUseTheirExactSuffixFormat },
		{ "yaml-errors", persistence::malformedValuesAndInvalidUsageThrowUsefulErrors },
		{ "checked-in-world", persistence::checkedInYamlWorldLoads },
	};
}

int main(int argc, char** argv)
{
	return smoke::main("persistence", checks, argc, argv);
}
