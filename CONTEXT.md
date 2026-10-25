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
A static sector that connects Locations through one or more Stops, such as a lift shaft, shuttle route, ladder, stairwell, or Airlock. Its landing Locations are on the Layer immediately in front, except for an Airlock or Chamber, which connects Locations on its own Layer; within a lift or shuttle Transit, an Agent may also occupy a specific Transport vehicle or Carriage.

**Stairwell**:
A compact stair transit that may connect several consecutive levels using alternating flights.
_Avoid_: Staircase

**Staircase**:
A single straight flight of steps connecting two adjacent levels. It may rise in either horizontal direction and is bidirectional while stationary.
_Avoid_: Stairwell

**Escalator**:
A staircase whose steps move at a non-zero speed. Its movement direction determines autonomous Agents' sole permitted travel direction; Player-controlled Agents may walk either way, with their motion combined with the moving steps.

**Local depth**:
A sector-local non-negative integer indicating front-to-back order within one Layer, with 0 front-most and larger values farther back. Numerical differences between Local depths express an Agent's preference for depth continuity between otherwise equally good routes, not physical distance.
_Avoid_: Layer, physical depth

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

**Furniture**:
An object placed in a Room, Corridor, or Facade, with a position, Local depth, and one or more individually defined movement points. Its usable points are Markers; optional side waypoints support routes in front of and behind it.

**Furniture definition**:
A reusable description of Furniture's appearance and movement-point and route layout, shared by its placed instances.
_Avoid_: Furniture instance, TileSet

**Furniture catalogue**:
A reusable collection of Furniture definitions that may be shared by multiple Worlds.
_Avoid_: World, Image set

**Furniture instance**:
One placed piece of Furniture with its own Location, position, Local depth, and distinct usable-point Marker identities, using a shared Furniture definition.
_Avoid_: Furniture definition

**Marker**:
A named, World-owned authored point in a Location with stable identity and a set of Marker properties, either standalone or belonging to Furniture. A Marker may be selected as an Agent behaviour's destination, and its identity survives rename; arbitrary routing vertices are not selectable destinations.
_Avoid_: Vertex, destination vertex

**Blocks pathing**:
A Marker property that prevents a Path from using the Marker as an intermediate waypoint. The Marker remains valid as a Path's origin or destination.
_Avoid_: Obstacle, which suggests a physical collision boundary

**Route loss**:
The condition in which an Agent's selected destination has no valid Path, either when movement first begins or after the simulation attempts to replace an invalidated Path. An Agent behaviour may respond by choosing a new destination.
_Avoid_: Replan, which recalculates a Path to the same destination

**Agent action**:
An explicitly selected activity an Agent performs at a target Marker after physical arrival. Every movement request selects an Agent action, defaulting to Idle when none is specified; arriving at a Marker does not implicitly select another action.
_Avoid_: Usable-point action, which implied automatic Furniture use; Agent behaviour, which directs destination choice; Device command, which requests device operation

**Idle action**:
The immutable built-in Agent action that schedules no activity on arrival. It is universally available, even when the Marker offers no additional Agent actions, and is selected when a movement request omits its Action.
_Avoid_: Deactivation, which stops an Agent's participation; disabling Agent behaviour

**Available Agent actions**:
The ordered additional Agent actions offered at a Marker, beyond the universally available Idle action. A Marker offers no additional Agent actions by default.
_Avoid_: Vertex actions, because authored targets are Markers

**Use furniture action**:
An Agent action at a Furniture-owned Marker that invokes the use associated with that Furniture definition. The selected Marker identifies the usable point being used.

**Furniture use**:
The activity associated with a Furniture definition and shared by its placed instances, performed through the Use furniture action. A Furniture definition may offer no use; sitting and lying are uses rather than implicit destination-arrival effects.

**Finish using furniture**:
The activity paired with a Furniture use that ends an Agent's use of its selected usable point and releases its occupancy. Its resulting bodily stance is the Furniture's finishing posture.

**Occupied**:
The runtime state of a usable point claimed by a seated or lying Agent. An Occupied usable point is an invalid Path destination for every other Agent until the claim is released, but remains usable as an intermediate waypoint.
_Avoid_: Occupant, which describes an Agent inside a shared resource; Reservation, which is an exclusive traversal permission

**Skippable path vertex**:
An intermediate waypoint that an agent need not physically visit when the agent and the next two physical vertices share a layer, those vertices lie horizontally on opposite sides of the agent at the same height, and no interaction, Local-depth change, or other specific action is required at the nearer vertex. Coincident topology-only vertices do not count as distinct physical waypoints.
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
A threshold that connects two adjacent layers. It is authored on the front layer of the pair and opens into the layer immediately behind it. A regular Door has a one-level footprint. An ordinary Door authored in a Room may be regular height or 0.9-unit tall; Corridor, Facade, Lift, and Shuttle Doors always use regular height. Lift and Shuttle landing doors belong to their transport. Ordinary Regular Doors between Rooms, Corridors, or Facades may have an authored Height scale from 0.1 through 1.0 relative to the hardcoded Regular height; omission or reset uses 1. Scaling is bottom-anchored and changes visual height, vertical travel duration, and Standing Agent clearance, not tile footprint, width, or crossing lanes. Door clearance is the fit of an Agent's traversal vertical envelope, including its Pose and applicable Support elevation, below the doorway top relative to the approach Floor. An Agent crosses in the tallest supported, allowed Pose that fits, trying Standing, Crouching, then Crawling, or is refused if no choice fits. This is the same preference used for Room movement; Human allows all three in both contexts. That supported-pose fit rule applies to ordinary Regular Doors, standalone Bulkhead Doors, Lift and Shuttle landing Doors, Airlocks and both Chamber subtypes; Windows and BoothWindows are excluded and gain no traversal behaviour. A crossing moves at the selected Door-context speed ratio of ordinary threshold speed, without preparation/recovery delay. Clearance is a hard planning and new-admission constraint, independent of activation mode, permissions, and Mobility fallback; admitted crossings finish safely. Tall and specialized or transport-owned thresholds refuse an override, including explicit 1.
_Avoid_: Portal

**Bulkhead Door**:
A threshold in a shared wall between two adjacent Locations on the same Layer. While closed or opening it controls passage; once fully open it is an unconstrained bidirectional passage. In automatic mode it detects every Agent within its automatic sensor distance on either side, whether or not that Agent intends to cross.
_Avoid_: Door, when the same-Layer distinction matters

**Airlock**:
A stationary, capacity-limited Sector connecting Rooms or Corridors on the same Layer through two interlocked Bulkhead Doors, at least one of which remains fully closed. Its chamber holds one Agent per cell of horizontal length, and each admitted batch exits opposite its entry side after a closed-door cycle.
_Avoid_: Transport vehicle, paired ordinary Bulkhead Doors

**Chamber**:
A stationary Transit connecting Rooms or Corridors on the same Layer through interlocked Bulkhead Doors. It has a required subtype that defines its admission and journey behaviour; Security Scanner is the creation default; Decontamination Chamber admits width-capacity batches, while Airlock remains separate.
_Avoid_: Airlock, Transport vehicle

**Security Scanner**:
A directional, capacity-one Chamber subtype connecting Rooms or Corridors on the same Layer through two interlocked Bulkhead Doors, with fixed entry and exit sides independent of its horizontal length. It automatically scans the admitted Agent before releasing it through the exit.
_Avoid_: Airlock, paired ordinary Bulkhead Doors, security checkpoint when referring to this chamber

**Decontamination Chamber**:
A directional Chamber subtype with one Agent of capacity per horizontal cell. It admits a batch, performs a shared decontamination sequence with both Bulkhead Doors closed, and releases the batch through its fixed exit.
_Avoid_: Airlock, Security Scanner

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
A one-cell-wide, one-cell-high service aperture on adjacent Layers, with an openable shutter, connecting two Locations or a Dumbwaiter shaft to its landing Location. It never permits Agent movement between Layers; a standalone BoothWindow has an approach point on each side and an invisible back-side shutter Interaction point, while a Dumbwaiter owns its apertures as part of the complete unit and controls their shutters through its landing buttons.

**Dumbwaiter**:
A non-passenger object-service lift with one car in a fixed one-cell-wide shaft spanning exactly two adjacent Levels, two owned BoothWindows, and landing buttons, with its landing Locations on the Layer immediately in front. Agents never enter its shaft or car; each landing button calls the car when it is at the other Level or sends it to the other Level when it is present.
_Avoid_: Lazy waiter, passenger Lift

**Walkway**:
A traversable floor within a multi-level room, above that room's ground-level floor.

**Force Bridge**:
An extensible floor that spans a gap between two floor supports—a ground-level Floor, a Walkway, or one of each—within one Room. While fully extended it behaves as ordinary walkable floor: any number of Agents may traverse it concurrently in either direction.

**Platform lift**:
An open transport vehicle within one room. Its ground stop is mandatory, and selected walkway stops connect it to higher levels in the same column. Walkways in that column need not all be stops.
_Avoid_: Lift, when distinguishing the open room object from an enclosed lift transit

## Actors and shared resources

**Agent**:
A simulated intelligent entity with a position, destination Path, and current movement state.

**Agent type**:
An Agent's immutable physical identity, with a stable type ID, a presentation display name, a complete physical baseline, and a complete default Mobility profile shared in meaning by Agents of that type. An instance freezes both defaults for its lifetime; individual Agent properties and Agent tags modify its effective observations without changing its type.
_Avoid_: Agent tag, which supplies shared Agent properties

**Agent type ID**:
The stable identifier of an Agent type, independent of its presentation display name. A saved Agent records both its type ID and the Agent type resource it resolved from; legacy Human records lacking them resolve to the bundled Human definition.
_Avoid_: Agent type, display name

**Object usage**:
An Agent's resolved mode for operating objects: Arms permits physical operation, Remote control permits distance-limited operation of eligible objects, and None permits no Agent-operated commands. Capability does not grant Access permission or traversal admission.
_Avoid_: Mobility use, which governs traversal kinds

**Object usage distance**:
An independently resolved distance in World units: arm length for Arms, maximum straight-line activation range for Remote control. None ignores it. It describes Agent capability, not an Interaction point's geometry.
_Avoid_: Interaction point reach, which is object geometry and may be zero

**Arms**:
The Object usage mode that operates objects through physical approach within arm length. It does not provide remote activation or bypass Location, side, height, Mobility, Access permission or Broken conditions.

**Remote control object usage**:
The exclusive Object usage mode for distance-limited operation of eligible objects in the Agent's current Sector. It does not include Arms operation and does not replace physical passage or traversal admission.

**Remote Access panels**:
An independently inherited boolean Agent property enabling Access panel operation in Remote control mode. It does not govern Arms or None and confers no authorization.

**Remote BoothWindow shutters**:
An independently inherited boolean Agent property enabling BoothWindow shutter operation in Remote control mode. It is independent of Remote Access panels, does not govern Arms or None, and confers no control ownership or authorization.

**None object usage**:
The Object usage mode in which an Agent cannot operate objects but may pass through already usable resources or join another Agent's accepted journey when existing rules permit. It is neither a Mobility restriction nor a revocation of admitted crossings or committed passenger exits.
_Avoid_: Deactivation, which stops participation in the simulation

**Agent instance**:
One living Agent of an Agent type, with its own private state and fixed physical baseline. A surviving Agent retains its instance across ordinary structural edits; load, Reset, and restoration after deletion start a fresh lifetime.
_Avoid_: Agent type, which defines identity rather than one living entity

**Human**:
An Agent representing a person, with human physical baselines. Human is a bundled script-backed Agent type whose `.agent.lua` definition preserves the historical baselines; legacy Human documents resolve to that bundled definition.
_Avoid_: Agent, when specifically distinguishing a person from other intelligent entities

**Height modifier**:
An Agent property that multiplies its base standing height. An individual value overrides the persisted sample inherited from an Agent tag; the effective value affects appearance and ordinary Door clearance, without redefining unrelated traversal physics.
_Avoid_: Pose height scale, which describes the runtime stance rather than an authored property

**Pose**:
An Agent's runtime bodily stance: Standing (the default), Sitting, Lying, Crouching, or Crawling. A Pose is simulation state, never authored, and determines the Agent's bodily envelope independently of its movement state or Furniture activity.
_Avoid_: Movement state, which describes pathing progress rather than bodily stance

**Crouching**:
A lowered Agent Pose with reduced bodily height relative to Standing, distinct from Sitting and Crawling. Its bodily height uses its Agent type's Crouching height ratio (60% for Human); Actions or applicable automatic context choices may select it when supported and fitting.

**Crawling**:
A low Agent Pose with reduced bodily height relative to Standing, used for passage through Doors that cannot admit the Agent Standing. Its bodily height uses its declared Crawling ratio, and its movement speed uses the applicable context choice (30% of effective Standing height, ordinary Room speed and 50% of ordinary threshold speed for Human).

**Support elevation**:
The physical height of an Agent's support above its supporting Floor or Walkway, distinct from decorative artwork offsets. It contributes to the Agent's top while the Agent remains supported, but does not imply movable Furniture.
_Avoid_: Render offset, Marker height

### Pose capabilities

**Supported pose**:
A bodily stance an Agent type is physically capable of performing. Capability is distinct from whether that stance fits the Agent's current surroundings or is selected automatically.
_Avoid_: Mobility use, which describes access to traversal kinds rather than bodily stances

**Pose envelope**:
The physical bodily extent of an Agent in a particular Pose, derived from its type's base dimensions and that Pose's height and width ratios, used with Support elevation and available space to determine fit. It is distinct from decorative rendering offsets.
_Avoid_: Sprite bounds, decorative offset

**Automatic pose selection**:
The choice of the tallest supported, allowed, physically fitting Pose for a movement context, trying Standing, Crouching, then Crawling for both Room movement and Door crossing. A lack of any fitting choice is a refusal, not a new physical capability.
_Avoid_: Pose capability, which describes possibility rather than selection

**Pose ownership**:
The activity or movement condition that determines an Agent's current bodily stance: its environment, an explicit Agent action, Furniture use, or an admitted crossing. Ownership is distinct from whether the Agent supports or physically fits that stance. Planning and failed requests retain ownership; successful pose replacement or actual departure transfers it.
_Avoid_: Movement state, occupancy

**Furniture use pose**:
The bodily stance a Furniture use requires while the Agent uses its selected usable point. It is shared by instances of the Furniture definition.
_Avoid_: Arrival pose, which suggests implicit use on arrival

**Furniture finish pose**:
The bodily stance required when finishing a Furniture use after releasing its physical support. Together with the Furniture use pose, it determines whether an Agent can fulfil the use's posture requirements.
_Avoid_: Default pose, which need not be the Furniture's required finishing stance

**Grandfathered completion**:
Completion of an admitted crossing or committed journey using its previously accepted supported Pose when later conditions no longer admit a fitting Pose. It is an exception for that commitment, not a successful current-space fit or permission for new admission.
_Avoid_: Physical fit, pose capability

### Agent behaviours and shared resources

**Agent behaviour**:
A reusable state-machine definition that may direct many Agents. Each assigned Agent runs an independent instance of the behaviour.
_Avoid_: Agent group, Agent tag

**Player-controlled Agent**:
An Agent assigned the Player controlled behaviour, eligible for direct human control rather than autonomous destination choice. It remains an Agent of its existing Agent type, not a separate physical identity.
_Avoid_: Player Agent type, Human when describing the control role

**Agent behaviour configuration**:
Authored data supplied to one Agent's behaviour instance according to its Agent behaviour schema. Agents using the same Agent behaviour may have different configurations, such as different schedules.
_Avoid_: Behaviour state, which is the instance's changing runtime state

**Agent behaviour schema**:
The named, typed fields an Agent behaviour requires in each Agent behaviour configuration. A Marker field is selected by name but retains the Marker's stable identity.
_Avoid_: Behaviour configuration, behaviour state

**Agent behaviour registry**:
A collection of reusable Agent behaviours that forms one shared behaviour namespace for the Worlds that reference it.
_Avoid_: Agent tag registry, script directory

**Installed behaviour**:
The runtime-owned association between one Agent and the behaviour closure its behaviour runtime invokes at each boundary. By default the association holds a no-op; assigning an Agent behaviour installs that behaviour's independent per-Agent closure, and the behaviour finishing or being unassigned re-installs the default. It is runtime state, never authored, and an Agent type script must not define a `behaviour` member.
_Avoid_: Agent behaviour, which names the reusable definition rather than its per-Agent installation

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

**Access panel**:
An openable object set into a Location's visible back surface and owned by one of its cells. Opening it exposes functionality determined by its type; an Empty access panel exposes no controls and can only be closed.
_Avoid_: Window, aperture, Button

**Button**:
A physical control representing an Interaction point, owned by the object or Location that requires it. It belongs to one cell even when its visible shape extends slightly into a neighbouring cell.

**Access panel**:
A cell-owned object on the visible back surface of a Room, Corridor, or Facade, identified by cell X and an integer Level offset relative to its Location. It has cell-centred width, physical height, and a bottom Y offset, requires Floor or Walkway support, and owns a floor-Level approach vertex. It is not an aperture, perimeter wall, traversal resource, or movement obstruction; the initial Empty type is reconstructed Closed.

**Button stack**:
A vertical group of physical Buttons in one Location that share a horizontal position and one approach vertex. Each Button retains its own Interaction point, Device commands, and Permission requirement.

**Control approach side**:
The Location-side approach from which an object's control is operated, distinct from the control's horizontal placement beside the object.

**Control placement side**:
The horizontal left or right side of an object or its endpoint on which its control is placed, distinct from the approach from which it is operated.

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

## Station systems

**Station systems**:
A World's coupled electrical power, environmental control and life support, thermal control, and monitoring services. Their resource inventories, processing capacities, demands, and exchanges determine available service and resource endurance.

**Biological population**:
An aggregate of living crops or algae within the environmental control and life support system, with its own living biomass and resource exchanges. Crop and algae populations are distinct; their growth, respiration, harvest, and losses depend on available resources and environmental conditions.

**Living biomass**:
The dry mass of living material in a Biological population. It is distinct from stored harvested food and from the installation's Growing capacity.

**Growing capacity**:
The installation limits within which a Biological population can be supported, including illuminated growing area for crops or active culture volume for algae. It is distinct from the population's current Living biomass.

**Crew demand**:
The aggregate resource requirements and metabolic loads of a declared crew population at a declared activity level. It is distinct from the movement and behaviour of individual Agents.

**Unmet demand**:
The portion of a resource or service requirement that cannot be supplied. It is reported separately from actual consumption and does not count as material available for recovery.

**Protected resource reserve**:
The portion of a usable resource inventory withheld from discretionary use to preserve a declared critical service. Surplus transfers must preserve that reserve and the supplying service's required process resources.

**Fuel-cell reactant stores**:
Dedicated hydrogen and oxygen inventories available to fuel-cell generation, distinct from life-support inventories. They may receive controlled surplus transfers from life support, but fuel cells cannot automatically draw on protected breathing oxygen.

**Resource store**:
A capacity-limited inventory of a specified material held by the Station systems. Its usable contents change through deliveries, production, consumption, transfers, and losses; material awaiting treatment or qualification is not automatically usable.

**Material identity**:
The explicit identity of a material held or exchanged by Station systems, independent of an inventory's name or unit. Material identity is descriptive: it says what a material is, never what may be wired to it. It is distinct from the constituent composition of a compound material.
_Avoid_: Store ID, unit, inferred material name

**Total material inventory**:
The quantity of one identified material across all explicitly accounted Station systems inventories, including qualified and unqualified contents and material outside Resource stores, counted exactly once. A total covering only Resource stores is a Stored material inventory, not a system-wide total.
_Avoid_: Usable inventory, constituent inventory

**Stored material inventory**:
The quantity of one identified material held in Resource stores, including qualified and unqualified contents. It excludes material held elsewhere in Station systems.

**Usable material inventory**:
The portion of a material inventory currently qualified for use; unqualified contents are excluded. Usability does not imply that a Protected resource reserve is available for discretionary use.
_Avoid_: Total material inventory, drawable surplus

**Resupply delivery**:
An external addition of declared resource quantities at a scheduled simulation time, optionally recurring. A delivery cannot silently overfill a Resource store; quantities that cannot be accepted remain explicitly accounted for.

**Critical station service**:
A station service whose declared minimum demand and necessary support services are protected ahead of discretionary demand. If available supply cannot meet that minimum, the deficit remains Unmet demand rather than silently becoming permissible curtailment.

**Service priority**:
The declared ordering used to allocate constrained station resources and curtail discretionary demand. It does not override physical capacity, safety limits, or protected minimums.

**Atmosphere compartment**:
A contained atmospheric inventory with a declared volume, gas composition, water vapour, and temperature. Compartments exchange or lose material only through explicitly accounted pathways — an Atmosphere pathway, a leak, or an airlock cycle — and nothing moves between compartments implicitly. Containment follows from which pathways are wired, never from a declared class label.

**Atmosphere pathway**:
An explicitly authored, undirected link joining two Atmosphere compartments, or one compartment and Exterior vacuum, through which gases move down their own partial-pressure gradients. It is an exchange relationship, not a physical carrier; compartments without a joining pathway never share an atmosphere.
_Avoid_: Door, Hatch, Vent, Air duct, Airlock — these name agent-traversal thresholds or physical carriers, not this exchange

**Exterior vacuum**:
Space outside the Station systems, with fixed zero gas partial pressures and no contained inventory. Gas crossing an Atmosphere pathway into Exterior vacuum is an accounted loss from the station, not material stored in another compartment.
_Avoid_: Infinite compartment, empty compartment (a finite contained volume that can fill)

**Pathway openness**:
The fraction of an Atmosphere pathway currently available to diffusion, from zero to one. Zero means closed but still actuable, not absent. Openness may change during a run under command.
_Avoid_: Open percentage (a presentation form only), valve state, gate

**Pathway conductance**:
An Atmosphere pathway's capacity to pass a given gas, set by that gas's diffusion coefficient together with the pathway's area and openness. Two pathways with the same area and openness always conduct identically for every gas; nothing else can make them differ.
_Avoid_: Throughput (reserved for Station connections), permeability, diffusivity

**Pathway command**:
An instruction that sets an Atmosphere pathway's openness at a stated simulation time. A command takes effect at the time it names, never at the moment it was issued, so the same authored commands produce the same openness trajectory however the run is stepped.
_Avoid_: Immediate write, door signal, valve toggle

**Thermal link**:
An explicitly authored, undirected conduction relationship between two Atmosphere compartments, carrying a declared Thermal link conductance. A Thermal link moves heat down the compartments' own temperature difference and no gas at all; two compartments no link joins never share heat, however long they run. It is the heat mechanism that works without any mixing: a link carries heat and no mass, whereas an Atmosphere pathway can carry heat only as cargo alongside the gas it moves (see Advected enthalpy), so a pathway can never warm a neighbour without also mixing into it. The two are independent aggregates, so a solid wall with no opening and an open hatch through a thin bulkhead are both expressible without faking one to obtain the other. A link is not a compartment's own coupling to a Thermal reservoir either: that coupling is the compartment's heat-only exchanger, which moves heat between a compartment and the reservoir it names, while a link moves heat between two compartments and owns no temperature, no capacity and no service limits of its own. Recorded in ADR-0028, `docs/adr/0028-thermal-links-are-a-dedicated-undirected-conduction-aggregate.md`.
_Avoid_: Heat pathway, thermal conduit, heat duct, radiator (that names a reservoir's rejection of heat to space), heat exchanger (that names a compartment's heat-only coupling to a Thermal reservoir), insulation (the absence of a link, not a kind of link)

**Thermal link conductance**:
A Thermal link's capacity to pass heat, declared in kW/K. It is a single authored number with no per-species, per-area or per-thickness decomposition: two links with the same conductance always behave identically, whatever the compartments they join. It is not a throughput ceiling and not an efficiency — a link loses nothing in transit and is never the limiter on the heat it passes; the temperature difference alone decides how much moves. It must be finite and strictly positive, so a zero-conductance link is refused rather than silently inert. Recorded in ADR-0028, `docs/adr/0028-thermal-links-are-a-dedicated-undirected-conduction-aggregate.md`.
_Avoid_: Thermal pathway, heat duct, radiator (a reservoir's rejection of heat to space by radiation), heat exchanger (a compartment's heat-only coupling to a Thermal reservoir), thermal resistance (its reciprocal, which the model never declares)

**Conducted heat**:
Energy that has crossed a Thermal link. It is booked exactly once on each side as an exact negation — the donor's loss and the receiver's gain are the same number, never two independently rounded ones — so both compartments' energy ledgers close on one figure. Conducted heat is what a link has moved, distinct from the Thermal link conductance that decided how fast it could move, and distinct from exchanged heat, which is a compartment's own heat-only exchanger transfer with a Thermal reservoir. A compartment's conducted total therefore says what it gained or lost through its structure, as opposed to its exchanger, crew, crops or algae. Recorded in ADR-0028, `docs/adr/0028-thermal-links-are-a-dedicated-undirected-conduction-aggregate.md`.

**Advected enthalpy**:
The sensible heat that rides with gas as it crosses an Atmosphere pathway, so moving air carries its source compartment's warmth with it. Between two compartments it is a *difference*: the gas carries the source compartment's Advected specific heat across the two compartments' temperature difference, and each side shifts by that amount against its own thermal capacity — hot gas visibly warms a cold room, and cold gas drawn out of a warm one cools it. It is exactly zero at equal temperature, because mass flow alone is not a heat source. Venting to exterior vacuum has no other temperature to difference against, so the cooling follows the exact integral of `C·dT/dt = −ṁ·cp·T`, giving `T_new = T_K · exp(−cp · m_vented / C)`: the spray-can effect, where a compartment cools as it empties and its authored thermal capacity decides how sharply — a thin inflatable goes properly cold while a heavily-built hab module barely notices. That exponential form is multiplicative on absolute temperature and must be evaluated in kelvin, whereas the inter-compartment difference form is scale-free and may use Celsius; 0 °C is neither an empty compartment nor zero enthalpy. Advected enthalpy is booked once per side as an exact negation and reported separately from Conducted heat, so what a hatch did stays distinct from what a wall did. It never carries latent heat, which is still released only by the destination compartment's own saturation solve, and it is applied after the pathway's mass solve rather than interleaved with it, so the closed-form mass law's constant-coefficient premise still holds. Recorded in ADR-0026, `docs/adr/0026-atmosphere-diffusion-is-per-species-closed-form-and-thermally-neutral.md`.
_Avoid_: Heat flow, conduction (that is a Thermal link), convection (this is not a fluid-dynamics solver), spray-can cooling (the effect, not the term), latent heat (released locally by condensation, never carried)

**Advected specific heat**:
A compartment's authored specific heat in kJ/kg·K, the price at which each kilogram of its departing gas is charged with heat when it crosses a pathway (see Advected enthalpy). It is a bulk property of the mixture standing in that compartment, deliberately not a per-species value, and it must be finite and strictly positive so a zero specific heat is refused rather than silently inert. It is not the compartment's thermal capacity: capacity decides how much energy it takes to move the compartment's own temperature, while specific heat decides how much energy each departing kilogram takes with it. Together they set the realism lever on advective cooling and warming.
_Avoid_: Heat flow (undirected and ambiguous with rate), transferred heat (reserved for Station connections), exchanged heat (the reservoir exchanger's term), shared heat

**Thermal reservoir**:
A body or aggregate of material whose stored thermal energy and thermal capacity determine its temperature over time. Its ability to absorb heat is finite and constrained by the temperature limits of the services it supports. A compartment exchanges heat with a reservoir only through its own heat-only exchanger; a Thermal link never touches a reservoir.

**Heat engine**:
The station's one crossing from a Thermal node to the electrical bus: a machine that draws heat from a hot Thermal reservoir, converts a declared fraction of it to bus generation, and rejects the remainder into a colder rejection reservoir. Its conversion is bounded at the conversion site by the Carnot efficiency of its two interface temperatures at tick start, evaluated in absolute (kelvin) temperature — the deliberate counterpart to a Thermal link's ΔT conduction, which is scale-free and tolerates Celsius. A Heat engine is the exact mirror of a chiller: where a chiller spends work to move heat against a gradient, an engine is paid work by a gradient it never creates. It is not a generator: it burns no fuel and its output is real bus supply, never a load with a refund. When the gradient is dead, the source is below its minimum hot gate, or its gross output cannot cover its own auxiliaries, the engine idles; rejection into a sink at or above its maximum sink gate trips it whole until the sink recovers. Its intake can never carry the hot reservoir below its sink's tick-start temperature, so an engine cannot manufacture the gradient it needs to keep running. Recorded in ADR-0029, `docs/adr/0029-waste-heat-becomes-power-only-through-a-carnot-bounded-machine-with-its-own-rejection-path.md`.
_Avoid_: Motor (names rotation, not the thermodynamic crossing), turbine (a mechanism, not the modelled aggregate), waste-heat recycler (vague), generator (that burns fuel), heat pump (that spends work to move heat uphill — the mirror image)

**Carnot fraction**:
A Heat engine's declared share of the Carnot ceiling it is allowed to capture, in (0, 1]. The engine's actual efficiency is this fraction of the Carnot efficiency at its current interface temperatures, so over-unity conversion is refused at authoring rather than rediscovered at runtime. It is a declared ceiling on capture, not a part-load curve: the engine sits at its fraction whatever its intake, and it is not the engine's rated power, which is declared separately as rated thermal input.
_Avoid_: Efficiency (the actual η is published beside it, not conflated with it), utilisation, capture rate, second-law margin

**Commanded input**:
A Heat engine's intake dispatch, in [0, rated thermal input]: the ceiling the author places on the engine's heat intake for this operating state. Throttling below rated leaves heat deliberately in the hot reservoir for a competing consumer. It is a cap on the request, never a grant: acceptance and supply can still hold the intake below the command, and a command above rated is refused at authoring. On the wire the field travels only when the engine is actually throttled; absent, it means full rated.
_Avoid_: Setpoint (implies a control target the governor chases), throttle position (a mechanism), demand (the load's word)

**Demand acceptance**:
The power the station's generation-and-battery pool can actually take this tick — the demand it has presented plus the charge headroom the batteries can still absorb, less the generation already in the pool. A Heat engine converts only the heat whose power was demand-accepted: unadmitted power means unconverted heat, left in the hot reservoir. This is the exact mirror of the standing rule for energy-fed machines, where a reactor processes only what the actually supplied energy can drive. With zero demand and zero charge headroom the engine extracts nothing and its limiter reads DemandAcceptance.
_Avoid_: Curtailment (the load-side word for reduced supply), dispatch shortfall (the dispatcher's unmet target), rejection (the engine's thermal term)

**Thermal service class**:
A cooling service distinguished by its permitted interface temperatures and compatible heat-transfer requirements. Spare cooling capacity in one class is not automatically usable by another class.

**Microbial processor**:
An aggregate biological waste-treatment or nutrient-recovery process within life support, with finite processing capacity and a condition that affects its performance. Its material and heat exchanges, unrecoverable residues, and dependence on operating conditions are accounted for separately from crop and algae populations.

**Installation**:
A registered Station systems component with a stable identity — a generator, chiller, coolant loop, radiator, processor, or similar — that scenario reductions, maintenance work, and Faults target. It is the unit of Repair.
_Avoid_: Unit, module, device (reserved for traversal equipment)

**Condition**:
A Station installation's continuous state of intactness from zero to one, where one is fully capable. Condition scales the capability the installation can deliver and is moved by Degradation and by funded recovery.
_Avoid_: Health, integrity, quality (reserved for material qualification)

**Degradation**:
A gradual loss of a Station installation's Condition while adverse conditions act on it. Degradation is continuous and reversible only through funded recovery; it is distinct from the sudden onset of a Fault.
_Avoid_: Wear (a Degradation driver, not the state), decay, erosion

**Fault**:
A discrete, identified adverse condition of one Station installation, with an onset time and a recorded cause. A Fault's consequence is Failure of the affected service; the Fault itself is the state, not the event that produced it.
_Avoid_: Broken (reserved for traversal devices), breakdown, defect, damage (the external act that causes a Fault, not the Fault)

**Failure**:
The consequence of a Fault: an installation's inability to deliver its rated service. Failure is a Fault's effect, never a separate object from the Fault that causes it.
_Avoid_: Fault (the state and its consequence are named separately), outage (an authored scenario reduction, not a Fault consequence)

**Trip**:
A protective automatic isolation of a Station installation taken by its own safety gate to prevent further harm, which latches until deliberately reset. A Trip is the machine protecting itself, distinct from a Fault-driven shutdown.
_Avoid_: Shutdown (any stop, protective or not), breaker (a mechanism), fault (a Trip may precede or avert a Fault)

**Monitor**:
A read-only authored observation rule over one Station component or Station connection: a named Metric, one Alert Rule, and an enabled flag. It evaluates the Metric against the Rule and creates Alerts on match. Monitors are disabled by default; a disabled Monitor evaluates nothing and creates nothing. A Monitor never isolates, shuts down, or creates a Fault.
_Avoid_: Sensor (the physical metaphor, not the modelled rule), watchdog (implies acting on timeout), alarm rule (alarm is reserved for existing internal scalars)

**Alert Rule**:
A Monitor's trigger definition: a threshold value with direction (above/below), a trigger-on time the Metric must persist beyond the threshold to fire, and a trigger-off time it must persist back inside before the Alert auto-clears. Hysteresis is temporal only; zero times mean instant fire or clear.
_Avoid_: Deadband (there is no value-band hysteresis), window (reserved for the dashboard's run view)

**Alert**:
The raised condition a Monitor creates when its Alert Rule matches. An Alert is implemented in code as a bound callback function per alert type, so what an Alert does on fire and clear is an implementation decision, never an authored action; the authored surface stays the Rule alone.
_Avoid_: Trip (a protective isolation taken by the machine), Fault (a discrete adverse state, not an observation), warning (a severity level, not the condition)

**Repair**:
A client-initiated action that clears a Fault from a Station installation and restores its service, at cost tiers from a free Trip reset to resource-reserved maintenance work. Repair clears Faults and restores Condition; it never reverses accumulated Wear.
_Avoid_: Fix, restore (funded Condition recovery is one tier, not the whole), reset (the free Trip tier only)

**Nutrient inventory**:
The quantities of nutrient elements held in stores or incorporated into food, living biomass, and waste. Nitrogen, phosphorus, and potassium are accounted for separately, with other minerals grouped in the initial model; total elemental inventory is distinct from nutrients currently available for biological uptake.

**External equipment load group**:
A named aggregate of station equipment outside the Station systems, with declared electrical, thermal, and material exchanges, an operating profile, and service priorities. It excludes equipment whose demands are already calculated within the Station systems; its initially authored inputs may later be supplied by World equipment.

**Terminal waste**:
Material designated for disposal after resource recovery, including unrecoverable residue and recovered material that cannot fit in its appropriate Resource store. Each waste stream is held in its own finite storage; storage exhaustion constrains affected processing rather than deleting material.

**Scheduled waste ejection**:
The removal of Terminal waste to space at declared simulation times from explicitly selected Resource store partitions: qualified stock by default, held stock only when declared, or qualified stock first then held stock for an emergency drain. Ejected material leaves station inventories and is recorded as a material loss; held stock remains unavailable for use.

### Station diagram authoring

**Configuration draft**:
The working Station systems configuration the editor mutates, distinct from the configuration a World is actually simulating. A draft changes nothing in the World until it is applied.
_Avoid_: Working copy, staging config

**Authored node**:
A station diagram node backed by a configuration entry the user created — a Resource store, transfer, conversion, chiller, crew, or any other declared station object.
_Avoid_: Real node, editable node

**Derived service node**:
A station diagram node representing an aggregate service with no configuration entry of its own: the shared electrical pool, and one cooling service per Thermal service class. Drawn for legibility only — never authored, selected, or connected by the user.
_Avoid_: Bus node, virtual node, pseudo-node

**Station connection**:
An individually identifiable relationship between Station systems endpoints, representing a material flow, energy flow, or service relationship. A connection's design properties are distinct from the resource or service actually supplied.

**Connection medium**:
The material, energy, or service represented by a Station connection, distinct from the physical means of carrying it.

**Physical connection type**:
The physical means represented by a Station connection, such as a power cable, gas pipe, liquid pipe, air duct, or material-handling route. An abstract service relationship or ambient biological exchange does not imply a physical carrier.

**Maximum connection throughput**:
The upper bound on a physical Station connection's input rate, expressed in units appropriate to its flow. It is distinct from the delivered rate after connection losses and from a consumer's requested rate.

**Connection efficiency**:
The fraction of a physical Station connection's accepted input delivered to its destination, from zero to one, distinct from equipment conversion efficiency. On non-physical relationships the saved value is informational.

**Connection transport loss**:
Material or energy leaving the modeled Station through an inefficient physical connection, equal to accepted input minus delivered output. It is distinct from heat retained at its source because a connection cannot accept it.

**Retained source heat**:
Produced heat still held at its source because a physical connection cannot accept it. It is neither delivered heat nor connection transport loss.

**Port**:
One declared input or output requirement of an Authored node, shown as a connector. A Port is the visible face of a reference the underlying configuration entry carries, not a separate object.
_Avoid_: Pin, socket, slot

**Growable port set**:
A Port group whose count the user increases within a declared limit, such as a conversion's inputs and outputs or a maintenance job's requirements.
_Avoid_: Dynamic port

**Optional port**:
A Port a node may or may not declare, such as a treatment consumable or a buffer's circulation gate. Always drawn so a node's needs stay visible, marked as unused until connected.
_Avoid_: Empty port

**Port descriptor**:
The structural declaration of a Port's kind, direction, cardinality, and which counterpart kinds it may join. Deliberately too coarse to restate thermal service class, medium, or unit agreement — those belong to configuration validation alone.
_Avoid_: Port type, port schema

**Connection constraint**:
Whether a Port may join a counterpart: structurally from its Port descriptor, semantically from configuration validation. A link is permitted only when both allow it; the descriptor can be more permissive than validation but never less.
_Avoid_: Link rule, compatibility check

**Cascade disconnect**:
Removing an Authored node, or renaming it, together with every link that referenced it, as a single undoable action. A refused cascade leaves no partial state.
_Avoid_: Orphan cleanup, implicit unlink

**Station configuration document**:
A standalone authored document holding one Station systems configuration plus its diagram layout, so a configuration can be created, saved, and loaded independently of any World. Layout is presentation and never simulation input.
_Avoid_: Preset, station file
