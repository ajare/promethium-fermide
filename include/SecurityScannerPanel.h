#pragma once

#include "core/World.h"
#include "core/SecurityScannerTransit.h"

struct SecurityScannerDraft
{
	bool valid{ false };
	uint32_t x{}, y{}, width{};
	bool leftToRight{ true };
	std::string diagnostic;
};

SecurityScannerDraft planSecurityScannerDrag(core::World const& world, uint32_t layer,
	float anchorPositionX, float anchorPositionY, float endPositionX, float endPositionY);
uint32_t commitSecurityScannerDraft(core::World& world, uint32_t layer,
	SecurityScannerDraft const& draft);
void drawSecurityScannerSelectionPanel(core::SecurityScannerTransit const& chamber);
