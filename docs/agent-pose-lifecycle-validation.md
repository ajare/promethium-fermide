# Agent pose lifecycle and edit integration (#524)

The final #520 slice composes the frozen declarations (#521), shared movement
selection (#522), and declared Furniture requirements (#523).

## Runtime contract

- Path start, delayed/failed planning and same-position Idle do not erase an
  explicit Action posture. Actual departure transfers ownership to the physical
  environment or admitted crossing. Standing is not a privileged reset effect.
- Furniture keeps its posture and occupancy until feasible departure or successful
  replacement. Replacement Actions observe the declared finishing posture without
  publishing it prematurely. Finish/replacement effects are staged together;
  refused batches publish neither posture/claims, device requests nor logs.
- Finishing applies the literal declared finish pose and releases occupancy,
  including callback failure. Script failure keeps the existing reporting and
  pause policy; ordinary capability/fit refusal is not a Lua failure.
- Room ceiling and effective Height edits preflight occupied space. Environmental
  owners may choose another supported movement pose; Action/Furniture owners may
  not be silently substituted. Active Furniture requires both use and finish fit.
  Tag ranges are preflighted at their maximum before sampling or shared-registry
  mutation, respecting individual-over-tag precedence.
- Furniture and Location replay preflight pose feasibility before teardown.
  Surviving replay carries Action ownership with the frozen Agent definition;
  environmental replay resolves the resulting physical Sector. This transient
  carry is not authored persistence. Load/Reset/deleted-Agent restoration retain
  fresh-lifetime resource validation and do not replay old finishing callbacks.
- Admission records the actual supported exit choice. Shuttle admission records
  the selected alighting Door, not merely a graph's preferred Door. If an accepted
  exit later has no fit, completion uses that recorded pose and exposes
  `AgentSnapshot::grandfatheredCrossing`. This flag describes an exception, not
  current physical fit; new admission still refuses. Pause/deactivation freeze
  crossing posture until physical completion.

No runtime pose, acceptance record, ownership snapshot, frozen physical definition
or Lua state is added to World YAML/binary persistence.

## Production-seam coverage

New checks:

- `markerActions/poseOwnershipAndEditSafety`: rejected same-position replacement
  preserves use, occupancy and logs; valid-range Height and ceiling refusals keep
  dirty state; explicit posture survives Idle, failed planning and structural
  replay; a lowered declared finish is literal and low-Room departure never
  introduces impossible Standing; a Standing-only robot's unsupported literal
  Action rolls back pose, claim and logs as an ordinary refusal.
- `shuttles/robotCommittedExit`: the real Standing-only Lua resource completes a
  previously admitted exit after an effective Height change, remains Standing,
  exposes the commitment exception and refuses a subsequent impossible journey.
  Existing Shuttle aperture fixtures provide controlled threshold dimensions.
- `roomHeightScale/occupiedPoseHistory`: production document snapshots/history
  preserve geometry, dirty state and history on a rejected occupied-space edit;
  accepted fitting edits undo/redo with a valid Standing-only Agent.

Existing Marker Action atomicity, Furniture use/finish/callback failure,
structural edits, all Door/chamber/transport families, mixed-resource journeys,
route costs, editor history, and Agent-resource lifetime/persistence checks supply
composition coverage. Dedicated `pose-lifecycle-*` CTests provide focused ownership.

## Validation

Only incremental Release builds and repository-owned CTest are used. No tests
from unchanged submodules are included. Final results are recorded with the #524
commit/closure note.
