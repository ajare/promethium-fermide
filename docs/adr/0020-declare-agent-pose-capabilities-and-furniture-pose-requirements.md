# Declare Agent pose capabilities and Furniture pose requirements

Status: accepted design; API v2 declarations and frozen capabilities implemented (#521);
shared selection, Furniture and lifecycle integration pending (#522–#524)

Agent types declare their supported canonical Poses, physical envelope parameters, and ordered automatic pose choices for Room movement and Door crossing. The host validates and freezes these declarations for each Agent lifetime, selects the first physically fitting eligible pose, and uses the same result for route prediction, admission, movement, and duration estimates. This extends ADR 0019's frozen physical baseline rather than introducing live Lua selection callbacks; a standing-only Agent must never acquire Crawling through a host fallback.

Furniture definitions declare exactly one `use_pose` and one `finish_use_pose` alongside their paired callbacks. The host stages those poses atomically with callback effects and exposes one World eligibility query that checks both Agent capability and target-space fit. Furniture callbacks cannot call `set_pose`; general Marker Actions retain that capability with supported-pose and physical-fit validation. This revises ADR 0018's script-owned Furniture pose effects and unconditional Standing cleanup: finishing restores the declared feasible finish pose and releases occupancy, even if the callback fails.

## Considered options

- **Keep universal poses and merely change their ratios.** Rejected because a robot that cannot crouch or crawl must not route or recover through those poses.
- **Invoke live Lua to choose poses.** Rejected because prediction and actual movement require the same stable decision, including captured route facts; arbitrary callbacks would expand the deferred invocation boundary in ADR 0019.
- **Allow arbitrary pose identities and bounding shapes now.** Deferred. The five canonical poses, their rendering conventions, and existing width behavior remain; scripts declare subsets and applicable ratios.
- **Duplicate numeric clearance thresholds in scripts.** Rejected for this version. Hard thresholds derive from effective dimensions, support, and available clearance; discretionary anticipatory thresholds are a separate future concern.
- **Offer incompatible Furniture use and discover it through execution.** Rejected because declarations let the editor and request APIs refuse before travel, without speculative callback execution.
- **Let Furniture declarations and callback `set_pose` both set posture.** Rejected as two competing sources of truth. Any such callback call is a contract error, even if it requests the declared pose.
- **Silently substitute an environmental pose for a Furniture finish requirement.** Rejected. An Agent must support and physically fulfil both declared poses before using Furniture.

## Consequences

Agent-type API v2 replaces flat pose ratio fields with supported-pose definitions and context-specific automatic choices. Repository resources and fixtures migrate together; external v1 scripts receive migration diagnostics rather than implicitly gaining all poses. No frozen pose snapshot or runtime Pose is added to World persistence. The surviving-instance and fresh-lifetime boundaries in ADR 0019 remain unchanged.

Physical fit is a hard constraint for placement, new admission, ordinary Action effects, and Furniture use. Edits cannot make occupied space impossible or invalidate an active Furniture user's declared finishing posture. Planning preserves existing Action/Furniture ownership until actual departure; release reselects the applicable environmental or crossing pose rather than blindly restoring Standing.

The existing commitment exception is explicit, not a new capability: admitted crossings keep their selected supported pose; a committed transport or chamber exit uses a supported fitting pose, or its previously accepted supported exit pose if later conditions make every choice impossible. Such grandfathered completion does not authorize new admission or claim physical fit. Edit guards continue to protect commitments.

The approved contract, authoring examples, ownership table, testing seams, and implementation issue links are in [Agent pose capabilities](../agent-pose-capabilities.md). This ADR records the agreed direction. Only the declaration/frozen-data slice is implemented and tested; the linked follow-up slices remain pending.
