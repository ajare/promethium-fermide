#pragma once

#include "AtlasInstall.h"

// The Sector sheet the draw path draws, as the handle every Sector draw
// helper takes (#1088).
//
// The draw helpers stopped reading a sheet out of file state: a caller now
// names the sheet it is drawing, and the helper asks its tile questions of
// that sheet and of no other. A check that drives a draw helper directly
// is such a caller, and this is the sheet it names - the one the install
// registry is drawing, which is exactly what `renderSector` hands down.
//
// Resolved at call time rather than cached in a static. Checks clear the
// registry, install another sheet, and ask the same question again
// afterwards; a value read once at start-up would answer every one of
// them with the sheet that happened to be up when the process started.
inline pf::render::InstalledSectorSheet const* drawingSectorSheet()
{
	return pf::render::sectorSheetRegistry().drawing();
}
