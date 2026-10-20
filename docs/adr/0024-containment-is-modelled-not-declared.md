# Containment is modelled through physical state and unmet requirements, not declared and enforced

Status: accepted (#748, #761)

The station model carried a hazard class — `normal` or `hazardous` — on Resource stores, Atmosphere compartments, coolant loops and every water/chemical process, plus a release-clearance chain (`clearanceStoreId`, `clearancePerKg`, `releaseToNormalClass`) that gated treated output and sanctioned the one crossing back to the normal class. The label carried no physical meaning: nothing in the model measured toxicity, pathogens or dose, and the class was a binary proxy the simulation enforced on the author's behalf — refusing cross-class wiring at configuration and holding output until a declared test supply was spent. It was removed from all four domains (#749–#760). Cross-class wiring is now legal: mass moves wherever it is wired, and the consequences live in physical state and reported shortfalls — unmet demand, quality holds on incomplete energy evidence, consumer tolerance throttles, capacity backpressure — rather than in authoring prohibitions. There is deliberately no "boundary crossed" reporting: the consequences are the report (#748 D3).

What the model still refuses is incoherence, not imprudence:

- mass creation or destruction — every ledger closes, every gram is booked;
- non-finite values — NaN and infinities are refused at the settings boundary;
- unknown references — every store, compartment, loop and reservoir a document names must exist;
- capacity violations — a full destination caps or refuses the inflow and names itself as the limiter; overflows never silently vanish mass;
- schema violations — a document carrying a removed field at any version is refused with a conversion-required diagnostic naming the entry and the field, never silently defaulted (World schema 103).

Coherent-but-unwise is always allowed. A treatment may draw its feed from the same store that supplies a drinking-water outlet, and any process may touch any store (#751); the model will not stop the wiring, and it will honestly report what that costs downstream. What still binds is the material itself, not a label on it: the compound-material boundary refuses a transfer between stores whose declared compositions do not close, because that would create constituents out of nothing (#740) — a conservation refusal, not a class refusal.

Two boundaries the removal deliberately keeps:

- **Consumer water-quality tolerances remain.** A `waterQuality` gate reads already-tracked constituents from the source ledger and scales the consumer's throughput. It lives on the consumer, measures an accounting constituent, and produces throughput rather than prohibition: "this crop needs water at ≤ 0.0012 kg/kg other minerals" is a requirement with a spec, not a material tag (#748 D4).
- **Protection priority is authored, never inferred.** `criticalService` is the author's judgement expressed explicitly; the model ranks nothing on its own. The one inferred priority — a `hazardous` label granting protected-service status ahead of discretionary loads — is gone (#754). Deleting protection itself was rejected: the power draw is greedy first-come-first-served, so removing the pass would move the ranking from a visible declaration to an accident of implementation order.

Reversing this decision would mean rebuilding the validation rules, metrics partition, persistence refusals and Lua mirror this removal deleted, and re-adopting a classification the model cannot physically justify. That is why it is recorded here rather than left implicit in the diff.
