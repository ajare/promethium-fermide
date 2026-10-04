# Location plan

Select a Room, Corridor, or Facade in the World canvas and choose **Location plan** in Selection. The read-only plan opens at the clicked World Level. A single reusable window stays pinned to that Location even when ordinary Selection changes; choosing the button for another Location explicitly retargets it. The Level selector lists World Levels, including the Location's vertical offset, not Location-local supporting-Level offsets.

The grid's horizontal labels are World X, including the Location's horizontal offset. Its four rows represent integer Local depths 0–3, with 0 at the bottom. Local depth describes ordering, not physical distance, World height, or Layer. The Location's Layer is shown separately.

World content is recorded in `WorldDrawList`, clipped to the plan viewport and rendered by MPP into a dedicated offscreen texture. It never overwrites the main World canvas texture before ImGui composites both images.

Closing and reopening starts a fresh view at the selected Level. World replacement, history reconstruction, and target deletion close the transient view rather than rebinding a potentially reused Sector index. Opening again selects the current World-owned Location. The plan adds no document data or persistence schema.

This is ticket #439's empty-grid slice of #438. Furniture drawing, supporting-Floor shading, depth-range expansion, palette changes, and editing gestures are not yet provided. Editor smoke coverage (`locationPlan/workflow`) drives the opening button, pinning, explicit retargeting, Level combo, close/reopen, reconstruction, replacement, and deletion. Render coverage (`locationPlan/grid`) inspects World-X alignment, depth orientation, and intersected clipping at the CPU command-stream boundary.
