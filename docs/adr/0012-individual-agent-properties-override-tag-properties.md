# Individual Agent properties override tag properties

Status: accepted

Agent properties may be authored directly on one Agent as well as supplied by Agent tags. When both sources supply the same property, the individual Agent property wins. Removing it reveals the inherited tag value again without changing tag assignments or definitions.

Individual Walk speed and Height modifiers are concrete authored values rather than sampled ranges. Tag definitions remain reusable distributions whose persisted per-Agent samples provide their effective values. An individual value is already specific to one Agent, so sampling it would add hidden randomness without adding reusable variation.

## Consequences

- Effective property lookup follows one order: individual value, inherited tag value, then the existing default.
- Individual properties are World-owned authored data, persisted with the Agent and editable only while simulation is paused.
- Direct Mobility profiles use the same per-traversal Mobility uses and routing enforcement as tag-supplied profiles.
- Removing an individual property is non-destructive to its underlying tag property and sample.
- Object usage and Object usage distance follow this order independently, with frozen
  script defaults as their final fallback. Tag distance is concrete, not sampled.
  Shared definition edits and property removals preflight each affected effective
  configuration across all loaded dependent Worlds; Arms and Remote control require finite positive
  effective distance, while None ignores it. Individual overrides never hide a
  duplicate inherited source conflict. Invalid edits leave all documents and history
  unchanged.
- Remote Access panels and Remote BoothWindow shutters use the same precedence
  independently of mode, distance and each other. Their script defaults are true;
  they gate only Remote control, not Arms or None. A mode-only Human override
  therefore keeps its separately resolved 0.25-unit distance rather than adopting
  the 1-unit default of a distance-omitting Remote control script declaration.
