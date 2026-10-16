#!/usr/bin/env python3
"""Generate the code-referenced Agent path flowchart (stdlib only).

Run from any directory: python3 docs/diagrams/generate_agent_path_flow.py
Source links and line numbers are resolved against the current checkout.
"""
from pathlib import Path
import re
import textwrap
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).with_name('agent-path-evaluation.svg')
PW, PH, GAP = 1420, 3100, 35
TOP = 370
parts = []
ids = set()
links = []
refs = set()


def emit(s):
    parts.append(s)


def text(x, y, lines, cls='body', anchor='middle', leading=22):
    emit(f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}">')
    for i, line in enumerate(lines):
        emit(f'<tspan x="{x}" dy="{0 if i == 0 else leading}">{escape(line)}</tspan>')
    emit('</text>')


def source(file, symbol):
    path = ROOT / file
    lines = path.read_text().splitlines()
    # Match declarations/definitions, not calls to the same function.
    needle = symbol + '('
    for i, line in enumerate(lines, 1):
        if needle in line and (('::' in symbol) or re.match(r'^\s*(?:\[\[\w+\]\]\s*)?[\w:<>,*& ]+\s+' + re.escape(needle), line)):
            refs.add((file, symbol))
            return file, i
    raise ValueError(f'Cannot resolve {symbol} in {file}')


class Panel:
    def __init__(self, key, col, row, title, subtitle):
        self.key = key
        self.x = GAP + col * (PW + GAP)
        self.y = TOP + row * (PH + GAP)
        emit(f'<g id="{key}"><rect x="{self.x}" y="{self.y}" width="{PW}" height="{PH}" rx="20" class="panel"/>')
        text(self.x + 35, self.y + 48, [f'{key}  {title}'], 'heading', 'start')
        for i, line in enumerate(textwrap.wrap(subtitle, 114)):
            text(self.x + 35, self.y + 82 + i * 23, [line], 'subtitle', 'start')
        self.nodes = {}

    def node(self, ident, row, fn, desc, file=None, symbol=None, kind='action', side=False, target=None):
        assert ident not in ids
        ids.add(ident)
        x = self.x + (1150 if side else 445)
        y = self.y + 240 + row * 275
        w = 450 if side else 760
        h = 210 if kind == 'decision' else 174
        self.nodes[ident] = (x, y, w, h)
        emit(f'<g id="{ident}">')
        text(x-w/2+8, y-h/2-8, [ident], 'note-heading', 'start')
        if target:
            links.append(target)
            emit(f'<a href="#{target}">')
        emit(f'<title>{escape(fn + ': ' + desc)}</title>')
        if kind == 'decision':
            points = f'{x},{y-h/2} {x+w/2},{y} {x},{y+h/2} {x-w/2},{y}'
            emit(f'<polygon points="{points}" class="decision"/>')
            fwidth, dwidth = (39, 43) if not side else (25, 27)
        else:
            emit(f'<rect x="{x-w/2}" y="{y-h/2}" width="{w}" height="{h}" rx="{35 if kind == "port" else 10}" class="{kind}"/>')
            fwidth, dwidth = (66, 76) if not side else (37, 43)
        flines = textwrap.wrap(fn, fwidth, break_long_words=True, break_on_hyphens=False)
        dlines = textwrap.wrap(desc, dwidth, break_long_words=False)
        total = len(flines) * 19 + len(dlines) * 21 + 10
        start = y - total / 2 + 14
        text(x, start, flines, 'fn-small' if kind == 'decision' else 'fn', leading=19)
        text(x, start + len(flines) * 19 + 9, dlines, 'body-small' if kind == 'decision' else 'body', leading=21)
        if target:
            emit('</a>')
        if file:
            file, line = source(file, symbol or fn.replace('()', ''))
            emit(f'<a href="../../{file}#L{line}">')
            text(x, y + h/2 + 19, [f'{file}:{line}'], 'ref', leading=15)
            emit('</a>')
        emit('</g>')

    def edge(self, a, b, label='', dashed=False):
        ax, ay, aw, ah = self.nodes[a]
        bx, by, bw, bh = self.nodes[b]
        if ax == bx:
            if by - ay > 300:
                # A skip/case-return must not draw through intervening boxes.
                gx = self.x + 27
                path = f'M {ax-aw/2} {ay} H {gx} V {by} H {bx-bw/2-5}'
                lx, ly = self.x+70, ay+ah/2+53
            elif by > ay:
                start, end = (ax, ay+ah/2+28), (bx, by-bh/2-5)
                path = f'M {start[0]} {start[1]} V {end[1]}'
                lx, ly = ax+15, (start[1]+end[1])/2
            else:
                # Retry loops use the outer left gutter, never pass through nodes.
                gx = self.x + 27
                path = f'M {ax-aw/2} {ay} H {gx} V {by} H {bx-bw/2-5}'
                lx, ly = gx+8, (ay+by)/2
        else:
            direction = 1 if bx > ax else -1
            sx, ex = ax+direction*aw/2, bx-direction*bw/2-5*direction
            mx = (sx+ex)/2
            path = f'M {sx} {ay} H {mx} V {by} H {ex}'
            lx, ly = mx+8, (ay+by)/2-10 if ay != by else ay-12
        emit(f'<path d="{path}" class="edge{" dashed" if dashed else ""}" marker-end="url(#arrow)"/>')
        if label:
            label_lines = textwrap.wrap(label, 12 if ax != bx else 21)
            if ax != bx:
                lx = mx
                ly -= (len(label_lines) - 1) * 18
            text(lx, ly, label_lines, 'edge-label', 'middle' if ax != bx else 'start', 18)

    def end(self, notes):
        text(self.x+35, self.y+PH-145, ['READING NOTES'], 'note-heading', 'start')
        for i, line in enumerate(textwrap.wrap(notes, 125)):
            text(self.x+35, self.y+PH-111+i*23, [line], 'note', 'start')
        emit('</g>')


A = 'src/core/Agent.cpp'
P = 'src/core/Pathing.cpp'
T = 'src/core/SimulationCoordinatorTraversal.cpp'
Q = 'src/core/SimulationCoordinatorQueues.cpp'
I = 'src/core/SimulationCoordinatorInteractions.cpp'
D = 'src/core/SimulationCoordinatorDoors.cpp'
L = 'src/core/SimulationCoordinatorLifts.cpp'
G = 'src/core/SimulationCoordinatorAgents.cpp'
C = 'src/core/RouteTraversalCapture.cpp'
E = 'src/core/RouteTraversalInputs.cpp'
K = 'src/core/SimulationCoordinatorTick.cpp'


def main():
    width = 3 * PW + 4 * GAP
    height = TOP + 3 * (PH + GAP) + 80
    emit(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title description">')
    emit('<title id="title">Agent Path evaluation and vertex-to-vertex movement</title>')
    emit('<desc id="description">Nine linked flowcharts show generic route planning, directed edge evaluation, waypoint approach, resource admission, control operation, transport and chamber journeys, movement and commit, and recovery. Function and source links refer to this repository. Diamonds are inline decisions unless identified as predicate functions. Dashed arrows indicate cross-panel calls or next-tick continuation, not synchronous calls.</desc>')
    emit('''<defs><marker id="arrow" markerWidth="10" markerHeight="10" refX="9" refY="5" orient="auto" markerUnits="strokeWidth"><path d="M0,0 L10,5 L0,10 Z" fill="#48617b"/></marker></defs>
<style>
svg { background:#edf2f7; } text { fill:#152d42; font-family:DejaVu Sans,sans-serif; }
.panel { fill:#fff; stroke:#bdcbd8; stroke-width:2; }
.heading { font-size:28px; font-weight:700; } .subtitle { font-size:18px; fill:#466078; }
.fn { font-family:DejaVu Sans Mono,monospace; font-size:16px; font-weight:700; }
.fn-small { font-family:DejaVu Sans Mono,monospace; font-size:13px; font-weight:700; }
.body { font-size:18px; } .body-small { font-size:15px; }
.action { fill:#edf5ff; stroke:#4f86bd; stroke-width:2; }
.decision { fill:#fff4d6; stroke:#c68c28; stroke-width:2; }
.port { fill:#e8f6ef; stroke:#348361; stroke-width:2; }
.failure { fill:#ffeded; stroke:#bd6666; stroke-width:2; }
.ref { font-size:12px; fill:#48617b; text-decoration:underline; }
.edge { fill:none; stroke:#48617b; stroke-width:2; } .dashed { stroke-dasharray:8 6; }
.edge-label { font-size:15px; font-weight:700; paint-order:stroke; stroke:#fff; stroke-width:6px; stroke-linejoin:round; }
.note-heading { font-size:15px; font-weight:700; fill:#526779; } .note { font-size:17px; fill:#526779; }
a:hover rect, a:hover polygon { stroke:#152d42; stroke-width:4; }
</style>''')
    emit(f'<rect width="{width}" height="{height}" fill="#edf2f7"/>')
    text(60, 70, ['Agent Path evaluation & vertex-to-vertex movement'], 'heading', 'start')
    text(60, 115, ['General decision logic for any Path — not an example route. Read A, then follow the lettered, clickable subchart ports.'], 'subtitle', 'start')
    text(60, 157, ['Blue = function/action   •   Amber diamond = predicate or enclosing-function [inline] condition   •   Green = linked subchart/continuation   •   Red = refusal/recovery'], 'subtitle', 'start')
    text(60, 197, ['SC = SimulationCoordinator. World forwards traversal APIs to SC. Source references are clickable relative links; regenerate after code changes.'], 'subtitle', 'start')
    text(60, 237, ['Solid arrows show local control flow. Dashed arrows show tick continuation or subchart handoff. A subchart return resumes its caller; preparation never grants admission.'], 'subtitle', 'start')
    text(60, 277, ['Scope: autonomous Path following. Player input-specific movement is intentionally not expanded. Panels summarize function-level branches, not every defensive guard.'], 'subtitle', 'start')

    p = Panel('A',0,0,'Overview & tick ordering','Planning produces route intent. Every movement transition still follows the request → allocation → permit → commit protocol.')
    p.node('a0',0,'SC::moveAgentToMarker()','Accept destination Marker and explicitly selected Agent action.',G,'SimulationCoordinator::moveAgentToMarker')
    p.node('a1',1,'SC::beginRoutePlanning()','Sample the planning interval; retain goal and remain stationary.',G,'SimulationCoordinator::beginRoutePlanning')
    p.node('a2',2,'B · Route planning','Evaluate candidate directed transitions; select a Path or report Route loss.',kind='port',target='B')
    p.node('a3',3,'SC::runSimulationPhase()','ResourceAdvancement: advance devices, vehicles, Airlocks and Chambers.',K,'SimulationCoordinator::runSimulationPhase')
    p.node('a4',4,'D · Intent collection','Observe local conditions; approach/skip waypoints; create traversal request.',kind='port',target='D')
    p.node('a5',5,'E · Allocation','Dispatch to resource admission; F handles required controls; G handles journeys.',kind='port',target='E')
    p.node('a6',6,'H · Movement and commit','Move only with an adopted permit. Commit the transfer in the Commit phase.',kind='port',target='H')
    p.node('a7',7,'SC::runSimulationPhase()','Cleanup: progress/timeouts, task cleanup, interaction outcomes, planning expiry, movement goals.',K,'SimulationCoordinator::runSimulationPhase')
    p.node('a8',8,'SC::updateMovementGoals() [inline]','Goal completed, cancelled or lost?',G,'SimulationCoordinator::updateMovementGoals',kind='decision')
    p.node('a9',9,'SC::updateMovementGoals()','Publish DestinationReached / MovementCancelled / RouteLost; execute selected Action on arrival.',G,'SimulationCoordinator::updateMovementGoals',kind='port')
    p.node('ar',8,'SC::advanceTick()','Continue the six phases on the next fixed tick.',K,'SimulationCoordinator::advanceTick',side=True,kind='port',target='a3')
    for n in range(8): p.edge(f'a{n}',f'a{n+1}')
    p.edge('a8','a9','yes'); p.edge('a8','ar','no',True)
    p.end('The D approach subchart runs during Movement; its collected request is handled in the following IntentCollection phase. Each phase skips inactive Agents. Resource advancement and interaction progression continue separately from Path decisions.')

    p = Panel('B',1,0,'Planning & search','Graph::calculatePath() delegates to pathing::findPath(); each candidate directed arc uses C, not runtime admission.')
    p.node('b0',0,'pathing::findPath() [inline]','Graph, target fit, destination occupancy and Location access valid?',P,'findPath',kind='decision')
    p.node('bx',0,'pathing::findPath()','Return nullptr; I handles Route loss or retained valid Path.',P,'findPath',side=True,kind='failure',target='I')
    p.node('b1',1,'Graph::getPathSourceVertex()','Infer source when omitted; seed connected floor interval endpoints, never split controlled edges.', 'src/core/Graph.cpp','Graph::getPathSourceVertex')
    p.node('b2',2,'PathfindingWorkspace::beginRouteDecision()','Freeze profile, Mobility and observations; capture non-Location traversal inputs.',P,'PathfindingWorkspace::beginRouteDecision')
    p.node('b3',3,'pathing::findPath() [inline]','runSearch: Local-depth routes present?',P,'findPath',kind='decision')
    p.node('bd',3,'findDepthContinuousPath()','Search depth-continuous states; Local depth is a secondary objective.',P,'findDepthContinuousPath',side=True)
    p.node('b4',4,'pathing::findPath() [inline]','Expand minimum-priority vertex. Blocking Marker is intermediate?',P,'findPath',kind='decision')
    p.node('bs',4,'pathing::findPath()','Do not expand it. Origin may leave; destination may finish.',P,'findPath',side=True,kind='port',target='b4')
    p.node('b5',5,'C · Directed transition evaluation','evaluateArc() → arcFacts() → captured evaluate() or Location edge facts.',kind='port',target='C')
    p.node('b6',6,'pathing::findPath() [inline]','Arc cost exists and improves the target score?',P,'findPath',kind='decision')
    p.node('br',6,'pathing::findPath()','Skip infeasible/non-improving arc; continue frontier until target or exhaustion.',P,'findPath',side=True,kind='port',target='b4')
    p.node('b7',7,'pathing::findPath()','Update predecessor, perceived score, duration and priority; reconstruct reached Path.',P,'findPath')
    p.node('b8',8,'pathing::findPath() [inline]','Search found a Path?',P,'findPath',kind='decision')
    p.node('bf',8,'PathfindingWorkspace::allowFallbackMobility()','If first pass failed: allow OnlyIfNoOtherOption; repeat once with same observations.',P,'PathfindingWorkspace::allowFallbackMobility',side=True,kind='port',target='b3')
    p.node('b9',9,'SC::advanceRoutePlanning()','At interval expiry: apply Route persistence to valid retained Path; assign selected Path. No Path → I.',G,'SimulationCoordinator::advanceRoutePlanning',kind='port',target='D')
    p.edge('b0','bx','no'); p.edge('b0','b1','yes'); p.edge('b1','b2'); p.edge('b2','b3'); p.edge('b3','bd','yes'); p.edge('b3','b4','no'); p.edge('b4','bs','yes'); p.edge('b4','b5','no'); p.edge('b5','b6'); p.edge('b6','br','no'); p.edge('b6','b7','yes'); p.edge('b7','b8','search end'); p.edge('b8','bf','no, first pass'); p.edge('b8','b9','yes')
    p.end('Depth search also uses evaluateArc() / C. The successful-update branch repeats expansion before reconstruction; it does not stop after one edge. If the fallback search also fails, return nullptr to advanceRoutePlanning() / I. CannotUse is never relaxed.')

    p = Panel('C',2,0,'Directed arc facts & vertex pairs','The dispatch key is EdgeType plus directed source/target Sector. VertexType alone does not determine permission or motion.')
    p.node('c0',0,'PathfindingWorkspace::arcFacts() [inline]','Target requiredPoseFor() and canAgentAccessLocation() pass?',P,'PathfindingWorkspace::arcFacts',kind='decision')
    p.node('cx',0,'PathfindingWorkspace::evaluateArc()','No cost: exclude arc (Clearance / Permission / Mobility / Control / Direction / PreparationSide / NoContinuation).',P,'PathfindingWorkspace::evaluateArc',side=True,kind='failure',target='b6')
    p.node('c1',1,'RouteTraversalInputs::capture()','Derive source = edge.getOtherVertex(target), observed Sector, rise, length and requiresButton().',C,'RouteTraversalInputs::capture')
    p.node('c2',2,'RouteTraversalInputs::evaluate() [inline]','Traversal Mobility AND required Buttons allowed in this search pass?',E,'RouteTraversalInputs::evaluate',kind='decision')
    p.node('cm',2,'RouteTraversalInputs::evaluate()','CannotUse rejects; OnlyIfNoOtherOption rejects first pass. This authored classification is not current press necessity.',E,'RouteTraversalInputs::evaluate',side=True,kind='port',target='cx')
    p.node('c3',3,'RouteTraversalInputs::capture() [inline]','Location / Gap / Window edge?',C,'RouteTraversalInputs::capture',kind='decision')
    p.node('cl',3,'SectorEdge::getDirectedTraversalFacts()','Location: Room fit/speed. Gap: no continuation. Window: normally traversable aperture only. Return c8.','src/core/SectorEdge.cpp','SectorEdge::getDirectedTraversalFacts',side=True,kind='port',target='c8')
    p.node('c4',4,'RouteTraversalInputs::capture() [inline]','Door/Bulkhead endpoints: ordinary threshold, transport landing, Airlock or Chamber?',C,'RouteTraversalInputs::capture',kind='decision')
    p.node('ct',4,'RouteTraversalInputs::capture()','selectAgentCrossing(); validate both journey apertures, direction, Location access, operation vs locally usable passage. Return c8.',C,'RouteTraversalInputs::capture',side=True,kind='port',target='c8')
    p.node('c5',5,'RouteTraversalInputs::capture() [inline]','Ladder or LadderMount edge?',C,'RouteTraversalInputs::capture',kind='decision')
    p.node('clad',5,'RouteTraversalInputs::capture()','Check endpoint/control side, extension, remembered Broken state, operation or adherence. Return c8.',C,'RouteTraversalInputs::capture',side=True,kind='port',target='c8')
    p.node('c6',6,'RouteTraversalInputs::capture() [inline]','Staircase/Stairwell body or Mount edge?',C,'RouteTraversalInputs::capture',kind='decision')
    p.node('cst',6,'RouteTraversalInputs::capture()','Check mount/body role. Moving Escalator: one-way. Broken Escalator: stationary Staircase. Return c8.',C,'RouteTraversalInputs::capture',side=True,kind='port',target='c8')
    p.node('c7',7,'RouteTraversalInputs::capture()','ForceBridge: Room fit, extension, approach side/control. Lift/Shuttle/Mount: boarding, ride or exit; feasible landing/journey.',C,'RouteTraversalInputs::capture')
    p.node('c8',8,'RouteTraversalInputs::evaluate()','For the matching type only: motion + known/expected wait + interaction + effort + crowding + risk + uncertainty.',E,'RouteTraversalInputs::evaluate')
    p.node('c9',9,'PathfindingWorkspace::evaluateArc()','policy.evaluate(facts, profile) returns weighted perceived cost and objective duration; cache by decision/pass.',P,'PathfindingWorkspace::evaluateArc',kind='port',target='b6')
    p.edge('c0','cx','no'); p.edge('c0','c1','yes'); p.edge('c1','c2'); p.edge('c2','cm','no'); p.edge('c2','c3','yes'); p.edge('c3','cl','yes'); p.edge('c3','c4','no'); p.edge('c4','ct','threshold'); p.edge('c4','c5','other types'); p.edge('c5','clad','yes'); p.edge('c5','c6','no'); p.edge('c6','cst','yes'); p.edge('c6','c7','no'); p.edge('c7','c8'); p.edge('c8','c9')
    p.end('Rows 3–7 summarize alternative type cases, not cumulative requirements. Ordinary Location arcs bypass capture and use SectorEdge facts directly. Live device/queue observations are local; remote planning uses baseline expectations or remembered condition, not global live state. BoothWindows never admit Agents.')

    p = Panel('D',0,1,'Waypoint approach & intent','Path nodes store targetVertex and incoming edge. The next transition uses nodes[targetNode] → nodes[targetNode + 1].')
    p.node('d0',0,'Agent::startPathingInternal()','Validate destination occupancy/access; sync pose; enter MovingToVertex.',A,'Agent::startPathingInternal')
    p.node('d1',1,'Agent::getSkippablePathTarget() [inline]','Can bypass this intermediate physical waypoint?',A,'Agent::getSkippablePathTarget',kind='decision')
    p.node('ds',1,'Agent::getSkippablePathTarget()','Same Layer/height; opposite sides of Agent; same Local depth; no required source action or Chamber waypoint.',A,'Agent::getSkippablePathTarget',side=True)
    p.node('d2',2,'Agent::moveToVertex()','Use original or skippable target; look through coincident topology-only nodes, never skip an action-bearing edge.',A,'Agent::moveToVertex')
    p.node('d3',3,'SC::stopForAvailableQueuePosition()','Can stop at a reserved waiting position before exact endpoint? Airlock occupants keep assigned interior position.',Q,'SimulationCoordinator::stopForAvailableQueuePosition')
    p.node('d4',4,'SC::isAtDoorCrossingArrival()','For ordinary Door, threshold row and crossing-width band suffice; do not force convergence on vertex centre.',Q,'SimulationCoordinator::isAtDoorCrossingArrival')
    p.node('d5',5,'Agent::moveToVertex() [inline]','Queue boundary, Door arrival band, or exact source endpoint reached?',A,'Agent::moveToVertex',kind='decision')
    p.node('dw',5,'Agent::moveToPosition()','Walk at fitting Room-pose speed. F may operate the next needed control while approaching.',A,'Agent::moveToPosition',side=True,kind='port',target='F')
    p.node('d6',6,'Agent::moveToVertex() [inline]','This is the final Path node?',A,'Agent::moveToVertex',kind='decision')
    p.node('de',6,'SC::updateMovementGoals()','Arrival is finalized in cleanup; selected Agent action only, no implicit Furniture use.',G,'SimulationCoordinator::updateMovementGoals',side=True,kind='port',target='a9')
    p.node('d7',7,'Agent::collectTraversalIntent()','WaitingForTraversal with no task: bind next edge, source vertex and destination vertex.',A,'Agent::collectTraversalIntent')
    p.node('d8',8,'SC::createTraversalRequest()','Create owned request; attach queue ticket / Ladder admission and extension lease where applicable.',Q,'SimulationCoordinator::createTraversalRequest')
    p.node('d9',9,'E · Runtime allocation','The next Allocation phase rechecks feasibility and asks the resource for admission.',kind='port',target='E')
    p.edge('d0','d1'); p.edge('d1','ds','yes'); p.edge('d1','d2','no'); p.edge('ds','d2','new target'); p.edge('d2','d3'); p.edge('d3','d4','not stopped'); p.edge('d4','d5'); p.edge('d5','dw','no'); p.edge('d5','d6','yes'); p.edge('d6','de','yes'); p.edge('d6','d7','no / next intent'); p.edge('d7','d8'); p.edge('d8','d9')
    p.end('A physical Interactable waypoint blocks skipping for Arms/None, but Remote control may omit a control-only detour. Gap, threshold, mount/body and Local-depth constraints still apply. Early queue stopping takes precedence over Door crossing-band arrival. An absent/finished Path idles instead of creating a request.')

    p = Panel('E',1,1,'Runtime admission dispatch','Agent::allocateTraversal() rechecks before starting, not in the middle of an admitted crossing. Every branch returns Pending, Granted or refusal.')
    p.node('e0',0,'Agent::allocateTraversal()','Recheck opening fit, Location access, manual Door operation and committed-exit pose. Entry/resource gates repeat hard Mobility constraints.',A,'Agent::allocateTraversal')
    p.node('e1',1,'SC::allocateTraversalRequest() [inline]','Destination fit valid, and resource exists if identified?',T,'SimulationCoordinator::allocateTraversalRequest',kind='decision')
    p.node('ex',1,'I · Refusal / replan','denyTraversalRequest(); authorization/clearance refusals may enter Route planning.',kind='failure',side=True,target='I')
    p.node('e2',2,'SC::allocateTraversalRequest() [inline]','Airlock / Chamber / Lift / Shuttle authority?',T,'SimulationCoordinator::allocateTraversalRequest',kind='decision')
    p.node('eg',2,'G · Journey admission','Includes transport-owned Doors and open Platform lifts; interior Chamber walk waits for release phase.',side=True,kind='port',target='G')
    p.node('e3',3,'SC::allocateTraversalRequest() [inline]','Extensible resource not yet admitting?',T,'SimulationCoordinator::allocateTraversalRequest',kind='decision')
    p.node('ef',3,'SC::allocateExtensiblePreparation()','Choose applicable nearest source-side control; F prepares it; then retry admission.',D,'SimulationCoordinator::allocateExtensiblePreparation',side=True,kind='port',target='F')
    p.node('e4',4,'SC::allocateTraversalRequest() [inline]','Ladder / coordinated Stairwell / ForceBridge?',T,'SimulationCoordinator::allocateTraversalRequest',kind='decision')
    p.node('el',4,'SC::tryGrantLadderAdmissions()','Entry: direction, capacity, spacing. Non-entry grants directly. ForceBridge: tryGrantDoorQueue(). Return e9.', 'src/core/SimulationCoordinatorAdmissions.cpp','SimulationCoordinator::tryGrantLadderAdmissions',side=True,kind='port',target='e9')
    p.node('e5',5,'SC::allocateTraversalRequest() [inline]','Ordinary Door/Bulkhead: Broken, Unavailable or already usable?',T,'SimulationCoordinator::allocateTraversalRequest',kind='decision')
    p.node('eu',5,'SC::tryGrantDoorQueue()','If physical passage, willingness and queue/lane safety pass, grant. Frozen/unavailable unusable aperture → I.',Q,'SimulationCoordinator::tryGrantDoorQueue',side=True,kind='port',target='e9')
    p.node('e6',6,'SC::allocateTraversalRequest() [inline]','RemoteControlled activation mode?',T,'SimulationCoordinator::allocateTraversalRequest',kind='decision')
    p.node('ed',6,'SC::allocateRemoteDoorPreparation()','Select same-source-Sector control, share preparation; F executes the press.',D,'SimulationCoordinator::allocateRemoteDoorPreparation',side=True,kind='port',target='F')
    p.node('e7',7,'SC::allocateTraversalRequest()','Manual ordinary Door: feasible and within capability reach before OpenDoor. Automatic ordinary Door: activate operation. Bulkhead Automatic: wait for spatial sensor. Return e9.',T,'SimulationCoordinator::allocateTraversalRequest',kind='port',target='e9')
    p.node('e8',8,'SC::allocateTraversalRequest()','No shared resource: edge.isTraversable() or requestTraversal(); traversable → grantTraversalRequest(). Window resource: normally traversable only.',T,'SimulationCoordinator::allocateTraversalRequest')
    p.node('e9',9,'Agent::allocateTraversal() [inline]','Granted → adopt permit and go H. Pending → wait/local approach and repeat E next tick. Denied/expired → I.',A,'Agent::allocateTraversal',kind='port',target='H')
    p.edge('e0','e1'); p.edge('e1','ex','no'); p.edge('e1','e8','valid; no authority / Window',True); p.edge('e1','e2','valid resource'); p.edge('e2','eg','yes'); p.edge('e2','e3','no'); p.edge('e3','ef','yes'); p.edge('e3','e4','no'); p.edge('e4','el','yes'); p.edge('e4','e5','Door/Bulkhead'); p.edge('e5','eu','yes'); p.edge('e5','e6','needs opening'); p.edge('e6','ed','yes'); p.edge('e6','e7','no'); p.edge('e8','e9')
    p.end('Dispatch rows are alternatives; no-resource/Window enters e8 directly. Ladder/Bridge/Door cases return to e9, not unrelated gates. Extensible passage repeats Permission adherence even when extended. Fully open standalone Bulkhead Doors allow concurrent bidirectional passage; controlled Door crossing lanes remain coordinated.')

    p = Panel('F',2,1,'Control necessity, reach & press','Device activation and physical crossing are separate. Remote control still uses control ownership, authorization, queue and press duration.')
    p.node('f0',0,'SC::tryAnticipatoryInteraction() [inline]','Remote control Object usage?',I,'SimulationCoordinator::tryAnticipatoryInteraction',kind='decision')
    p.node('fp',0,'SC::tryPhysicalPassingPress()','Arms: may press next Door control in passing. None: cannot operate objects.',I,'SimulationCoordinator::tryPhysicalPassingPress',side=True)
    p.node('f1',1,'SC::discoverUpcomingRemoteTarget()','Find first action-bearing edge after Location legs in current Sector; never activate unrelated nearby objects.',I,'SimulationCoordinator::discoverUpcomingRemoteTarget')
    p.node('f2',2,'SC::remoteControlIsNeeded()','Resolve remoteInteractionControls(); filter source-Sector physical Buttons, permissions, enabled resource and needed command.',I,'SimulationCoordinator::remoteControlIsNeeded')
    p.node('f3',3,'World::agentCanOperateInteraction() [inline]','Agent active, same Sector, non-None, Buttons allowed and eligible geometry/capability?',I,'World::agentCanOperateInteraction',kind='decision')
    p.node('fx',3,'I · Ineligible operation','Rejected/cancelled or no approach. Already usable passage may still be eligible via E/G; None grants no operation.',kind='failure',side=True,target='I')
    p.node('f4',4,'SC::requestInteractionForTraversal()','Arms may queue an autonomous physical approach. Remote must enter eligible range; use limitRemoteInteractionMovement() while walking.',I,'SimulationCoordinator::requestInteractionForTraversal')
    p.node('f5',5,'SC::allocateInteractions()','Revalidate mutable eligibility; serialize control queue; compatible already-activated work can be shared.',I,'SimulationCoordinator::allocateInteractions')
    p.node('f6',6,'SC::moveInteractions() [inline]','Current grants/capability valid, and physical reach/range satisfied?',I,'SimulationCoordinator::moveInteractions',kind='decision')
    p.node('fa',6,'SC::moveInteractions()','Arms approaches until distance ≤ min(arm length, point reach). Remote uses inclusive Euclidean range to Button centre.',I,'SimulationCoordinator::moveInteractions',side=True,kind='port',target='f6')
    p.node('f7',7,'SC::moveInteractions()','Count normal press ticks; pressPhysicalControl(); activate typed operations. Remote pauses locomotion while press work is queued.',I,'SimulationCoordinator::moveInteractions')
    p.node('f8',8,'SC::advanceDeviceOperations()','Pending → Running → terminal. Reuse compatible work; enforce interlocks and Broken state; observe actual device travel.',I,'SimulationCoordinator::advanceDeviceOperations')
    p.node('f9',9,'SC::updateInteractionResults()','Required rejection/failure propagates; otherwise wait or succeed (possibly best-effort failure). Return to E/G; still no traversal permit.',I,'SimulationCoordinator::updateInteractionResults',kind='port',target='E')
    p.edge('f0','fp','no'); p.edge('f0','f1','yes'); p.edge('f1','f2'); p.edge('f2','f3'); p.edge('f3','fx','no'); p.edge('f3','f4','yes'); p.edge('fp','f4','if needed/eligible',True); p.edge('f4','f5'); p.edge('f5','f6'); p.edge('f6','fa','not in reach'); p.edge('f6','f7','yes'); p.edge('f7','f8'); p.edge('f8','f9')
    p.end('Manual ordinary Doors have no separate Button: activateRemoteAnticipatoryInteraction() can issue eligible OpenDoor directly; E handles physical threshold operation. No eligible target means continue Path movement, not automatically fail. Panel/BoothWindow ownership-specific eligibility exists, but neither creates a passage edge.')

    p = Panel('G',0,2,'Transport & chamber journeys','Source/target Sector selects boarding, riding or exit. This panel is called from E for every corresponding journey edge.')
    p.node('g0',0,'SC::allocateLiftTraversal() [inline]','Outside → inside: g1 boarding. Both inside: g4 riding. Inside → outside: g6 exit.',L,'SimulationCoordinator::allocateLiftTraversal',kind='decision')
    p.node('gp',0,'SC::allocateOpenPlatformLiftTraversal()','Open Platform lift: same Room Sector; use mount/body role, occupant and Stop intent rather than Sector change.', 'src/core/SimulationCoordinatorPlatformLifts.cpp','SimulationCoordinator::allocateOpenPlatformLiftTraversal',side=True)
    p.node('g1',1,'SC::allocateLiftBoarding()','Recheck landing/destination willingness and feasible complete journey; attach ticket and Trip intent.',L,'SimulationCoordinator::allocateLiftBoarding')
    p.node('g2',2,'SC::allocateLiftBoarding() [inline]','Locally boardable shared journey, or successful landing call?',L,'SimulationCoordinator::allocateLiftBoarding',kind='decision')
    p.node('gf',2,'F · Call / select control','Call if needed. None may only share a locally available accepted journey; no capability/permission bypass.',side=True,kind='port',target='F')
    p.node('g3',3,'SC::allocateLiftBoarding()','Wait for vehicle alignment, entry aperture, disembark-first order, queue priority, capacity and boarding window; reserve slot then grant.',L,'SimulationCoordinator::allocateLiftBoarding')
    p.node('g4',4,'SC::allocateLiftRiding()','Occupant only: reuse accepted destination, or serialize requestLiftSelectorDestination(). Remote onboard selection is Lift/Platform only, not Shuttle.',L,'SimulationCoordinator::allocateLiftRiding')
    p.node('g5',5,'SC::advanceLiftResources()','Interlocked close → travel → open → disembark → board. Move passengers with vehicle; grant ride at scheduled destination.',K,'SimulationCoordinator::advanceLiftResources')
    p.node('g6',6,'SC::allocateLiftDisembarking()','Reserve safe exit; walk within car/carriage as needed; retain occupancy until crossing commit. Committed exits survive later authorization loss.',L,'SimulationCoordinator::allocateLiftDisembarking')
    p.node('g7',7,'SC::allocateAirlockTraversal()','Alternative Airlock branch: entry control/shared accepted call, entry/exit clearance, same-side batch capacity, interlocked Door admission.', 'src/core/SimulationCoordinatorAirlocks.cpp','SimulationCoordinator::allocateAirlockTraversal')
    p.node('g8',8,'SC::allocateSecurityScannerTraversal()','Alternative Chamber branch: fixed entry/exit, exit fit and Location access; automatic capacity-one Scanner or batched Decontamination.', 'src/core/SimulationCoordinatorSecurityScanners.cpp','SimulationCoordinator::allocateSecurityScannerTraversal')
    p.node('g9',9,'SC::grantTraversalRequest()','Each journey leg grants only when ready. Airlock/Chamber close–process–open progression is resource-owned; return E/H for physical commit.',T,'SimulationCoordinator::grantTraversalRequest',kind='port',target='H')
    p.edge('g0','gp','platform'); p.edge('g0','g1','boarding'); p.edge('g1','g2'); p.edge('g2','gf','call needed'); p.edge('g2','g3','ready'); p.edge('g3','g4','after entry commit',True); p.edge('g4','g5','destination accepted'); p.edge('g5','g6','destination reached'); p.edge('g6','g9','exit ready',True); p.edge('g7','g9','cycle/exit ready',True); p.edge('g8','g9','scan/process/exit ready',True)
    p.end('g7 and g8 are independent entries from E, not stages after a Lift. Vehicle rides and entry/exit are separate requests. Platform contiguous Lift legs form one final-Stop journey; Shuttle carriage/door assignment is coordinated. Interrupted or Broken journeys use safe-exit recovery in I, never teleportation.')

    p = Panel('H',1,2,'Permit, motion & commit','Runtime movement preserves physical source membership until a valid authorized commit; source and target may coincide in 2D but lie on different Layers.')
    p.node('h0',0,'Agent::allocateTraversal()','Adopt Granted permit; record accepted exit pose, coalesce contiguous platform legs, and apply permitted waypoint skipping.',A,'Agent::allocateTraversal')
    p.node('h1',1,'Door::selectAgentCrossing()','Door/Bulkhead: tallest allowed supported pose fitting aperture, including frozen opening; accepted exit pose may be grandfathered.', 'src/core/Door.cpp','Door::selectAgentCrossing')
    p.node('h2',2,'Agent::allocateTraversal()','Set crossing pose/duration; release Furniture use on departure. Moving Escalator samples walk/stand once per admitted task.',A,'Agent::allocateTraversal')
    p.node('h3',3,'Agent::update() [inline]','Ordinary Door crossing duration still running?',A,'Agent::update',kind='decision')
    p.node('hw',3,'Agent::update()','Decrement crossing ticks in place; continue on a later tick.',A,'Agent::update',side=True,kind='port',target='h3')
    p.node('h4',4,'Agent::update() [inline]','Door or enclosed Lift/Shuttle ride commits in place?',A,'Agent::update',kind='decision')
    p.node('hi',4,'Agent::update()','Enter AwaitingTraversalCommit without walking toward the far-side coincident/ride vertex.',A,'Agent::update',side=True,kind='port',target='h8')
    p.node('h5',5,'Agent::update() [inline]','Ladder mount not yet reached horizontally?',A,'Agent::update',kind='decision')
    p.node('hl',5,'Agent::moveToPosition()','First walk to Ladder source x; only then begin vertical climb.',A,'Agent::moveToPosition',side=True,kind='port',target='h5')
    p.node('h6',6,'Edge::getTraversalSpeed()','Resolve type-specific climb/stair/escalator speed; fallback to climb/walk; apply crossing pose ratio. Vehicle translation runs in G.', 'include/core/Edge.h','getTraversalSpeed')
    p.node('h7',7,'Agent::moveToPosition()','Move toward destination; wait if not reached. A Location leg may stop/commit early at the next resource queue boundary.',A,'Agent::moveToPosition')
    p.node('h8',8,'SC::commitTraversal() [inline]','Request/permit live, owner matched, endpoint or in-place rule and reservations/sector/interlocks valid?',T,'SimulationCoordinator::commitTraversal',kind='decision')
    p.node('hc',8,'I · Commit not accepted','Stay AwaitingTraversalCommit; cleanup handles timeout/expiry. Never transfer without authority.',side=True,kind='failure',target='I')
    p.node('h9',9,'Agent::commitTraversal()','Commit sector/occupancy transfer; consume node(s) via nextPathNode(); syncPoseToSector(); cleanup releases ownership → D or arrival A.',A,'Agent::commitTraversal',kind='port',target='D')
    p.edge('h0','h1'); p.edge('h1','h2'); p.edge('h2','h3'); p.edge('h3','hw','yes'); p.edge('h3','h4','no'); p.edge('h4','hi','yes'); p.edge('h4','h5','no'); p.edge('h5','hl','yes'); p.edge('h5','h6','no'); p.edge('h6','h7'); p.edge('h7','h8','reached / Commit'); p.edge('h8','hc','no'); p.edge('h8','h9','yes')
    p.end('h1 is a threshold-only step; other edge types proceed to h2. Bulkhead crossing is physical horizontal movement, ordinary Door crossing is timed in place. Waiting Agents move only toward resource-owned local goals or remote-range approach targets. Crossings finish in their admitted pose even if later conditions change.')

    p = Panel('I',2,2,'Waiting, refusal & recovery','Continuation paths are conditional: waiting does not mean immediate failure, and replanning does not revoke an accepted commitment.')
    p.node('i0',0,'SC::updateTraversalProgressAndTimeouts()','Observe queue/approach/permit progress. Permit expiry returns request to Pending; local-goal retries may deny LocalGoalUnreachable.',Q,'SimulationCoordinator::updateTraversalProgressAndTimeouts')
    p.node('i1',1,'Agent::considerTraversalReplan() [inline]','Unpermitted queued request waited to a replan boundary?',A,'Agent::considerTraversalReplan',kind='decision')
    p.node('iw',1,'E · Continue waiting','Move to assigned queue/local goal; repeat allocation. Already served Airlock/Chamber batch reservation suppresses voluntary replanning.',side=True,kind='port',target='E')
    p.node('i2',2,'World::beginVoluntaryRoutePlanning()','Retain valid Path/intent; sample interval; return B. Route persistence prevents marginal voluntary switching.','src/core/World.cpp','World::beginVoluntaryRoutePlanning',kind='port',target='B')
    p.node('i3',3,'SC::replanAgentAfterAuthorizationRefusal()','Hard invalidation (permission, fit or stale Path): mandatory planning; current valid commitment must finish first.',G,'SimulationCoordinator::replanAgentAfterAuthorizationRefusal')
    p.node('i4',4,'SC::hasCommittedMovement() [inline]','Crossing or occupied journey still committed?',G,'SimulationCoordinator::hasCommittedMovement',kind='decision')
    p.node('ic',4,'SC::requestLiftPassengerSafeExit()','Retain passenger/exit guarantees; defer ordinary planning until safe completion. Broken vehicle may have to wait aligned/open.',L,'SimulationCoordinator::requestLiftPassengerSafeExit',side=True,kind='port',target='G')
    p.node('i5',5,'SC::allocateRemoteDoorPreparation() [inline]','Temporary preparation failed but retry budget remains?',D,'SimulationCoordinator::allocateRemoteDoorPreparation',kind='decision')
    p.node('ir',5,'F · Bounded retry','Remote Door: at most 2 failed attempts, 3-tick retry delay. Lift destination retry uses TraversalWaitingPolicy instead.',side=True,kind='port',target='F')
    p.node('i6',6,'Agent::cancelTraversal()','Cancel/release uncommitted requests and permits; clear local goals/remote approach. Cancellation is requester-local for shared work.',A,'Agent::cancelTraversal')
    p.node('i7',7,'B · Re-evaluate route','At planning expiry calculate replacement; preserve valid retained alternative where applicable.',kind='port',target='B')
    p.node('i8',8,'SC::advanceRoutePlanning() [inline]','No replacement AND no valid retained Path?',G,'SimulationCoordinator::advanceRoutePlanning',kind='decision')
    p.node('iv',8,'D · Resume valid Path','assignPath(); active movement resumes on following tick.',side=True,kind='port',target='D')
    p.node('i9',9,'SC::updateMovementGoals()','Publish RouteLost once. Cancellation/target removal emits its appropriate outcome; behaviour may choose another destination.',G,'SimulationCoordinator::updateMovementGoals',kind='failure')
    p.edge('i0','i1'); p.edge('i1','iw','no'); p.edge('i1','i2','yes'); p.edge('i3','i4','mandatory entry'); p.edge('i4','ic','yes'); p.edge('i4','i6','no'); p.edge('i5','ir','yes / device case'); p.edge('i5','i6','otherwise'); p.edge('i6','i7'); p.edge('i7','i8'); p.edge('i8','iv','no'); p.edge('i8','i9','yes')
    p.end('i3 and i5 are independent refusal/preparation entries as well as recovery gates; voluntary planning need not pass through remote Door retries. Paused simulation freezes planning intervals. Inactive Agents do not walk or press. Safe committed completion is an exception for that journey, never permission for a new admission.')

    text(60, height-35, ['Generated by docs/diagrams/generate_agent_path_flow.py • Source of truth: current C++ implementation • ADRs 0001, 0011, 0014, 0015, 0016, 0017, 0020'], 'note', 'start')
    emit('</svg>')
    for target in links:
        if target not in ids and target not in 'ABCDEFGHI':
            raise ValueError(f'Unknown chart target: {target}')
    OUT.write_text('\n'.join(parts) + '\n')
    print(f'Wrote {OUT.relative_to(ROOT)} ({len(ids)} nodes; {len(refs)} verified source symbols)')


if __name__ == '__main__':
    main()
