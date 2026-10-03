# World Movement Simulation

A simulation of people moving through a two-dimensional world while doors, lifts, shuttles, ladders, and other shared resources constrain when transitions may occur.

## World and routes

**World**:
The complete simulation aggregate, including its spatial structure, movement network, devices, and agents. A World's size is bounded by a total cell budget (`CORE_MAX_WORLD_CELLS`): each Layer owns one cell per (x, level) position, and the cells summed across every Layer must fit that budget.

**Layer**:
One of the ordered spatial planes used to represent depth in the two-dimensional world. Layers are numbered from front (0) to back. A World may have between 2 and 256 layers.

**Level**:
A vertical position analogous to a floor or storey in a building. Levels are numbered upward from ground level 0.
_Avoid_: Deck; floor or storey when referring to a vertical position

**Floor**:
A physical horizontal surface of a Sector on which an Agent may walk.
_Avoid_: Level, deck, storey

**Sector**:
A region of a Layer in the world's spatial structure. A sector is either a Location, a Transit, or a Background; an agent belongs to a Location or a Transit, never to a Background.

**Location**:
A stationary sector such as a room, corridor, or façade.
_Avoid_: Sector, when specifically referring to stationary space

**Room**:
A named location that may occupy any layer.

**Corridor**:
A location that may occupy any layer.

**Facade**:
A location whose perimeter walls are all open. It is occupiable, hosts every object type a Room hosts, owns walkable floor, and takes part in traversal exactly as a Room does; its only differences are that every wall end on every level is open, and that it is rendered as a solid opaque colour like a Background. The open perimeter is an intrinsic creation property, not an editable state: wall add/remove commands and Bulkhead Doors refuse a Facade.

**Background**:
A non-occupiable Sector that exists only to be seen through Windows and other apertures from the Layer in front. It carries one opaque colour, hosts no objects, owns no walkable floor, and takes no part in traversal. Where a Window looks into a sector, that sector is its Background - no separate domain noun is minted for it.

**Transit**:
A static sector that connects Locations through one or more Stops, such as a lift shaft, shuttle route, ladder, stairwell, or Airlock. Its landing Locations are on the Layer immediately in front, except for an Airlock or Security scanner, which connects Locations on its own Layer; within a lift or shuttle Transit, an Agent may also occupy a specific Transport vehicle or Carriage.

**Stairwell**:
A compact stair transit that may connect several consecutive levels using alternating flights.
_Avoid_: Staircase

**Staircase**:
A single straight flight of steps connecting two adjacent levels. It may rise in either horizontal direction and is bidirectional while stationary.
_Avoid_: Stairwell

**Escalator**:
A staircase whose steps move at a non-zero speed. Its movement direction determines its sole permitted travel direction and its agents' travel speed.

**Stop**:
A place at which a transit resource connects to a location.

**Access zone**:
A connected waiting area at a stop whose usable doors share one logical boarding queue. Disconnected approaches are separate access zones.
_Avoid_: Stop, queue position

**Path**:
An agent's planned sequence of vertices and edges. A path expresses route intent, not permission to traverse every edge immediately.
_Avoid_: Reservation, traversal permit

**Perceived route cost**:
An Agent's subjective, seconds-equivalent judgement of a possible Path, combining expected duration with effort, waiting, interaction, crowding, risk, uncertainty, and personal preference. It is not elapsed journey time.
_Avoid_: Edge weight, actual travel time

**Stair speed modifier**:
An Agent property from 0.5 through 1.5 that multiplies physical and estimated movement speed on stationary Staircases and Stairwells. Its neutral default is 1; it does not alter Ladder speed or moving Escalator belt speed.
_Avoid_: Walk speed modifier, effort aversion

**Permission adherence**:
A boolean Agent property expressing whether the Agent is willing to use an already usable resource when it does not satisfy the applicable permission requirements for operating that resource from its approach side. Its default is true. It changes route and traversal choices but grants no Access permission and does not create a physical authorization barrier. It applies to ordinary manual and control-operated Doors, extended Force Bridges, extended extensible Ladders, and Lift, Platform lift, Shuttle landing, and Airlock outside-control requirements; it does not prevent an already-boarded passenger or admitted Airlock occupant from completing its committed exit.
_Avoid_: Access permission, Location permission requirement

**Interaction aversion**:
An Agent property from 0 through 3 that weights the perceived inconvenience of Door activation and threshold interaction. Its neutral default is 1; it changes route preference, not whether an interaction is possible.
_Avoid_: Mobility profile, interaction capability

**Effort aversion**:
An Agent property from 0 through 3 that weights physical-effort components of perceived route cost. Its neutral default is 1; it does not alter objective duration or physical movement speed.
_Avoid_: Stair speed modifier, mobility profile

**Waiting aversion**:
An Agent property from 0.5 through 3 that weights known and expected waiting components of perceived route cost. Its neutral default is 1; it does not alter actual device timing or queue service.
_Avoid_: Effort aversion, interaction aversion

**Crowd aversion**:
An Agent property from 0 through 3 that weights discomfort from locally observed access-zone density. Its neutral default is 1; it does not alter capacity, queue order, or waiting estimates.
_Avoid_: Waiting aversion, capacity

**Risk aversion**:
An Agent property from 0 through 3 that weights finite perceived risk on Ladders, exposed Force Bridges, and other risk-bearing traversals. Its neutral default is 1; it does not alter feasibility, physical duration, or accident behaviour.
_Avoid_: Mobility profile, effort aversion

**Route observation**:
Information locally available to an Agent when it chooses or revises a Path, such as a nearby queue or visible device state. The live state of a remote resource is not a Route observation.
_Avoid_: Global simulation state, perfect knowledge

**Route familiarity**:
An Agent property from 0 through 1 that reduces uncertainty premiums and stable perception variation for uncertain route components. Its neutral default is 0.5; it improves confidence in baseline expectations but never reveals remote live queues or device states.
_Avoid_: Route observation, topology knowledge

**Route persistence**:
An Agent property from 0 through 1 that sets the minimum proportional perceived-cost improvement required before voluntarily replacing a still-valid Path. Its default is 0.15; hard Path invalidation bypasses it.
_Avoid_: Route lock, forbidden traversal

**Route planning**:
A transient movement state in which an Agent remains stationary and retains its destination intent for a sampled interval of simulation time, then calculates and selects a Path at interval completion. Pause and deactivation freeze the interval; a Route-planning Agent is otherwise active, and successful active movement resumes on the following tick.
_Avoid_: Background planning, completed replan

**Minimum route planning time**:
An Agent property from 0.1 through 10 seconds that sets the inclusive lower bound of each Route planning interval, with a default of 1 second. Each episode samples uniformly between the effective bounds rounded upward to whole simulation ticks.
_Avoid_: Route calculation time

**Maximum route planning time**:
An Agent property from 0.1 through 10 seconds that sets the inclusive upper bound of each Route planning interval, with a default of 3 seconds. An effective maximum below the effective minimum is treated as equal to that minimum without changing either authored property.
_Avoid_: Route calculation timeout

**Marker**:
A named, World-owned authored point in a Location with stable identity and a set of Marker properties. A Marker may be selected as an Agent behaviour's destination, and its identity survives rename. Agent behaviours cannot choose arbitrary Vertices as destinations.
_Avoid_: Vertex, destination vertex

**Blocks pathing**:
A Marker property that prevents a Path from using the Marker as an intermediate waypoint. The Marker remains valid as a Path's origin or destination.
_Avoid_: Obstacle, which suggests a physical collision boundary

**Route loss**:
The condition in which an Agent's selected destination has no valid Path, either when movement first begins or after the simulation attempts to replace an invalidated Path. An Agent behaviour may respond by choosing a new destination.
_Avoid_: Replan, which recalculates a Path to the same destination

**Skippable path vertex**:
An intermediate waypoint that an agent need not physically visit when the agent and the next two physical vertices share a layer, those vertices lie horizontally on opposite sides of the agent at the same height, and no interaction or other specific action is required at the nearer vertex. Coincident topology-only vertices do not count as distinct physical waypoints.
_Avoid_: Removing the vertex from the authored path

**Transport journey**:
An agent's coordinated use of one transport vehicle, from waiting and boarding at an origin stop through disembarking at a destination stop.
_Avoid_: Path, ride edge

**Trip intent**:
An agent's desired origin and destination for a transport journey before or while the vehicle accepts the request.
_Avoid_: Stop request, reservation

**Stop request**:
An active demand for a transport vehicle to visit a stop, created by a landing call or an onboard destination selection.
_Avoid_: Trip intent, queue ticket

**Threshold**:
The controlled boundary between two sectors, such as a doorway or the entrance to a lift.
_Avoid_: Vertex, when discussing physical movement rules

**Door**:
A threshold that connects two adjacent layers. It is authored on the front layer of the pair and opens into the layer immediately behind it. A regular Door has a one-level footprint. An ordinary Door authored in a Room may be regular height or 0.9-unit tall; Corridor, Facade, Lift, and Shuttle Doors always use regular height. Lift and Shuttle landing doors belong to their transport.
_Avoid_: Portal

**Bulkhead Door**:
A threshold in a shared wall between two adjacent Locations on the same Layer. While closed or opening it controls passage; once fully open it is an unconstrained bidirectional passage. In automatic mode it detects every Agent within its automatic sensor distance on either side, whether or not that Agent intends to cross.
_Avoid_: Door, when the same-Layer distinction matters

**Airlock**:
A stationary, capacity-limited Sector connecting Rooms or Corridors on the same Layer through two interlocked Bulkhead Doors, at least one of which remains fully closed. Its chamber holds one Agent per cell of horizontal length, and each admitted batch exits opposite its entry side after a closed-door cycle.
_Avoid_: Transport vehicle, paired ordinary Bulkhead Doors

**Security scanner**:
A directional, capacity-one stationary Transit connecting Rooms or Corridors on the same Layer through two interlocked Bulkhead Doors, with fixed entry and exit sides independent of its horizontal length. It automatically scans the admitted Agent before releasing it through the exit.
_Avoid_: Airlock, paired ordinary Bulkhead Doors, security checkpoint when referring to this chamber

**Airlock cycle**:
The waiting interval that begins when both Airlock Bulkhead Doors are fully closed and must complete before either may open.
_Avoid_: Door opening time

**Automatic sensor distance**:
At an automatic Bulkhead Door, the authored maximum physical gap between the Door and an Agent's nearest edge at which the Agent's presence requests opening. It is independent of the Agent's Path.
_Avoid_: Queue distance, crossing width

**Crossing width**:
At a Door, the symmetric distance either side of the threshold vertex's x position within which an agent on the threshold row may begin crossing. Derived from the physical doorway width (cell width minus the door's x insets) minus the agent's width; a 1-cell door yields +/-0.2. The width relaxes where a crossing starts, never who crosses: queue order, lane allocation, and safety interlocks are unchanged, and the band never stops an agent.
_Avoid_: Door width, doorway width, when referring to the arrival band

**Window**:
A threshold, similar to a Door, that connects two adjacent layers and is authored on the front layer of the pair. It always looks into the layer directly behind it, so it can never sit on the back-most layer.

**BoothWindow**:
A one-cell-wide, one-cell-high service aperture between two Locations on adjacent Layers, with an openable shutter and an approach point on each side. It never permits movement between Layers; Agents operate its shutter through an invisible Interaction point on its back side.

**Walkway**:
A traversable floor within a multi-level room, above that room's ground-level floor.

**Force Bridge**:
An extensible floor that spans a gap between two floor supports—a ground-level Floor, a Walkway, or one of each—within one Room. While fully extended it behaves as ordinary walkable floor: any number of Agents may traverse it concurrently in either direction.

**Platform lift**:
An open transport vehicle within one room. Its ground stop is mandatory, and selected walkway stops connect it to higher levels in the same column. Walkways in that column need not all be stops.
_Avoid_: Lift, when distinguishing the open room object from an enclosed lift transit

## Actors and shared resources

**Agent**:
A simulated person with a position, destination path, and current movement state.

**Agent behaviour**:
A reusable state-machine definition that may direct many Agents. Each assigned Agent runs an independent instance of the behaviour.
_Avoid_: Agent group, Agent tag

**Agent behaviour configuration**:
Authored data supplied to one Agent's behaviour instance according to its Agent behaviour schema. Agents using the same Agent behaviour may have different configurations, such as different schedules.
_Avoid_: Behaviour state, which is the instance's changing runtime state

**Agent behaviour schema**:
The named, typed fields an Agent behaviour requires in each Agent behaviour configuration. A Marker field is selected by name but retains the Marker's stable identity.
_Avoid_: Behaviour configuration, behaviour state

**Agent behaviour registry**:
A collection of reusable Agent behaviours that forms one shared behaviour namespace for the Worlds that reference it.
_Avoid_: Agent tag registry, script directory

**Agent tag**:
A named reusable set of Agent properties that may be assigned to many Agents.
_Avoid_: Agent group, label

**Agent property**:
One typed appearance or behaviour value authored directly on an Agent or supplied by an Agent tag. An individual Agent property overrides the same property inherited from a tag.
_Avoid_: Agent attribute

**Individual Agent property**:
An Agent property authored directly on one Agent. Its concrete value takes precedence over the same property supplied by any assigned Agent tag.
_Avoid_: Agent tag property, per-Agent tag

**Agent property namespace**:
An optional category that groups Agent properties in editor surfaces without becoming part of the property's name. Properties without one belong to the default, unlabelled group.
_Avoid_: Property name prefix, Agent tag

**Mobility profile**:
An Agent property that assigns one Mobility use to each traversal kind: Can use, Cannot use, or Only if no other option. Buttons describes use of interaction points and also governs every traversal whose authored resource requires one.
_Avoid_: Bitfield, capability probe

**Mobility use**:
An Agent's authored relationship to one traversal kind. Can use admits it normally; Cannot use excludes it from routing and refuses it at the runtime traversal gate; Only if no other option admits it only when no Path exists without last-resort traversals.
_Avoid_: Accessibility preference, perceived route cost

**Forbidden traversal**:
A transit, threshold, or button-operated resource whose effective Mobility use is Cannot use. It is excluded from routing and refused at the runtime traversal gate.
_Avoid_: Last-resort traversal, temporary obstruction

**Last-resort traversal**:
A transit, threshold, or button-operated resource whose effective Mobility use is Only if no other option. Routing excludes all such traversals on its first attempt and admits them on a second attempt only after finding no Path; runtime traversal then permits the selected Path.
_Avoid_: Forbidden traversal, high perceived route cost

**Agent tag registry**:
A collection of Agent tags that forms one shared tag namespace for the Worlds that reference it.
_Avoid_: Tag list, Agent group registry

**Agent tag assignment**:
An association between one Agent and one Agent tag.
_Avoid_: Agent group membership

**Agent group**:
A named, World-scoped classification that may be assigned to an Agent for administrative organisation. An Agent group retains its identity when renamed; deleting it leaves its Agents with no Agent group. It does not confer inherited simulation behaviour, but may be used to activate or deactivate all its current Agents at once; each Agent retains its own activation and may be changed individually afterwards.
_Avoid_: Group, when the broader term could be ambiguous

**Shared resource**:
A constrained facility that agents must coordinate to use, such as a lift car, shuttle carriage, ladder, stairwell, or doorway.

**Transport vehicle**:
An independently scheduled moving resource: either a lift car or a complete coupled shuttle.
_Avoid_: Transit, transport car

**Carriage**:
A capacity-owning passenger compartment within a shuttle. All carriages in a shuttle move and are scheduled together.
_Avoid_: Vehicle, shuttle

**Capacity**:
The maximum number of agents that may occupy a shared resource at once.

**Occupant**:
An agent that has completed entry into a shared resource and consumes one unit of its capacity.
_Avoid_: Waiting agent, reserved agent

**Waiting agent**:
An agent that has requested a transition but has not yet received permission to enter or cross.
_Avoid_: Occupant

**Reservation**:
Exclusive permission for one agent to use a queue position, doorway crossing slot, or capacity slot.
_Avoid_: Path

**Queue ticket**:
An agent's logical priority in a waiting line, independent of where that agent is physically standing.
_Avoid_: Queue position, traversal permit

**Queue position**:
A reserved waiting position near a threshold. Queue positions keep waiting agents separated without imposing general collision avoidance on agents merely passing in two dimensions.
_Avoid_: Queue ticket, occupancy slot

## Interaction and coordination

**Door activation mode**:
The permitted way to open a door: automatically by presence, manually by the crossing agent, through a remote interaction point, or not at all.

**Door opening style**:
The visual manner in which a Door's leaf or leaves reveal its threshold: OpenUp, OpenLeft, OpenRight, or OpenApart. Opening style does not change the Door's state, obstruction, or traversal behaviour. A tall OpenUp Door takes proportionally longer to open and close so its leaf moves at the same vertical speed as a regular Door.

**Interaction point**:
A place where an agent can request one or more typed device commands, usually represented by a physical button or switch.
_Avoid_: Device controller, traversal controller

**Broken**:
A condition of a moving traversal device in which operation is unavailable, with device-specific consequences for passage. A Broken Door or extensible device retains its physical position, a Broken transport cannot provide journeys, and a Broken Escalator is a stationary Staircase.
_Avoid_: Forbidden traversal, which describes an Agent's Mobility use rather than device condition

**Remembered device condition**:
An Agent's last locally observed Broken status and traversal-relevant physical condition of a moving traversal device. It persists until replaced by a fresh local Route observation or cleared by simulation reset, and does not reveal remote changes.
_Avoid_: Route familiarity, global device state

**Device command**:
A typed request for a device state change, such as opening a door, extending a bridge, or calling a lift.
_Avoid_: Interaction, traversal request

**Device operation**:
The queryable progress and outcome of accepted device commands.
_Avoid_: Callback, traversal request

**Traversal resource**:
The authority that owns admission, queueing, capacity, reservations, and permits for movement through a shared resource or threshold.
_Avoid_: Traversal controller, device controller

**Access permission**:
A named, World-owned authorization that may be required to operate a protected Interaction point, manually open an ordinary Door, select a Lift, Platform lift, or Shuttle destination, or enter a protected Location. It retains its identity when renamed; deleting it removes it from every requirement, Permission set, and Agent. An Agent may receive an Access permission directly or through an assigned Permission set.
_Avoid_: Traversal permit, which authorizes one specific transition at runtime

**Permission requirement**:
The set of Access permissions required for a protected operation. An Agent satisfies the requirement only when it has every member; an empty requirement allows every Agent. A requirement restricts operating a control, not using a resource that is already locally observed to be usable without that operation. Adding a requirement that an Agent on an affected Path does not satisfy requires Route planning; failure to find a replacement Path produces Route loss.
_Avoid_: Permission set, which grants Access permissions to an Agent

**Transport destination permission requirement**:
The World-owned set of Access permissions required to select one destination Stop of a Lift, Platform lift, or Shuttle, shared by every car, coupled Carriage, and boarding origin. Every member is required, using direct and Permission set grants; an empty requirement is unrestricted, and this operation requirement is distinct from permission to pass through an already usable resource.
_Avoid_: Landing control requirement, which protects calling the transport rather than selecting its destination

**Location permission requirement**:
The set of Access permissions an Agent must hold to enter or use the vertices of a Room or Corridor; every member is required and an empty requirement allows every Agent. It does not apply to Facades and never prevents an Agent from leaving a Location it already occupies.
_Avoid_: Permission requirement, which authorizes a protected operation

**Permission grant**:
An Access permission conferred directly on an Agent or through an assigned Permission set. Authored grants determine the Agent's initial grants; runtime changes made through the Agent Selection panel or an Agent behaviour affect its current grants until the simulation is reset. Losing an effective grant requires Route planning for an affected Path; gaining one uses voluntary Route planning and ordinary Route persistence.
_Avoid_: Traversal permit, Permission requirement

**Permission set**:
A named, reusable World-owned collection of Access permissions that may be assigned to Agents. It retains its identity when renamed; deleting it removes its Agent assignments but leaves its Access permissions intact. An Agent's effective Permission grants are the union of its direct grants and the grants conferred by all its assigned Permission sets.
_Avoid_: Permission requirement, Agent tag

**Traversal permit**:
A short-lived authorization to perform one specific sector transition after all door, capacity, and resource-position conditions are satisfied.
_Avoid_: Path, reservation, Access permission

## Editor rendering and resources

**World canvas**:
The visual editing and simulation surface for a World. Its primitives are recorded in a backend-independent command stream, rendered by MPP to an offscreen texture, and presented inside the Dear ImGui interface.
_Avoid_: ImGui window, when referring specifically to rendered World content

**Application resource manifest**:
`resources/Resources.yaml`, the authoritative catalogue for bundled application assets and their dependencies. User-selected documents may be registered dynamically without modifying this file.
_Avoid_: World document, tile-set metadata

**Image set**:
A Willpower resource that names rectangular regions of one atlas image. Whether a region is tintable or repeated is a World-renderer semantic rather than a separate resource type.
_Avoid_: Tile set
