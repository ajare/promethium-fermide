#pragma once

#include "core/World.h"
#include "core/ChamberTransit.h"
#include "DocumentEdit.h"
#include <optional>

struct ChamberDraft
{
	bool valid{ false };
	uint32_t x{}, y{}, width{};
	bool leftToRight{ true };
	std::string diagnostic;
};

ChamberDraft planChamberDrag(core::World const& world, uint32_t layer,
	float anchorPositionX, float anchorPositionY, float endPositionX, float endPositionY);
uint32_t commitChamberDraft(core::World& world, uint32_t layer,
	ChamberDraft const& draft);
// Structural commands are applied after drawing, so selection replacement cannot
// invalidate the controls' chamber reference partway through the frame.
std::optional<core::World::ChamberEditPlan> drawChamberSelectionPanel(
	std::shared_ptr<core::World> const& world, core::ChamberTransit const& chamber,
	DocumentHistory& history = gWorldDocumentHistory);
