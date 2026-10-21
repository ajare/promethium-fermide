#include "StationReferenceFixtures.h"
#pragma once
#include "core/StationReference.h"

namespace smoke::station
{
	// Meridian plus a compound conversion and unequal battery banks, within
	// the ordinary 32-store configuration limit. Five-way ports are exercised
	// independently by exchangeObservations.
	inline core::StationSystemsSettings mixedObservationFixture()
	{
		auto s = core::bundledStationReference().settings;
		s.observationIntervalSeconds = 0.1f;
		s.trendRetentionSeconds = 3600;
		s.batteries.push_back({"audit-small", 10, 8, 2, 1, 1});
		auto product = s.stores.front(); // Food composition, not inferred chemistry.
		product.id = "audit-product"; product.capacity = 100;
		product.startingQualified = 0; product.startingUnqualified = 0;
		product.protectedReserve = 0; product.materialId = "audit-product";
		s.stores.push_back(product);
		core::ResourceConversionSettings c; c.id = "audit-split";
		c.inputs = {{"food", 0.02}}; c.outputs = {{product.id, 0.01}};
		s.conversions.push_back(c);
		return s;
	}
}
