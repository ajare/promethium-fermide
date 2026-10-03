#pragma once

namespace core
{
	// Chamber always has an explicit supported subtype; Airlock is independent.
	enum class ChamberSubtype { SecurityScanner, Decontamination };

	constexpr bool isSupportedChamberSubtype(ChamberSubtype subtype)
	{
		return subtype == ChamberSubtype::SecurityScanner || subtype == ChamberSubtype::Decontamination;
	}
}
