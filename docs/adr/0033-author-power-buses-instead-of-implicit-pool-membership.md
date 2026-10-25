# Author Power Buses instead of implicit electrical pool membership

Replace ADR-0023's automatic electrical pool membership with authored Power Buses and role-based source, storage and consumer membership (#878). Explicit circuits let designers isolate and size distribution instead of implying a supply path between every generator and every load; Single, Transfer and Shared describe load-side supply relationships, not separate switch nodes or bus ties.

Bus protection is a threshold-plus-persistence latched Trip. Its Critical Alert is an always-on code-bound announcement, never a Monitor or an actuation input; ADR-0031's observation/actuation boundary remains intact. Unrated buses never trip.

## Staged transition

#879 is the expand step only: settings, persistence, validation of bus identity/numeric fields, and a snapshot wrapper. The allocator and derived diagram Power node remain unchanged until #880/#881; Transfer/Shared execution and breaker protection arrive in #882–#884. Multi-bus snapshot rows expose metadata only until per-bus allocation exists; the unchanged pool account is attributable only when there is one bus. No new protection behaviour is claimed by the data model.

Schema 124 carries non-default bus topology and membership. Default-only settings retain their historical wire shape and feature floor, like other optional Station features, so earlier feature contracts remain testable. Missing legacy fields resolve to exactly one unrated `main` bus and Single membership for all electrical roles; explicit native lists (including invalid empty lists) are never silently repaired. Serialization does not mutate settings or save runtime Trip/transfer state. ADR-0032 remains the independent typed Thermal-node decision; schema 123 remains the existing Thermal-body floor.
