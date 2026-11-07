#pragma once

#include <string>
#include <utility>

namespace core
{
	// The persisted Resource-name / expected-UUID pair of one external registry
	// Resource (ADR 0007 / ADR 0008). A World persists only this reference;
	// the loaded registry is deliberately not a child for dirty-state purposes.
	// The shared RegistryBinding state shape (#676); World aliases it under the
	// per-family names serialization and replay already use.
	template <typename Registry>
	struct RegistryReference
	{
		std::string resourceName;
		std::string expectedUuid;
	};
}
