#pragma once

#include "Smoke.h"

namespace persistence
{
	void checkedInYamlWorldLoads(smoke::Context const& context);
	void stringYamlRoundTripsPrimitiveValues(smoke::Context const& context);
	void binarySerializerHonoursTheSerializerContract(smoke::Context const& context);
	void fileYamlRoundTrips(smoke::Context const& context);
	void transactionalWriterPreservesOpaqueBytes(smoke::Context const& context);
	void worldDocumentsUseTheirExactSuffixFormat(smoke::Context const& context);
	void malformedValuesAndInvalidUsageThrowUsefulErrors(smoke::Context const& context);
}
