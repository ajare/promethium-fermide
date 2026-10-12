-- Station systems slice 5: lifecycle, authored finite Resource stores,
-- bounded authored transfers, compound-resource conversions, solar
-- generation, External equipment load supply, rechargeable battery banks,
-- scheduled resupply deliveries, thermal reservoirs with radiator
-- rejection, coolant loops with freeze-protection heating, chillers
-- supplying class-matched cold services, fuelled generators with
-- startup, ramp, fuel-depletion and shutdown-heat behaviour, compartment
-- atmospheres with occupancy draw, vapour constraints, leakage and
-- scheduled airlock losses, and scheduled airlock cycles. Every
-- system executes this factory in its own budgeted private sandbox.
-- Stores, transfer rules, conversion rules, solar arrays, equipment
-- load contracts, battery descriptors, delivery schedules, thermal
-- reservoir contracts, radiator contracts, chiller contracts,
-- generator contracts, atmosphere compartment contracts and airlock
-- cycle contracts arrive as plain immutable descriptor values;
-- each object initialises its own copy and re-checks the finite-capacity,
-- per-unit conservation, per-constituent compound conservation,
-- power-allocation, battery energy-conservation, delivery-schedule,
-- thermal energy-conservation, chiller and generator fuel-ledger and
-- atmosphere per-component/thermal containment invariants every tick.
-- The host applies the authoritative ledger;
-- this mirror audits the same rules inside the sandbox so a corrupt
-- crossing or model drift fails contained through the integrated error
-- path.
local BALANCE_TOLERANCE = 1e-9
-- Stefan-Boltzmann constant in kW/(m^2 K^4) and the Celsius offset,
-- mirroring core::StationStefanBoltzmannKWPerM2K4 and
-- core::StationCelsiusOffset for the radiator rejection law.
local STEFAN_BOLTZMANN = 5.670374419e-14
local KELVIN_OFFSET = 273.15
-- Ideal-gas and humidity constants (#400): molar masses of the tracked
-- species (kg/mol), the universal gas constant (J/(mol K)) and the
-- latent heat of vaporization of water (kJ/kg). These are constants of
-- the declared working fluids, mirroring the host formulas exactly.
local MOLAR_MASS_OXYGEN = 0.0319988
local MOLAR_MASS_NITROGEN = 0.0280134
local MOLAR_MASS_CARBON_DIOXIDE = 0.0440095
local MOLAR_MASS_WATER = 0.0180153
local GAS_CONSTANT = 8.314462618
local LATENT_HEAT_VAPORIZATION = 2257.0
-- Schedule boundary tolerance: an occurrence scheduled exactly on the
-- fixed-tick grid can land a few ulps either side of it, so due-ness is
-- judged with the same tolerance the host uses. Mirrors
-- core::StationScheduleTolerance.
local SCHEDULE_TOLERANCE = 1e-9

-- #553: a derived magnitude is publishable only when it is finite.
-- Individual authored values are validated finite, but sums and
-- products of finite values can overflow to infinity, and infinity
-- slips through inequality audits (inf > inf is false, and an
-- infinite pool passes every upper-bound comparison). Non-finite
-- derived results are rejected explicitly; ordinary finite
-- shortages remain valid outcomes.
local function isFinite(value)
    return type(value) == 'number' and value > -math.huge and value < math.huge
end

-- One body exchanging heat with a shared reservoir through a passive
-- conductance. The weight is the implicit step's coupling coefficient:
-- the heat this body gains is its weight times the gap between its own
-- tick-start temperature and the coupled end-of-tick reservoir
-- temperature the solver computes for the whole star. Mirrors
-- PassiveExchangeLeaf in the host so both sides book identical numbers.
local function passiveExchangeLeaf(capacityKJPerK, conductanceKWPerK, temperatureC)
    return {
        capacity = capacityKJPerK,
        conductance = conductanceKWPerK,
        temperature = temperatureC,
        weight = 0,
    }
end

-- Integrate one reservoir's whole exchange star over the tick
-- implicitly and return the reservoir's coupled end-of-tick
-- temperature, filling each leaf's weight.
--
-- A passive conductance is solved against the temperature difference
-- *during* the tick, not the one it started with. Solving every edge
-- against the reservoir's end-of-tick temperature rather than its
-- tick-start temperature is what makes all the leaves sharing a
-- reservoir compete for the same finite heat instead of each draining a
-- temperature none of them ever sees move. Eliminating the leaves from
-- that implicit system leaves a closed form: with coupling a = dt*G/C
-- and weight w = dt*G/(1 + a), the reservoir ends at the weighted
-- average of the tick-start temperatures, and each leaf's heat is
-- w x (that average - its own temperature).
--
-- Every weight is non-negative and each new temperature is a weighted
-- average of temperatures that already existed, so the exchange can
-- approach the coupled equilibrium as closely as the conductance allows
-- but can never cross it: no reversing a hot and a cold body, no
-- amplifying their separation, and nothing driven below absolute zero by
-- a stiff conductance. The heat a leaf gains is exactly the negation of
-- what the reservoir gives up, so both ledgers close on the same number.
local function solvePassiveExchange(leaves, reservoirCapacityKJPerK,
    reservoirTemperatureC, fixed_seconds)
    local weightedTemperatureKJ = 0
    local weightKJPerK = 0
    for _, leaf in ipairs(leaves) do
        leaf.weight = 0
        -- A dead conductance or a body with no heat capacity exchanges
        -- nothing, and contributes nothing to the coupled temperature.
        if leaf.conductance > 0 and leaf.capacity > 0 then
            local coupling = fixed_seconds * leaf.conductance / leaf.capacity
            leaf.weight = fixed_seconds * leaf.conductance / (1 + coupling)
            weightedTemperatureKJ = weightedTemperatureKJ + leaf.weight * leaf.temperature
            weightKJPerK = weightKJPerK + leaf.weight
        end
    end
    -- The reservoir's own capacity is always positive, so the weighted
    -- average is always defined; with no live leaves it is just the
    -- reservoir's own starting temperature and nothing moves.
    return (reservoirCapacityKJPerK * reservoirTemperatureC + weightedTemperatureKJ)
        / (reservoirCapacityKJPerK + weightKJPerK)
end

-- The heat a leaf exchanges over the tick once the coupled end-of-tick
-- reservoir temperature is known. Kept as its own expression so the
-- host and the sandbox book the identical arithmetic.
local function passiveExchangeHeatKJ(leaf, coupledReservoirTemperatureC)
    return leaf.weight * (coupledReservoirTemperatureC - leaf.temperature)
end

-- Compound constituents are accounted in kg; the energy term in kJ.
-- The seven mass constituents partition a compound store's wet mass
-- exactly: water is the moisture term and dryOrganics is the dry-mass
-- balance. Compositions use the documented dry-mass basis: moisture is
-- a fraction of wet mass, every other fraction is a fraction of dry
-- mass. Bound nutrient nitrogen is tracked separately from atmospheric
-- nitrogen; the two forms only meet through declared conversions.
local function addConstituents(totals, composition, wetMassKg, sign)
    local dry = wetMassKg * (1 - composition.moisture)
    totals.water = (totals.water or 0) + sign * wetMassKg * composition.moisture
    totals.nutrientNitrogen = (totals.nutrientNitrogen or 0) + sign * dry * composition.nutrientNitrogen
    totals.atmosphericNitrogen = (totals.atmosphericNitrogen or 0) + sign * dry * composition.atmosphericNitrogen
    totals.phosphorus = (totals.phosphorus or 0) + sign * dry * composition.phosphorus
    totals.potassium = (totals.potassium or 0) + sign * dry * composition.potassium
    totals.otherMinerals = (totals.otherMinerals or 0) + sign * dry * composition.otherMinerals
    totals.dryOrganics = (totals.dryOrganics or 0) + sign * dry * (1 - (
        composition.nutrientNitrogen + composition.atmosphericNitrogen
        + composition.phosphorus + composition.potassium + composition.otherMinerals))
    totals.energy = (totals.energy or 0) + sign * dry * composition.energyKJPerKgDry
end

-- Compound materials are mass-denominated; constituents normalize to kg.
local function toKg(unit, mass)
    if unit == 'g' then return mass * 0.001 end
    return mass
end

-- Fuel energy is authored in the store's energy unit; the generator's
-- physics runs in kWh. One factor converts store units to kWh and its
-- reciprocal converts the required fuel energy back, so host and sandbox
-- agree on every draw. Mirrors the host's energyUnitsToKWh.
local function energyUnitsToKWh(unit)
    if unit == 'kJ' then return 1 / 3600 end
    if unit == 'MJ' then return 1000 / 3600 end
    if unit == 'Wh' then return 1 / 1000 end
    return 1
end

return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries,
    reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)
    assert(type(name) == "string")
    assert(type(stores) == "table", "resource store descriptors missing")
    assert(type(transfers) == "table", "resource transfer descriptors missing")
    assert(type(conversions) == "table", "resource conversion descriptors missing")
    assert(type(solar) == "table", "solar array descriptors missing")
    assert(type(loads) == "table", "equipment load descriptors missing")
    assert(type(batteries) == "table", "battery descriptors missing")
    assert(type(deliveries) == "table", "resupply delivery descriptors missing")
    assert(type(reservoirs) == "table", "thermal reservoir descriptors missing")
    assert(type(radiators) == "table", "radiator descriptors missing")
    assert(type(coolantLoops) == "table", "coolant loop descriptors missing")
    assert(type(chillers) == "table", "chiller descriptors missing")
    assert(type(generators) == "table", "generator descriptors missing")
    assert(type(atmospheres) == "table", "atmosphere descriptors missing")
    assert(type(airlockCycles) == "table", "airlock cycle descriptors missing")
    local object = {
        ticks = 0, stores = {}, transfers = {}, order = {},
        conversions = {}, conversion_order = {},
        starting_totals = {}, unit_loss = {}, unit_delivered = {},
        starting_constituents = {}, constituent_loss = {},
        constituent_delivered = {},
        solar = {}, loads = {}, load_order = {}, batteries = {},
        deliveries = {}, reservoirs = {}, radiators = {}, coolant_loops = {},
        chillers = {}, generators = {}, atmospheres = {}, airlock_cycles = {},
    }
    local by_id = {}
    for index = 1, #stores do
        local store = stores[index]
        assert(type(store) == "table", "resource store descriptor must be a table")
        assert(type(store.id) == "string" and #store.id > 0, "resource store needs an id")
        assert(type(store.unit) == "string" and #store.unit > 0, "resource store needs a unit")
        assert(type(store.capacity) == "number" and store.capacity == store.capacity
            and store.capacity > 0 and store.capacity < math.huge,
            "resource store capacity must be finite and positive")
        assert(type(store.qualified) == "number" and store.qualified == store.qualified
            and store.qualified >= 0, "starting usable inventory must be finite and non-negative")
        assert(type(store.unqualified) == "number" and store.unqualified == store.unqualified
            and store.unqualified >= 0, "starting held inventory must be finite and non-negative")
        assert(store.qualified + store.unqualified <= store.capacity,
            "starting inventory exceeds resource store capacity")
        local protected = store.protectedReserve or 0
        assert(type(protected) == "number" and protected == protected and protected >= 0
            and protected <= store.qualified,
            "protected reserve must be finite, non-negative and within starting usable inventory")
        local composition
        if store.composition ~= nil then
            assert(type(store.composition) == "table", "resource composition must be a table")
            assert(store.unit == 'g' or store.unit == 'kg',
                "compound stores must use a mass unit")
            local c = store.composition
            local fields = { 'moisture', 'nutrientNitrogen', 'atmosphericNitrogen',
                'phosphorus', 'potassium', 'otherMinerals', 'energyKJPerKgDry' }
            for _, field in ipairs(fields) do
                local value = c[field]
                assert(type(value) == "number" and value == value and value >= 0
                    and value < math.huge,
                    "composition term '" .. field .. "' must be finite and non-negative")
            end
            assert(c.moisture < 1, "moisture fraction must be below 1")
            local drySum = c.nutrientNitrogen + c.atmosphericNitrogen
                + c.phosphorus + c.potassium + c.otherMinerals
            assert(drySum <= 1, "dry-mass fractions must not exceed 1")
            composition = {
                moisture = c.moisture,
                nutrientNitrogen = c.nutrientNitrogen,
                atmosphericNitrogen = c.atmosphericNitrogen,
                phosphorus = c.phosphorus,
                potassium = c.potassium,
                otherMinerals = c.otherMinerals,
                energyKJPerKgDry = c.energyKJPerKgDry,
            }
        end
        object.stores[index] = {
            id = store.id,
            unit = store.unit,
            capacity = store.capacity,
            qualified = store.qualified,
            unqualified = store.unqualified,
            protected_reserve = protected,
            composition = composition,
        }
        by_id[store.id] = object.stores[index]
        local total = object.starting_totals[store.unit] or 0
        object.starting_totals[store.unit] = total + store.qualified + store.unqualified
        if composition then
            addConstituents(object.starting_constituents, composition,
                toKg(store.unit, store.qualified + store.unqualified), 1)
        end
    end
    for index = 1, #transfers do
        local transfer = transfers[index]
        assert(type(transfer) == "table", "resource transfer descriptor must be a table")
        assert(type(transfer.id) == "string" and #transfer.id > 0, "resource transfer needs an id")
        local source, destination = by_id[transfer.source], by_id[transfer.destination]
        assert(source ~= nil, "transfer source store is unknown")
        assert(destination ~= nil, "transfer destination store is unknown")
        assert(source ~= destination, "transfer source and destination must differ")
        assert(transfer.unit == source.unit and transfer.unit == destination.unit,
            "transfer unit must match both stores")
        assert(type(transfer.rate) == "number" and transfer.rate == transfer.rate
            and transfer.rate > 0 and transfer.rate < math.huge,
            "transfer rate must be finite and positive")
        assert(type(transfer.priority) == "number" and transfer.priority == transfer.priority
            and transfer.priority == math.tointeger(transfer.priority),
            "transfer priority must be a finite integer")
        -- Compound material only moves between identical declared
        -- compositions; crossing a composition boundary is conversion.
        if source.composition ~= nil or destination.composition ~= nil then
            assert(source.composition ~= nil and destination.composition ~= nil
                and math.abs(source.composition.moisture - destination.composition.moisture) <= BALANCE_TOLERANCE
                and math.abs(source.composition.nutrientNitrogen - destination.composition.nutrientNitrogen) <= BALANCE_TOLERANCE
                and math.abs(source.composition.atmosphericNitrogen - destination.composition.atmosphericNitrogen) <= BALANCE_TOLERANCE
                and math.abs(source.composition.phosphorus - destination.composition.phosphorus) <= BALANCE_TOLERANCE
                and math.abs(source.composition.potassium - destination.composition.potassium) <= BALANCE_TOLERANCE
                and math.abs(source.composition.otherMinerals - destination.composition.otherMinerals) <= BALANCE_TOLERANCE
                and math.abs(source.composition.energyKJPerKgDry - destination.composition.energyKJPerKgDry) <= BALANCE_TOLERANCE,
                "transfer crosses a compound-material boundary")
        end
        object.transfers[index] = {
            id = transfer.id,
            source = source,
            destination = destination,
            unit = transfer.unit,
            rate = transfer.rate,
            priority = transfer.priority,
        }
    end
    for index = 1, #conversions do
        local conversion = conversions[index]
        assert(type(conversion) == "table", "resource conversion descriptor must be a table")
        assert(type(conversion.id) == "string" and #conversion.id > 0, "resource conversion needs an id")
        assert(type(conversion.inputs) == "table" and #conversion.inputs >= 1
            and #conversion.inputs <= 4, "resource conversion needs 1 to 4 inputs")
        assert(type(conversion.outputs) == "table" and #conversion.outputs >= 1
            and #conversion.outputs <= 4, "resource conversion needs 1 to 4 outputs")
        local seen = {}
        local inflow, outflow = {}, {}
        local function readFlow(flow, kind, totals)
            assert(type(flow) == "table", "conversion flow must be a table")
            local store = by_id[flow.store]
            assert(store ~= nil, "conversion " .. kind .. " store is unknown")
            assert(not seen[flow.store], "conversion store used more than once")
            assert(store.composition ~= nil,
                "conversion stores require a declared composition")
            local rate = kind == 'input' and flow.rate or flow.yield
            assert(type(rate) == "number" and rate == rate and rate > 0 and rate < math.huge,
                "conversion " .. kind .. " rate must be finite and positive")
            seen[flow.store] = true
            addConstituents(totals, store.composition, toKg(store.unit, rate), 1)
            return { store = store, rate = rate }
        end
        local inputs, outputs = {}, {}
        for flowIndex = 1, #conversion.inputs do
            inputs[flowIndex] = readFlow(conversion.inputs[flowIndex], 'input', inflow)
        end
        for flowIndex = 1, #conversion.outputs do
            outputs[flowIndex] = readFlow(conversion.outputs[flowIndex], 'output', outflow)
        end
        -- Impossible configured yields are rejected at the boundary just
        -- like the host does: a conversion may lose constituents but may
        -- never create one out of nothing.
        for constituent, produced in pairs(outflow) do
            local supplied = inflow[constituent] or 0
            assert(produced <= supplied + BALANCE_TOLERANCE * math.max(1, math.abs(supplied)),
                "conversion '" .. conversion.id .. "' would create constituent '" .. constituent .. "'")
        end
        assert(type(conversion.priority) == "number" and conversion.priority == conversion.priority
            and conversion.priority == math.tointeger(conversion.priority),
            "conversion priority must be a finite integer")
        object.conversions[index] = {
            id = conversion.id,
            inputs = inputs,
            outputs = outputs,
            priority = conversion.priority,
        }
    end
    -- Repeating piecewise-constant profile: a zero period is a constant
    -- level; a positive period repeats segments that start at 0 and rise
    -- strictly. Evaluation takes the last segment started at or before the
    -- current phase, so period boundaries are never ambiguous.
    local function checkProfile(profile, where)
        assert(type(profile) == 'table', where .. ' must be a table')
        assert(type(profile.periodSeconds) == 'number'
            and profile.periodSeconds == profile.periodSeconds
            and profile.periodSeconds >= 0 and profile.periodSeconds < math.huge,
            where .. ' period must be finite and non-negative')
        if profile.periodSeconds == 0 then
            assert(profile.segments == nil or #profile.segments == 0,
                where .. ': a constant profile must not declare segments')
            local level = profile.constantLevel or 1
            assert(type(level) == 'number' and level == level and level >= 0 and level <= 1,
                where .. ' constant level must be finite and from 0 to 1')
            return
        end
        assert(type(profile.segments) == 'table' and #profile.segments >= 1
            and #profile.segments <= 8,
            where .. ' needs between 1 and 8 segments')
        assert(profile.segments[1].startSeconds == 0,
            where .. ': the first segment must start at 0')
        local previous = -1
        for _, segment in ipairs(profile.segments) do
            assert(type(segment.startSeconds) == 'number'
                and segment.startSeconds == segment.startSeconds
                and segment.startSeconds >= 0 and segment.startSeconds < profile.periodSeconds,
                where .. ': segment start must be finite and inside the period')
            assert(type(segment.level) == 'number' and segment.level == segment.level
                and segment.level >= 0 and segment.level <= 1,
                where .. ': segment level must be finite and from 0 to 1')
            assert(segment.startSeconds > previous, where .. ': segments must strictly increase')
            previous = segment.startSeconds
        end
    end
    local function readProfile(profile)
        local segments = {}
        for index = 1, #(profile.segments or {}) do
            segments[index] = {
                startSeconds = profile.segments[index].startSeconds,
                level = profile.segments[index].level,
            }
        end
        return {
            constantLevel = profile.constantLevel or 1,
            periodSeconds = profile.periodSeconds,
            segments = segments,
        }
    end
    for index = 1, #solar do
        local array = solar[index]
        assert(type(array) == 'table', 'solar array descriptor must be a table')
        assert(type(array.id) == 'string' and #array.id > 0, 'solar array needs an id')
        assert(type(array.capacityKW) == 'number' and array.capacityKW == array.capacityKW
            and array.capacityKW >= 0 and array.capacityKW < math.huge,
            'solar array capacity must be finite and non-negative (0 disables)')
        checkProfile(array.illumination, 'solar array ' .. array.id .. ' illumination profile')
        object.solar[index] = {
            id = array.id,
            capacityKW = array.capacityKW,
            illumination = readProfile(array.illumination),
            lastIllumination = 0,
            lastAvailable = 0,
        }
    end
    -- Thermal reservoirs (#395): stored thermal energy and effective
    -- capacity determine temperature over time. The declared min/max are
    -- the service limits; crossing them is reported, never clamped.
    -- Energy is tracked against a 0 degrees C reference: E = C x T.
    local reservoir_by_id = {}
    for index = 1, #reservoirs do
        local reservoir = reservoirs[index]
        assert(type(reservoir) == 'table', 'thermal reservoir descriptor must be a table')
        assert(type(reservoir.id) == 'string' and #reservoir.id > 0, 'thermal reservoir needs an id')
        assert(reservoir.thermalClass == 'low' or reservoir.thermalClass == 'moderate'
            or reservoir.thermalClass == 'warm' or reservoir.thermalClass == 'generation',
            'thermal reservoir needs a declared thermal service class')
        assert(type(reservoir.heatCapacityKJPerK) == 'number'
            and reservoir.heatCapacityKJPerK == reservoir.heatCapacityKJPerK
            and reservoir.heatCapacityKJPerK > 0 and reservoir.heatCapacityKJPerK < math.huge,
            'thermal capacity must be finite and positive')
        local function checkTemp(field, value)
            assert(type(value) == 'number' and value == value and value < math.huge
                and value > -KELVIN_OFFSET,
                'thermal reservoir ' .. field .. ' must be finite and above absolute zero')
        end
        checkTemp('starting temperature', reservoir.startingTemperatureC)
        checkTemp('minimum temperature', reservoir.minTemperatureC)
        checkTemp('maximum temperature', reservoir.maxTemperatureC)
        assert(reservoir.minTemperatureC < reservoir.maxTemperatureC,
            'thermal temperature limits need min below max')
        assert(reservoir.startingTemperatureC >= reservoir.minTemperatureC
            and reservoir.startingTemperatureC <= reservoir.maxTemperatureC,
            'thermal starting temperature must be within its declared limits')
        assert(type(reservoir.absorbedHeatKW) == 'number'
            and reservoir.absorbedHeatKW == reservoir.absorbedHeatKW
            and reservoir.absorbedHeatKW >= 0 and reservoir.absorbedHeatKW < math.huge,
            'absorbed heat must be finite and non-negative')
        object.reservoirs[index] = {
            id = reservoir.id,
            thermalClass = reservoir.thermalClass,
            capacity = reservoir.heatCapacityKJPerK,
            startingTemperature = reservoir.startingTemperatureC,
            minTemperature = reservoir.minTemperatureC,
            maxTemperature = reservoir.maxTemperatureC,
            absorbedKW = reservoir.absorbedHeatKW,
            temperature = reservoir.startingTemperatureC,
            radiators = {},
            lastRouted = 0,
            lastLoopHeat = 0,
            lastHeatIn = 0,
            lastRejected = 0,
            lastNet = 0,
            absorbedKJ = 0,
            routedKJ = 0,
            loopKJ = 0,
            rejectedKJ = 0,
            aboveLimit = false,
            belowLimit = false,
        }
        reservoir_by_id[reservoir.id] = object.reservoirs[index]
    end
    -- Radiators reject a reservoir's heat by radiation: emissivity x view
    -- factor x area x sigma x (T^4 - T_sink^4), with the environmental
    -- back-radiation term explicit.
    for index = 1, #radiators do
        local radiator = radiators[index]
        assert(type(radiator) == 'table', 'radiator descriptor must be a table')
        assert(type(radiator.id) == 'string' and #radiator.id > 0, 'radiator needs an id')
        local host = reservoir_by_id[radiator.reservoir]
        assert(host ~= nil, 'radiator host thermal reservoir is unknown')
        assert(type(radiator.areaM2) == 'number' and radiator.areaM2 == radiator.areaM2
            and radiator.areaM2 > 0 and radiator.areaM2 < math.huge,
            'radiator area must be finite and positive')
        assert(type(radiator.emissivity) == 'number' and radiator.emissivity == radiator.emissivity
            and radiator.emissivity > 0 and radiator.emissivity <= 1,
            'radiator emissivity must be greater than 0 and at most 1')
        assert(type(radiator.viewFactor) == 'number' and radiator.viewFactor == radiator.viewFactor
            and radiator.viewFactor > 0 and radiator.viewFactor <= 1,
            'radiator view factor must be greater than 0 and at most 1')
        assert(type(radiator.sinkTemperatureC) == 'number'
            and radiator.sinkTemperatureC == radiator.sinkTemperatureC
            and radiator.sinkTemperatureC < math.huge
            and radiator.sinkTemperatureC > -KELVIN_OFFSET,
            'radiator sink temperature must be finite and above absolute zero')
        object.radiators[index] = {
            id = radiator.id,
            reservoir = host,
            areaM2 = radiator.areaM2,
            emissivity = radiator.emissivity,
            viewFactor = radiator.viewFactor,
            sinkTemperatureC = radiator.sinkTemperatureC,
            lastRejected = 0,
        }
        host.radiators[#host.radiators + 1] = object.radiators[index]
    end
    -- Coolant loops (#396): a finite, loop-specific closed coolant circuit
    -- pumped through one thermal reservoir. Normal and hazardous coolant
    -- are segregated classes and no process pools them: each loop owns its
    -- own inventory and the only crossing with station thermal state is
    -- heat through the circulation-gated exchanger. Coolant is not
    -- potable or nutrient water and never touches a Resource store. Zero
    -- starting coolant disables the loop: no flow, no pump draw, no
    -- heat — circulation is never invented from an empty inventory.
    for index = 1, #coolantLoops do
        local loop = coolantLoops[index]
        assert(type(loop) == 'table', 'coolant loop descriptor must be a table')
        assert(type(loop.id) == 'string' and #loop.id > 0, 'coolant loop needs an id')
        assert(loop.hazardClass == 'normal' or loop.hazardClass == 'hazardous',
            'coolant loop needs a declared hazard class (normal or hazardous)')
        local host = reservoir_by_id[loop.reservoir]
        assert(host ~= nil, 'coolant loop host thermal reservoir is unknown')
        assert(type(loop.coolantCapacityKg) == 'number'
            and loop.coolantCapacityKg == loop.coolantCapacityKg
            and loop.coolantCapacityKg > 0 and loop.coolantCapacityKg < math.huge,
            'coolant capacity must be finite and positive')
        assert(type(loop.startingCoolantKg) == 'number'
            and loop.startingCoolantKg == loop.startingCoolantKg
            and loop.startingCoolantKg >= 0 and loop.startingCoolantKg < math.huge
            and loop.startingCoolantKg <= loop.coolantCapacityKg,
            'starting coolant must be finite and within the loop capacity')
        assert(type(loop.specificHeatKJPerKgK) == 'number'
            and loop.specificHeatKJPerKgK == loop.specificHeatKJPerKgK
            and loop.specificHeatKJPerKgK > 0 and loop.specificHeatKJPerKgK < math.huge,
            'coolant specific heat must be finite and positive')
        local function checkLoopTemp(field, value)
            assert(type(value) == 'number' and value == value and value < math.huge
                and value > -KELVIN_OFFSET,
                'coolant loop ' .. field .. ' must be finite and above absolute zero')
        end
        checkLoopTemp('starting temperature', loop.startingTemperatureC)
        checkLoopTemp('freeze-protection setpoint', loop.freezeProtectionSetpointC)
        assert(type(loop.pumpPowerKW) == 'number' and loop.pumpPowerKW == loop.pumpPowerKW
            and loop.pumpPowerKW >= 0 and loop.pumpPowerKW < math.huge,
            'pump power must be finite and non-negative')
        assert(type(loop.ratedFlowKgPerS) == 'number'
            and loop.ratedFlowKgPerS == loop.ratedFlowKgPerS
            and loop.ratedFlowKgPerS >= 0 and loop.ratedFlowKgPerS < math.huge,
            'rated flow must be finite and non-negative')
        assert(type(loop.exchangerConductanceKWPerK) == 'number'
            and loop.exchangerConductanceKWPerK == loop.exchangerConductanceKWPerK
            and loop.exchangerConductanceKWPerK >= 0 and loop.exchangerConductanceKWPerK < math.huge,
            'exchanger conductance must be finite and non-negative')
        assert(type(loop.heaterCapacityKW) == 'number'
            and loop.heaterCapacityKW == loop.heaterCapacityKW
            and loop.heaterCapacityKW >= 0 and loop.heaterCapacityKW < math.huge,
            'heater capacity must be finite and non-negative')
        object.coolant_loops[index] = {
            id = loop.id,
            hazardClass = loop.hazardClass,
            reservoir = host,
            capacity = loop.coolantCapacityKg,
            coolant = loop.startingCoolantKg,
            startingCoolant = loop.startingCoolantKg,
            cp = loop.specificHeatKJPerKgK,
            startingTemperature = loop.startingTemperatureC,
            setpoint = loop.freezeProtectionSetpointC,
            pumpKW = loop.pumpPowerKW,
            ratedFlow = loop.ratedFlowKgPerS,
            conductance = loop.exchangerConductanceKWPerK,
            heaterKW = loop.heaterCapacityKW,
            temperature = loop.startingTemperatureC,
            pumpWorkKJ = 0,
            heaterKJ = 0,
            exchangedKJ = 0,
            lastPumpDemand = 0,
            lastPumpSupplied = 0,
            lastHeaterDemand = 0,
            lastHeaterSupplied = 0,
            lastCirculation = 0,
            lastFlow = 0,
            lastExchange = 0,
            belowFreeze = false,
            circulating = false,
            extractedKJ = 0,
            chillerExtraction = 0,
        }
    end
    -- Chillers (#397): a heat pump that supplies a cold service of its
    -- declared thermal service class. The cold-side interface is a coolant
    -- loop: the loop's actual circulation caps the duty the evaporator can
    -- deliver, and the chilled setpoint gates the demand. The hot-side
    -- interface is a thermal reservoir that accepts the condenser heat,
    -- refused whole while the sink sits at or above the maximum condensing
    -- temperature. Work at the configured COP plus running auxiliaries is
    -- drawn from the shared pool; condenser heat equals extracted heat
    -- plus the work and auxiliaries actually supplied, booked once at one
    -- consistent boundary. Both interfaces are class-matched: spare
    -- capacity in another class is never usable, and a chiller that
    -- rejects into the reservoir its own cold side cools is a heater, not
    -- a cooling service.
    local coolant_loop_by_id = {}
    for index = 1, #coolantLoops do
        coolant_loop_by_id[coolantLoops[index].id] = object.coolant_loops[index]
    end
    for index = 1, #chillers do
        local chiller = chillers[index]
        assert(type(chiller) == 'table', 'chiller descriptor must be a table')
        assert(type(chiller.id) == 'string' and #chiller.id > 0, 'chiller needs an id')
        assert(chiller.thermalClass == 'low' or chiller.thermalClass == 'moderate'
            or chiller.thermalClass == 'warm' or chiller.thermalClass == 'generation',
            'chiller needs a declared thermal service class')
        local coldLoop = coolant_loop_by_id[chiller.coldLoop]
        assert(coldLoop ~= nil, 'chiller cold-side coolant loop is unknown')
        local sink = reservoir_by_id[chiller.condenser]
        assert(sink ~= nil, 'chiller condenser sink reservoir is unknown')
        assert(coldLoop.reservoir.thermalClass == chiller.thermalClass,
            'chiller cold loop must serve the same thermal service class')
        assert(sink.thermalClass == chiller.thermalClass,
            'chiller condenser sink must share the chiller thermal service class')
        assert(sink ~= coldLoop.reservoir,
            'a chiller rejecting into the reservoir its cold side cools is a heater, not a cooling service')
        assert(type(chiller.ratedCoolingCapacityKW) == 'number'
            and chiller.ratedCoolingCapacityKW == chiller.ratedCoolingCapacityKW
            and chiller.ratedCoolingCapacityKW >= 0 and chiller.ratedCoolingCapacityKW < math.huge,
            'chiller rated cooling capacity must be finite and non-negative')
        assert(type(chiller.cop) == 'number' and chiller.cop == chiller.cop
            and chiller.cop > 0 and chiller.cop < math.huge,
            'chiller COP must be finite and positive')
        assert(type(chiller.auxiliaryPowerKW) == 'number'
            and chiller.auxiliaryPowerKW == chiller.auxiliaryPowerKW
            and chiller.auxiliaryPowerKW >= 0 and chiller.auxiliaryPowerKW < math.huge,
            'chiller auxiliary power must be finite and non-negative')
        assert(type(chiller.chilledSetpointC) == 'number'
            and chiller.chilledSetpointC == chiller.chilledSetpointC
            and chiller.chilledSetpointC < math.huge
            and chiller.chilledSetpointC > -KELVIN_OFFSET,
            'chiller chilled setpoint must be finite and above absolute zero')
        assert(type(chiller.maxCondensingTemperatureC) == 'number'
            and chiller.maxCondensingTemperatureC == chiller.maxCondensingTemperatureC
            and chiller.maxCondensingTemperatureC < math.huge
            and chiller.maxCondensingTemperatureC > -KELVIN_OFFSET,
            'chiller maximum condensing temperature must be finite and above absolute zero')
        assert(chiller.chilledSetpointC < chiller.maxCondensingTemperatureC,
            'chiller chilled setpoint must be below its maximum condensing temperature')
        object.chillers[index] = {
            id = chiller.id,
            thermalClass = chiller.thermalClass,
            coldLoop = coldLoop,
            condenser = sink,
            ratedKW = chiller.ratedCoolingCapacityKW,
            cop = chiller.cop,
            auxKW = chiller.auxiliaryPowerKW,
            chilledSetpoint = chiller.chilledSetpointC,
            maxCondensing = chiller.maxCondensingTemperatureC,
            lastDemand = 0,
            lastCooling = 0,
            lastDeliverable = 0,
            lastPowerDemand = 0,
            lastPowerSupplied = 0,
            lastCondenserKW = 0,
            lastFraction = 0,
            admitted = false,
            atSetpoint = false,
            sinkOver = false,
            extractedKJ = 0,
            workKJ = 0,
            condenserKJ = 0,
        }
    end
    -- Generators (#399): a fuelled electrical source that converts finite
    -- stored fuel energy into bus electrical output at the declared
    -- efficiency. The fuel inventory is an energy-unit Resource store: the
    -- generator draws qualified fuel above the store's protected reserve,
    -- so fuel is finite, resupplyable and closes through the store's own
    -- boundary ledger. Output is dispatchable within the installed rating
    -- and can only move at the declared ramp limit. Starting cranks at the
    -- startup power for the startup duration and only completes once that
    -- energy has actually been received from the shared pool while cooling
    -- is qualified - a generator never self-starts on output it has not
    -- yet made. After output stops, the configured shutdown heat keeps
    -- flowing into the sink for the configured decay period and the
    -- cooling requirement stays exposed with it. Waste, startup and decay
    -- heat route exactly once into the class-matched sink reservoir. This
    -- models the authored heat and energy contract of a fuelled source;
    -- it claims no reactor physics and no nuclear qualification.
    for index = 1, #generators do
        local generator = generators[index]
        assert(type(generator) == 'table', 'generator descriptor must be a table')
        assert(type(generator.id) == 'string' and #generator.id > 0, 'generator needs an id')
        assert(generator.thermalClass == 'low' or generator.thermalClass == 'moderate'
            or generator.thermalClass == 'warm' or generator.thermalClass == 'generation',
            'generator needs a declared thermal service class')
        local fuelStore = by_id[generator.fuelStore]
        assert(fuelStore ~= nil, 'generator fuel Resource store is unknown')
        assert(fuelStore.unit == 'kJ' or fuelStore.unit == 'MJ'
            or fuelStore.unit == 'Wh' or fuelStore.unit == 'kWh',
            'generator fuel store must be denominated in an energy unit')
        local heatSink = reservoir_by_id[generator.heatSink]
        assert(heatSink ~= nil, 'generator heat sink reservoir is unknown')
        assert(heatSink.thermalClass == generator.thermalClass,
            'generator heat sink must share the generator thermal service class')
        assert(type(generator.ratedKW) == 'number' and generator.ratedKW == generator.ratedKW
            and generator.ratedKW >= 0 and generator.ratedKW < math.huge,
            'generator installed capacity must be finite and non-negative')
        assert(type(generator.efficiency) == 'number' and generator.efficiency == generator.efficiency
            and generator.efficiency > 0 and generator.efficiency <= 1,
            'generator efficiency must be greater than 0 and at most 1')
        assert(type(generator.rampKWPerSecond) == 'number'
            and generator.rampKWPerSecond == generator.rampKWPerSecond
            and generator.rampKWPerSecond > 0 and generator.rampKWPerSecond < math.huge,
            'generator ramp limit must be finite and positive')
        assert(type(generator.commandedKW) == 'number'
            and generator.commandedKW == generator.commandedKW
            and generator.commandedKW >= 0 and generator.commandedKW < math.huge
            and generator.commandedKW <= generator.ratedKW,
            'generator commanded output must be finite and within the installed capacity')
        assert(type(generator.startupPowerKW) == 'number'
            and generator.startupPowerKW == generator.startupPowerKW
            and generator.startupPowerKW >= 0 and generator.startupPowerKW < math.huge,
            'generator startup power must be finite and non-negative')
        assert(type(generator.startupSeconds) == 'number'
            and generator.startupSeconds == generator.startupSeconds
            and generator.startupSeconds >= 0 and generator.startupSeconds < math.huge,
            'generator startup duration must be finite and non-negative')
        assert(type(generator.shutdownHeatKW) == 'number'
            and generator.shutdownHeatKW == generator.shutdownHeatKW
            and generator.shutdownHeatKW >= 0 and generator.shutdownHeatKW < math.huge,
            'generator shutdown heat must be finite and non-negative')
        assert(type(generator.shutdownSeconds) == 'number'
            and generator.shutdownSeconds == generator.shutdownSeconds
            and generator.shutdownSeconds >= 0 and generator.shutdownSeconds < math.huge,
            'generator shutdown heat duration must be finite and non-negative')
        assert(type(generator.maxSinkTemperatureC) == 'number'
            and generator.maxSinkTemperatureC == generator.maxSinkTemperatureC
            and generator.maxSinkTemperatureC < math.huge
            and generator.maxSinkTemperatureC > -KELVIN_OFFSET,
            'generator maximum sink temperature must be finite and above absolute zero')
        object.generators[index] = {
            id = generator.id,
            fuelStore = fuelStore,
            thermalClass = generator.thermalClass,
            heatSink = heatSink,
            ratedKW = generator.ratedKW,
            efficiency = generator.efficiency,
            rampKWPerSecond = generator.rampKWPerSecond,
            commandedKW = generator.commandedKW,
            startupPowerKW = generator.startupPowerKW,
            startupSeconds = generator.startupSeconds,
            shutdownHeatKW = generator.shutdownHeatKW,
            shutdownSeconds = generator.shutdownSeconds,
            maxSinkTemperatureC = generator.maxSinkTemperatureC,
            phase = 'stopped',
            outputKW = 0,
            lastFuelDrawKWh = 0,
            lastWasteHeatKW = 0,
            lastStartupDrawKW = 0,
            lastStartupUnmetKW = 0,
            lastDecayHeatKW = 0,
            startupReceivedKJ = 0,
            shutdownElapsed = 0,
            lastCoolingAvailable = false,
            lastCoolingRequired = false,
            blockReason = '',
            fuelEnergyKJ = 0,
            electricalKJ = 0,
            wasteHeatKJ = 0,
            startupHeatKJ = 0,
            decayHeatKJ = 0,
        }
    end
    for index = 1, #loads do
        local load = loads[index]
        assert(type(load) == 'table', 'equipment load descriptor must be a table')
        assert(type(load.id) == 'string' and #load.id > 0, 'equipment load needs an id')
        assert(type(load.demandKW) == 'number' and load.demandKW == load.demandKW
            and load.demandKW >= 0 and load.demandKW < math.huge,
            'equipment load demand must be finite and non-negative')
        assert(type(load.heatKW) == 'number' and load.heatKW == load.heatKW
            and load.heatKW >= 0 and load.heatKW < math.huge,
            'equipment load heat output must be finite and non-negative')
        assert(load.thermalClass == 'low' or load.thermalClass == 'moderate'
            or load.thermalClass == 'warm' or load.thermalClass == 'generation',
            'equipment load needs a declared thermal service class')
        assert(load.cooling == 'exported' or load.cooling == 'coolingService',
            'equipment load needs a declared cooling disposition')
        assert(type(load.criticalFloorKW) == 'number'
            and load.criticalFloorKW == load.criticalFloorKW
            and load.criticalFloorKW >= 0 and load.criticalFloorKW <= load.demandKW,
            'equipment load critical floor must be finite and within the demand')
        assert(type(load.priority) == 'number' and load.priority == load.priority
            and load.priority == math.tointeger(load.priority),
            'equipment load priority must be a finite integer')
        assert(type(load.maxCurtailment) == 'number'
            and load.maxCurtailment == load.maxCurtailment
            and load.maxCurtailment >= 0 and load.maxCurtailment <= 1,
            'equipment load allowed curtailment must be finite and from 0 to 1')
        checkProfile(load.profile, 'equipment load ' .. load.id .. ' operating profile')
        local exchanges = {}
        for flowIndex = 1, #(load.exchanges or {}) do
            local exchange = load.exchanges[flowIndex]
            local store = by_id[exchange.store]
            assert(store ~= nil, 'equipment load exchange store is unknown')
            assert(type(exchange.rate) == 'number' and exchange.rate == exchange.rate
                and exchange.rate > 0 and exchange.rate < math.huge,
                'equipment load exchange rate must be finite and positive')
            exchanges[flowIndex] = {
                store = store,
                rate = exchange.rate,
                consumes = exchange.consumes and true or false,
                lastRequested = 0,
                lastCompleted = 0,
                lastReserved = 0,
                lastUnmet = 0,
            }
        end
        -- Heat sink routing (#395): emitted heat goes to exactly one
        -- declared reservoir of the same thermal service class; spare
        -- capacity in another class is never usable, and a
        -- cooling-service load needs a cooling service rather than a
        -- passive sink.
        local heatSink
        if load.heatSinkId ~= nil and load.heatSinkId ~= '' then
            heatSink = reservoir_by_id[load.heatSinkId]
            assert(heatSink ~= nil, 'equipment load heat sink reservoir is unknown')
            assert(heatSink.thermalClass == load.thermalClass,
                'equipment load heat sink must share the load thermal service class')
            assert(load.cooling == 'exported',
                'a cooling-service load needs a cooling service, not a heat sink')
        end
        object.loads[index] = {
            id = load.id,
            demandKW = load.demandKW,
            profile = readProfile(load.profile),
            heatKW = load.heatKW,
            thermalClass = load.thermalClass,
            cooling = load.cooling,
            criticalFloorKW = load.criticalFloorKW,
            priority = load.priority,
            maxCurtailment = load.maxCurtailment,
            exchanges = exchanges,
            heatSink = heatSink,
            lastLevel = 0,
            requested = 0,
            supplied = 0,
            shortfall = '',
            unmetCritical = 0,
            deliveredFraction = 0,
            materialFraction = 1,
            cappedRequest = 0,
            materialComputed = false,
            materialRefused = false,
            materialReserved = false,
            heatExported = 0,
            unmetCooling = 0,
            admitted = false,
        }
    end
    -- Battery banks: nameplate energy is the installed total, usable
    -- energy the cyclable slice above the derived reserve floor
    -- (nameplate - usable). Stored energy starts at the authored value
    -- and is runtime state; charge and discharge efficiencies are
    -- applied at the bus boundary so losses stay accounted and free
    -- energy is impossible in either direction.
    for index = 1, #batteries do
        local battery = batteries[index]
        assert(type(battery) == 'table', 'battery descriptor must be a table')
        assert(type(battery.id) == 'string' and #battery.id > 0, 'battery needs an id')
        assert(type(battery.nameplateKWh) == 'number'
            and battery.nameplateKWh == battery.nameplateKWh
            and battery.nameplateKWh >= 0 and battery.nameplateKWh < math.huge,
            'battery nameplate energy must be finite and non-negative')
        assert(type(battery.usableKWh) == 'number'
            and battery.usableKWh == battery.usableKWh
            and battery.usableKWh >= 0 and battery.usableKWh <= battery.nameplateKWh,
            'battery usable energy must be finite and within the nameplate energy')
        assert(type(battery.startingStoredKWh) == 'number'
            and battery.startingStoredKWh == battery.startingStoredKWh
            and battery.startingStoredKWh >= 0
            and battery.startingStoredKWh <= battery.nameplateKWh,
            'battery starting stored energy must be finite and within the nameplate energy')
        assert(type(battery.maxChargeKW) == 'number'
            and battery.maxChargeKW == battery.maxChargeKW
            and battery.maxChargeKW >= 0 and battery.maxChargeKW < math.huge,
            'battery charge power limit must be finite and non-negative')
        assert(type(battery.maxDischargeKW) == 'number'
            and battery.maxDischargeKW == battery.maxDischargeKW
            and battery.maxDischargeKW >= 0 and battery.maxDischargeKW < math.huge,
            'battery discharge power limit must be finite and non-negative')
        assert(type(battery.chargeEfficiency) == 'number'
            and battery.chargeEfficiency == battery.chargeEfficiency
            and battery.chargeEfficiency > 0 and battery.chargeEfficiency <= 1,
            'battery charge efficiency must be greater than 0 and at most 1')
        assert(type(battery.dischargeEfficiency) == 'number'
            and battery.dischargeEfficiency == battery.dischargeEfficiency
            and battery.dischargeEfficiency > 0 and battery.dischargeEfficiency <= 1,
            'battery discharge efficiency must be greater than 0 and at most 1')
        object.batteries[index] = {
            id = battery.id,
            nameplateKWh = battery.nameplateKWh,
            usableKWh = battery.usableKWh,
            startingStored = battery.startingStoredKWh,
            maxChargeKW = battery.maxChargeKW,
            maxDischargeKW = battery.maxDischargeKW,
            chargeEfficiency = battery.chargeEfficiency,
            dischargeEfficiency = battery.dischargeEfficiency,
            stored = battery.startingStoredKWh,
            charged = 0,
            discharged = 0,
            delivered = 0,
            charge_input = 0,
            last_charge = 0,
            last_discharge = 0,
        }
    end
    -- Resupply delivery schedules: an authored external arrival of a
    -- declared quantity into one store, once or recurring. The mirror
    -- keeps its own fired count and next-arrival time so it can audit the
    -- same schedule the host runs; accepted material is booked as external
    -- inflow and refused material stays accounted as rejected.
    for index = 1, #deliveries do
        local delivery = deliveries[index]
        assert(type(delivery) == 'table', 'resupply delivery descriptor must be a table')
        assert(type(delivery.id) == 'string' and #delivery.id > 0, 'resupply delivery needs an id')
        local destination = by_id[delivery.store]
        assert(destination ~= nil, 'resupply delivery destination store is unknown')
        assert(delivery.unit == destination.unit,
            'resupply delivery unit must match its destination store unit')
        assert(type(delivery.quantity) == 'number'
            and delivery.quantity == delivery.quantity and delivery.quantity > 0
            and delivery.quantity < math.huge,
            'resupply delivery quantity must be finite and positive')
        assert(type(delivery.firstSeconds) == 'number'
            and delivery.firstSeconds == delivery.firstSeconds
            and delivery.firstSeconds >= 0 and delivery.firstSeconds < math.huge,
            'resupply delivery first occurrence must be finite and non-negative')
        assert(type(delivery.recurrenceSeconds) == 'number'
            and delivery.recurrenceSeconds == delivery.recurrenceSeconds
            and delivery.recurrenceSeconds >= 0 and delivery.recurrenceSeconds < math.huge,
            'resupply delivery recurrence must be finite and non-negative')
        assert(type(delivery.priority) == 'number'
            and delivery.priority == delivery.priority
            and delivery.priority == math.tointeger(delivery.priority),
            'resupply delivery priority must be a finite integer')
        object.deliveries[index] = {
            id = delivery.id,
            store = destination,
            unit = delivery.unit,
            quantity = delivery.quantity,
            firstSeconds = delivery.firstSeconds,
            recurrenceSeconds = delivery.recurrenceSeconds,
            qualified = delivery.qualified and true or false,
            priority = delivery.priority,
            fired = 0,
            lastAccepted = 0,
            lastRejected = 0,
            acceptedTotal = 0,
            rejectedTotal = 0,
            exhausted = false,
        }
    end
    -- Compartment atmospheres (#400): a finite compartment-owned gas
    -- inventory in a fixed volume, integrated on an authored effective
    -- thermal capacity (air plus bonded structure) and optionally
    -- coupled heat-only to one thermal reservoir. The ideal-gas partial
    -- pressure and Magnus saturation formulas mirror the host exactly;
    -- the sandbox re-audits the per-component and thermal conservation
    -- every tick so a corrupt crossing or model drift fails contained
    -- like any other script failure.
    local atmosphere_by_id = {}
    for index = 1, #atmospheres do
        local atmosphere = atmospheres[index]
        assert(type(atmosphere) == 'table', 'atmosphere descriptor must be a table')
        assert(type(atmosphere.id) == 'string' and #atmosphere.id > 0, 'atmosphere needs an id')
        assert(atmosphere.hazardClass == 'normal' or atmosphere.hazardClass == 'hazardous',
            'atmosphere needs a declared hazard class (normal or hazardous)')
        local function checkAmount(field, value, positive)
            assert(type(value) == 'number' and value == value and value < math.huge
                and (positive and value > 0 or value >= 0),
                'atmosphere ' .. field .. ' must be finite and '
                    .. (positive and 'positive' or 'non-negative'))
        end
        checkAmount('volume', atmosphere.volumeM3, true)
        checkAmount('starting oxygen', atmosphere.startingOxygenKg, false)
        checkAmount('starting nitrogen', atmosphere.startingNitrogenKg, false)
        checkAmount('starting carbon dioxide', atmosphere.startingCarbonDioxideKg, false)
        checkAmount('starting water vapour', atmosphere.startingWaterVapourKg, false)
        assert(type(atmosphere.startingTemperatureC) == 'number'
            and atmosphere.startingTemperatureC == atmosphere.startingTemperatureC
            and atmosphere.startingTemperatureC > -KELVIN_OFFSET
            and atmosphere.startingTemperatureC < math.huge,
            'atmosphere starting temperature must be finite and above absolute zero')
        checkAmount('thermal capacity', atmosphere.thermalCapacityKJPerK, true)
        local reservoir
        if atmosphere.reservoir ~= nil and atmosphere.reservoir ~= '' then
            reservoir = reservoir_by_id[atmosphere.reservoir]
            assert(reservoir ~= nil, 'atmosphere host thermal reservoir is unknown')
        end
        checkAmount('exchanger conductance', atmosphere.exchangerConductanceKWPerK, false)
        assert(type(atmosphere.occupants) == 'number'
            and atmosphere.occupants == math.tointeger(atmosphere.occupants)
            and atmosphere.occupants >= 0,
            'atmosphere occupants must be a non-negative integer')
        checkAmount('oxygen use per occupant', atmosphere.oxygenUsePerOccupantKgPerS, false)
        checkAmount('CO2 production per occupant', atmosphere.co2ProductionPerOccupantKgPerS, false)
        checkAmount('sensible heat per occupant', atmosphere.sensibleHeatPerOccupantKW, false)
        checkAmount('moisture addition per occupant', atmosphere.moistureAdditionPerOccupantKgPerS, false)
        checkAmount('leak rate', atmosphere.leakRateKgPerS, false)
        assert(type(atmosphere.minPressureKPa) == 'number'
            and type(atmosphere.maxPressureKPa) == 'number'
            and atmosphere.minPressureKPa == atmosphere.minPressureKPa
            and atmosphere.maxPressureKPa == atmosphere.maxPressureKPa
            and atmosphere.minPressureKPa > 0 and atmosphere.minPressureKPa < math.huge
            and atmosphere.maxPressureKPa < math.huge
            and atmosphere.minPressureKPa < atmosphere.maxPressureKPa,
            'atmosphere pressure limits must be finite and positive with min below max')
        checkAmount('minimum oxygen partial pressure', atmosphere.minOxygenPartialPressureKPa, false)
        checkAmount('maximum CO2 partial pressure', atmosphere.maxCarbonDioxidePartialPressureKPa, false)
        object.atmospheres[index] = {
            id = atmosphere.id,
            hazardClass = atmosphere.hazardClass,
            volumeM3 = atmosphere.volumeM3,
            oxygen = atmosphere.startingOxygenKg,
            nitrogen = atmosphere.startingNitrogenKg,
            carbonDioxide = atmosphere.startingCarbonDioxideKg,
            waterVapour = atmosphere.startingWaterVapourKg,
            startingOxygen = atmosphere.startingOxygenKg,
            startingNitrogen = atmosphere.startingNitrogenKg,
            startingCarbonDioxide = atmosphere.startingCarbonDioxideKg,
            startingWaterVapour = atmosphere.startingWaterVapourKg,
            startingTemperature = atmosphere.startingTemperatureC,
            temperature = atmosphere.startingTemperatureC,
            capacity = atmosphere.thermalCapacityKJPerK,
            reservoir = reservoir,
            conductance = atmosphere.exchangerConductanceKWPerK,
            occupants = atmosphere.occupants,
            oxygenUse = atmosphere.oxygenUsePerOccupantKgPerS,
            co2Production = atmosphere.co2ProductionPerOccupantKgPerS,
            sensibleHeat = atmosphere.sensibleHeatPerOccupantKW,
            moistureAddition = atmosphere.moistureAdditionPerOccupantKgPerS,
            leakRate = atmosphere.leakRateKgPerS,
            minPressure = atmosphere.minPressureKPa,
            maxPressure = atmosphere.maxPressureKPa,
            minOxygenPartial = atmosphere.minOxygenPartialPressureKPa,
            maxCo2Partial = atmosphere.maxCarbonDioxidePartialPressureKPa,
            leakOxygen = 0, leakNitrogen = 0,
            leakCarbonDioxide = 0, leakWaterVapour = 0,
            cycleOxygen = 0, cycleNitrogen = 0,
            cycleCarbonDioxide = 0, cycleWaterVapour = 0,
            oxygenConsumed = 0, co2Produced = 0,
            vapourAdded = 0, condensed = 0,
            leaked = 0, recovered = 0, lost = 0,
            occupantHeatKJ = 0, latentHeatKJ = 0, exchangedKJ = 0,
        }
        atmosphere_by_id[atmosphere.id] = object.atmospheres[index]
    end
    -- Scheduled airlock cycles (#400): scenario events on the same
    -- deterministic schedule rule as resupply deliveries. Each
    -- occurrence removes the authored mass capped by what the
    -- compartment holds, split composition-proportionally into a
    -- recovered and a lost share.
    for index = 1, #airlockCycles do
        local cycle = airlockCycles[index]
        assert(type(cycle) == 'table', 'airlock cycle descriptor must be a table')
        assert(type(cycle.id) == 'string' and #cycle.id > 0, 'airlock cycle needs an id')
        local atmosphere = atmosphere_by_id[cycle.atmosphere]
        assert(atmosphere ~= nil, 'airlock cycle atmosphere compartment is unknown')
        assert(type(cycle.gasMassKg) == 'number' and cycle.gasMassKg == cycle.gasMassKg
            and cycle.gasMassKg > 0 and cycle.gasMassKg < math.huge,
            'airlock cycled gas mass must be finite and positive')
        assert(type(cycle.firstSeconds) == 'number'
            and cycle.firstSeconds == cycle.firstSeconds
            and cycle.firstSeconds >= 0 and cycle.firstSeconds < math.huge,
            'airlock first occurrence must be finite and non-negative')
        assert(type(cycle.recurrenceSeconds) == 'number'
            and cycle.recurrenceSeconds == cycle.recurrenceSeconds
            and cycle.recurrenceSeconds >= 0 and cycle.recurrenceSeconds < math.huge,
            'airlock recurrence must be finite and non-negative')
        assert(type(cycle.recoveredFraction) == 'number'
            and cycle.recoveredFraction == cycle.recoveredFraction
            and cycle.recoveredFraction >= 0 and cycle.recoveredFraction <= 1,
            'airlock recovered fraction must be finite and from 0 to 1')
        assert(type(cycle.priority) == 'number' and cycle.priority == cycle.priority
            and cycle.priority == math.tointeger(cycle.priority),
            'airlock priority must be a finite integer')
        object.airlock_cycles[index] = {
            id = cycle.id,
            atmosphere = atmosphere,
            gasMass = cycle.gasMassKg,
            firstSeconds = cycle.firstSeconds,
            recurrenceSeconds = cycle.recurrenceSeconds,
            recoveredFraction = cycle.recoveredFraction,
            priority = cycle.priority,
            fired = 0,
            removedTotal = 0,
            recoveredTotal = 0,
            lostTotal = 0,
            exhausted = false,
        }
    end
    -- Declared deterministic order: priority descending, declaration index
    -- ascending. Including the index makes the comparator a total order, so
    -- the result cannot depend on sort stability.
    for index = 1, #object.transfers do object.order[index] = index end
    table.sort(object.order, function(left, right)
        local a, b = object.transfers[left], object.transfers[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    for index = 1, #object.conversions do object.conversion_order[index] = index end
    table.sort(object.conversion_order, function(left, right)
        local a, b = object.conversions[left], object.conversions[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    for index = 1, #object.loads do object.load_order[index] = index end
    table.sort(object.load_order, function(left, right)
        local a, b = object.loads[left], object.loads[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    local function evaluateProfile(profile, elapsed)
        if profile.periodSeconds <= 0 then return profile.constantLevel end
        local phase = elapsed % profile.periodSeconds
        if phase < 0 then phase = phase + profile.periodSeconds end
        local level = profile.segments[1].level
        for _, segment in ipairs(profile.segments) do
            if segment.startSeconds > phase then break end
            level = segment.level
        end
        return level
    end
    local function checkFiniteLimits(self)
        -- #556: the finite-limit audit honors the same documented
        -- numerical tolerance as the conservation residuals, so a
        -- one-ulp roundoff is never a software failure, while any
        -- genuine negative or over-capacity breach still fails.
        for index = 1, #self.stores do
            local store = self.stores[index]
            local bound = BALANCE_TOLERANCE * math.max(1.0, store.capacity)
            assert(store.qualified >= -bound and store.unqualified >= -bound,
                "resource store inventory went negative")
            assert(store.qualified + store.unqualified <= store.capacity + bound,
                "resource store inventory exceeded capacity")
        end
    end
    local function checkConservation(self)
        local totals = {}
        for index = 1, #self.stores do
            local store = self.stores[index]
            totals[store.unit] = (totals[store.unit] or 0) + store.qualified + store.unqualified
        end
        for unit, starting in pairs(self.starting_totals) do
            -- Accepted deliveries are external inflow on the same boundary
            -- as the declared losses: current + lost - delivered must equal
            -- starting, so arriving material is accounted, never implicit.
            local residual = (totals[unit] or 0)
                + (self.unit_loss[unit] or 0)
                - (self.unit_delivered[unit] or 0) - starting
            local bound = BALANCE_TOLERANCE * math.max(1.0, math.abs(starting))
            assert(residual == residual and math.abs(residual) <= bound,
                "conservation residual for unit '" .. unit .. "' exceeds tolerance")
        end
    end
    local function checkConstituentConservation(self)
        local totals = {}
        for index = 1, #self.stores do
            local store = self.stores[index]
            if store.composition then
                addConstituents(totals, store.composition,
                    toKg(store.unit, store.qualified + store.unqualified), 1)
            end
        end
        for constituent, starting in pairs(self.starting_constituents) do
            local residual = (totals[constituent] or 0)
                + (self.constituent_loss[constituent] or 0)
                - (self.constituent_delivered[constituent] or 0) - starting
            local bound = BALANCE_TOLERANCE * math.max(1.0, math.abs(starting))
            assert(residual == residual and math.abs(residual) <= bound,
                "constituent conservation residual for '" .. constituent .. "' exceeds tolerance")
        end
    end
    -- Occurrences of a schedule due at or before `elapsed`. Monotonic in
    -- elapsed time, so firing `due - fired` each tick runs every
    -- occurrence exactly once however many ticks an accelerated update
    -- batches together. Mirrors core::stationScheduleOccurrencesDue.
    local function occurrencesDue(firstSeconds, recurrenceSeconds, elapsed)
        if elapsed + SCHEDULE_TOLERANCE < firstSeconds then return 0 end
        if recurrenceSeconds <= 0 then return 1 end
        local steps = math.floor((elapsed + SCHEDULE_TOLERANCE - firstSeconds)
            / recurrenceSeconds)
        if not (steps > 0) then return 1 end
        return steps + 1
    end
    -- Mirror of the host delivery ledger: every occurrence due since the
    -- last tick is collected and applied in the declared scenario order
    -- (occurrence time ascending, priority descending, declaration order),
    -- so coincident arrivals never depend on iteration order. The
    -- destination accepts only what fits its total capacity; the refused
    -- remainder stays accounted as rejected material and never enters the
    -- station's own inventory (#392).
    local function applyDeliveries(self, elapsed)
        local arrivals = {}
        for index = 1, #self.deliveries do
            local delivery = self.deliveries[index]
            if not delivery.exhausted then
                local due = occurrencesDue(delivery.firstSeconds,
                    delivery.recurrenceSeconds, elapsed)
                for occurrence = delivery.fired + 1, due do
                    local when = delivery.recurrenceSeconds > 0
                        and delivery.firstSeconds + (occurrence - 1) * delivery.recurrenceSeconds
                        or delivery.firstSeconds
                    arrivals[#arrivals + 1] = {
                        when = when,
                        priority = delivery.priority,
                        declaration = index,
                        occurrence = occurrence,
                    }
                end
            end
        end
        table.sort(arrivals, function(left, right)
            if left.when ~= right.when then return left.when < right.when end
            if left.priority ~= right.priority then return left.priority > right.priority end
            return left.declaration < right.declaration
        end)
        for _, arrival in ipairs(arrivals) do
            local delivery = self.deliveries[arrival.declaration]
            local store = delivery.store
            local headroom = math.max(0.0,
                store.capacity - (store.qualified + store.unqualified))
            local accepted = math.min(delivery.quantity, headroom)
            local rejected = delivery.quantity - accepted
            assert(accepted >= 0 and rejected >= 0
                and accepted == accepted and rejected == rejected,
                "delivery accepted/rejected amounts are not finite and non-negative")
            if delivery.qualified then
                store.qualified = store.qualified + accepted
            else
                store.unqualified = store.unqualified + accepted
            end
            self.unit_delivered[store.unit] =
                (self.unit_delivered[store.unit] or 0) + accepted
            if store.composition then
                addConstituents(self.constituent_delivered, store.composition,
                    toKg(store.unit, accepted), 1)
            end
            delivery.fired = arrival.occurrence
            delivery.lastAccepted = accepted
            delivery.lastRejected = rejected
            delivery.acceptedTotal = delivery.acceptedTotal + accepted
            delivery.rejectedTotal = delivery.rejectedTotal + rejected
            if delivery.recurrenceSeconds <= 0 then delivery.exhausted = true end
        end
    end
    -- Mirror of the host ledger: same deterministic order, same limits.
    -- Unmet material never moves and is never deleted; shortages are valid
    -- outcomes, while a conservation violation is a software failure.
    -- Compartment atmospheres (#400): scheduled airlock cycles run first
    -- against the tick-start inventory in the shared scenario schedule
    -- order (occurrence time, priority descending, declaration order),
    -- then each compartment runs its occupancy draw, vapour constraint,
    -- leakage and thermal integration in the documented order. The
    -- ideal-gas partial pressure and Magnus saturation formulas mirror
    -- the host exactly; the reservoir books the exact negation of the
    -- compartment exchange in applyThermal, so the crossing is one
    -- exchange accounted once on each side.
    local function partialPressureKPa(massKg, molarMassKgPerMol, volumeM3, temperatureC)
        return massKg / molarMassKgPerMol * GAS_CONSTANT * (temperatureC + KELVIN_OFFSET)
            / volumeM3 / 1000.0
    end
    local function saturationVapourPressureKPa(temperatureC)
        return 0.61094 * math.exp(17.625 * temperatureC / (temperatureC + 243.04))
    end
    local function saturatedVapourMassKg(saturationKPa, volumeM3, temperatureC)
        return saturationKPa * 1000.0 * volumeM3 * MOLAR_MASS_WATER
            / (GAS_CONSTANT * (temperatureC + KELVIN_OFFSET))
    end
    local function applyAtmospheres(self, elapsed, fixed_seconds)
        local cycles = {}
        for index = 1, #self.airlock_cycles do
            local cycle = self.airlock_cycles[index]
            if not cycle.exhausted then
                local due = occurrencesDue(cycle.firstSeconds,
                    cycle.recurrenceSeconds, elapsed)
                for occurrence = cycle.fired + 1, due do
                    local when = cycle.recurrenceSeconds > 0
                        and cycle.firstSeconds + (occurrence - 1) * cycle.recurrenceSeconds
                        or cycle.firstSeconds
                    cycles[#cycles + 1] = {
                        when = when,
                        priority = cycle.priority,
                        declaration = index,
                        occurrence = occurrence,
                    }
                end
            end
        end
        table.sort(cycles, function(left, right)
            if left.when ~= right.when then return left.when < right.when end
            if left.priority ~= right.priority then return left.priority > right.priority end
            return left.declaration < right.declaration
        end)
        for _, due in ipairs(cycles) do
            local cycle = self.airlock_cycles[due.declaration]
            local compartment = cycle.atmosphere
            -- The cycle can only remove the gas the compartment
            -- actually holds: capped by the current total mass and
            -- split composition-proportionally, never inventing mass
            -- and never picking a favourite species. What it removes is
            -- booked whole: the recovered fraction into the receiver,
            -- the remainder as lost through the outer hatch.
            local totalMass = compartment.oxygen + compartment.nitrogen
                + compartment.carbonDioxide + compartment.waterVapour
            local removed = math.min(cycle.gasMass, totalMass)
            local removedOxygen = totalMass > 0
                and removed * compartment.oxygen / totalMass or 0
            local removedNitrogen = totalMass > 0
                and removed * compartment.nitrogen / totalMass or 0
            local removedCarbonDioxide = totalMass > 0
                and removed * compartment.carbonDioxide / totalMass or 0
            local removedWaterVapour = totalMass > 0
                and removed * compartment.waterVapour / totalMass or 0
            local recovered = removed * cycle.recoveredFraction
            local lost = removed - recovered
            compartment.oxygen = compartment.oxygen - removedOxygen
            compartment.nitrogen = compartment.nitrogen - removedNitrogen
            compartment.carbonDioxide = compartment.carbonDioxide - removedCarbonDioxide
            compartment.waterVapour = compartment.waterVapour - removedWaterVapour
            compartment.cycleOxygen = compartment.cycleOxygen + removedOxygen
            compartment.cycleNitrogen = compartment.cycleNitrogen + removedNitrogen
            compartment.cycleCarbonDioxide = compartment.cycleCarbonDioxide + removedCarbonDioxide
            compartment.cycleWaterVapour = compartment.cycleWaterVapour + removedWaterVapour
            compartment.recovered = compartment.recovered + recovered
            compartment.lost = compartment.lost + lost
            cycle.fired = due.occurrence
            cycle.removedTotal = cycle.removedTotal + removed
            cycle.recoveredTotal = cycle.recoveredTotal + recovered
            cycle.lostTotal = cycle.lostTotal + lost
            if cycle.recurrenceSeconds <= 0 then cycle.exhausted = true end
        end
        for _, compartment in ipairs(self.atmospheres) do
            local occupants = compartment.occupants
            -- Occupancy is an authored load assignment: the oxygen draw
            -- is capped by the oxygen actually present, and CO2
            -- production plus metabolic heat scale with the fraction
            -- of the demand actually consumed.
            local oxygenDemand = occupants * compartment.oxygenUse * fixed_seconds
            local oxygenConsumed = math.min(oxygenDemand, compartment.oxygen)
            local metabolicFraction = oxygenDemand > 0
                and oxygenConsumed / oxygenDemand or 1
            local co2Produced = occupants * compartment.co2Production
                * fixed_seconds * metabolicFraction
            local vapourAdded = occupants * compartment.moistureAddition * fixed_seconds
            compartment.oxygen = compartment.oxygen - oxygenConsumed
            compartment.oxygenConsumed = compartment.oxygenConsumed + oxygenConsumed
            compartment.carbonDioxide = compartment.carbonDioxide + co2Produced
            compartment.co2Produced = compartment.co2Produced + co2Produced
            compartment.waterVapour = compartment.waterVapour + vapourAdded
            compartment.vapourAdded = compartment.vapourAdded + vapourAdded
            -- Vapour constraint at the current temperature: the excess
            -- over saturation condenses now and releases its latent
            -- heat into the compartment this tick; the condensate
            -- leaves the gas phase and is reported, never destroyed.
            local saturation = saturationVapourPressureKPa(compartment.temperature)
            local saturatedMass = saturatedVapourMassKg(saturation,
                compartment.volumeM3, compartment.temperature)
            local condensed = 0
            if compartment.waterVapour > saturatedMass then
                condensed = compartment.waterVapour - saturatedMass
                compartment.waterVapour = saturatedMass
                compartment.condensed = compartment.condensed + condensed
            end
            -- Leakage: a constant authored total-mass rate capped by
            -- what the compartment holds, split composition-
            -- proportionally. Never routed into another compartment.
            local totalMass = compartment.oxygen + compartment.nitrogen
                + compartment.carbonDioxide + compartment.waterVapour
            local leaked = math.min(compartment.leakRate * fixed_seconds, totalMass)
            if leaked > 0 and totalMass > 0 then
                local leakedOxygen = leaked * compartment.oxygen / totalMass
                local leakedNitrogen = leaked * compartment.nitrogen / totalMass
                local leakedCarbonDioxide = leaked * compartment.carbonDioxide / totalMass
                local leakedWaterVapour = leaked * compartment.waterVapour / totalMass
                compartment.oxygen = compartment.oxygen - leakedOxygen
                compartment.nitrogen = compartment.nitrogen - leakedNitrogen
                compartment.carbonDioxide = compartment.carbonDioxide - leakedCarbonDioxide
                compartment.waterVapour = compartment.waterVapour - leakedWaterVapour
                compartment.leakOxygen = compartment.leakOxygen + leakedOxygen
                compartment.leakNitrogen = compartment.leakNitrogen + leakedNitrogen
                compartment.leakCarbonDioxide = compartment.leakCarbonDioxide + leakedCarbonDioxide
                compartment.leakWaterVapour = compartment.leakWaterVapour + leakedWaterVapour
            end
            compartment.leaked = compartment.leaked + leaked
            -- Thermal integration on the authored effective capacity:
            -- occupant sensible heat, condensation latent heat and the
            -- heat-only reservoir exchange. The exchange came from the
            -- reservoir's implicit star solve in applyPower, which
            -- already paired it with the reservoir's own negation, so it
            -- is only read here (#559).
            local occupantHeat = occupants * compartment.sensibleHeat * metabolicFraction
            local latentHeat = condensed * LATENT_HEAT_VAPORIZATION / fixed_seconds
            local exchange = compartment.pendingExchangeKW or 0
            if compartment.reservoir then
                compartment.reservoir.atmosphereHeat =
                    (compartment.reservoir.atmosphereHeat or 0) - exchange
            end
            compartment.temperature = compartment.temperature
                + (occupantHeat + latentHeat + exchange) * fixed_seconds / compartment.capacity
            compartment.occupantHeatKJ = compartment.occupantHeatKJ + occupantHeat * fixed_seconds
            compartment.latentHeatKJ = compartment.latentHeatKJ + latentHeat * fixed_seconds
            compartment.exchangedKJ = compartment.exchangedKJ + exchange * fixed_seconds
        end
    end
    local function applyTransfers(self, fixed_seconds)
        for _, index in ipairs(self.order) do
            local transfer = self.transfers[index]
            local requested = transfer.rate * fixed_seconds
            local drawable = math.max(0.0, transfer.source.qualified - transfer.source.protected_reserve)
            local headroom = transfer.destination.capacity
                - (transfer.destination.qualified + transfer.destination.unqualified)
            local completed = math.min(requested, drawable, headroom)
            assert(completed >= 0 and completed == completed
                and completed <= requested and completed <= drawable and completed <= headroom,
                "transfer completed amount violates its bounds")
            transfer.source.qualified = transfer.source.qualified - completed
            transfer.destination.qualified = transfer.destination.qualified + completed
        end
    end
    -- Mirror of the host conversion ledger: one common completed scale per
    -- conversion per tick, bounded by drawable input inventory above
    -- protected reserves and output store headroom. The difference between
    -- what the inputs gave up and what the outputs carried away is booked
    -- as the declared conversion loss, never silently dropped.
    local function applyConversions(self, fixed_seconds)
        for _, index in ipairs(self.conversion_order) do
            local conversion = self.conversions[index]
            local scale = 1.0
            for _, flow in ipairs(conversion.inputs) do
                local requested = flow.rate * fixed_seconds
                local drawable = math.max(0.0, flow.store.qualified - flow.store.protected_reserve)
                scale = math.min(scale, drawable / requested)
            end
            for _, flow in ipairs(conversion.outputs) do
                local produced = flow.rate * fixed_seconds
                local headroom = flow.store.capacity
                    - (flow.store.qualified + flow.store.unqualified)
                scale = math.min(scale, headroom / produced)
            end
            if scale > 0 then
                local net = {}
                for _, flow in ipairs(conversion.inputs) do
                    local requested = flow.rate * fixed_seconds
                    -- #556: clamp each flow to its own drawable/headroom:
                    -- requested * (limit / requested) can round one ulp
                    -- above the limit under the fixed float timestep, so
                    -- the binding flow completes exactly at its limit.
                    -- Mirrors the host ledger.
                    local drawable = math.max(0.0, flow.store.qualified - flow.store.protected_reserve)
                    local completed = math.min(requested * scale, drawable)
                    flow.store.qualified = flow.store.qualified - completed
                    flow.last_completed = completed
                    addConstituents(net, flow.store.composition,
                        toKg(flow.store.unit, completed), -1)
                end
                for _, flow in ipairs(conversion.outputs) do
                    local requested = flow.rate * fixed_seconds
                    local headroom = flow.store.capacity
                        - (flow.store.qualified + flow.store.unqualified)
                    local completed = math.min(requested * scale, headroom)
                    flow.store.qualified = flow.store.qualified + completed
                    flow.last_completed = completed
                    addConstituents(net, flow.store.composition,
                        toKg(flow.store.unit, completed), 1)
                end
                for constituent, change in pairs(net) do
                    self.constituent_loss[constituent] =
                        (self.constituent_loss[constituent] or 0) - change
                end
                for _, flow in ipairs(conversion.inputs) do
                    self.unit_loss[flow.store.unit] =
                        (self.unit_loss[flow.store.unit] or 0) + flow.last_completed
                end
                for _, flow in ipairs(conversion.outputs) do
                    self.unit_loss[flow.store.unit] =
                        (self.unit_loss[flow.store.unit] or 0) - flow.last_completed
                end
            end
        end
    end
    -- Mirror of the host power ledger: solar output follows installed
    -- capacity and evaluated illumination; allocation runs in two passes
    -- (protected critical floors first, then discretionary demand) in the
    -- declared deterministic order. Battery discharge rides the same pool,
    -- covering what generation could not reach within the reserve floor,
    -- the discharge power limit and the discharge efficiency; charging
    -- takes only the surplus no load took. Allocated power can never
    -- exceed generation plus actual battery output, and no load is
    -- supplied above its request. A load that requires a cooling service
    -- is never admitted in this slice: it draws nothing and its cooling
    -- need stays visible as unmet cooling rather than being silently
    -- absorbed. A critical floor is protected only as far as the load can
    -- actually run: the claim is the larger of the floor and the minimum
    -- operable supply, and a pool that cannot reach that minimum funds
    -- the load nothing at all, leaving its whole floor unmet (#549).
    local function batteryReserve(battery)
        return battery.nameplateKWh - battery.usableKWh
    end
    local function batteryDisabled(battery)
        return battery.usableKWh <= 0
    end
    local function deliverableKW(battery, dtHours)
        if batteryDisabled(battery) then return 0 end
        local drawable = math.max(0, battery.stored - batteryReserve(battery))
        -- The discharge power limit is per tick: whatever this battery
        -- already delivered this tick counts against it, so the two
        -- allocation passes can never jointly exceed the cap.
        local byPower = battery.maxDischargeKW - battery.last_discharge
        return math.max(0, math.min(byPower,
            drawable * battery.dischargeEfficiency / dtHours))
    end
    -- What the whole bank could deliver this tick before any of it is
    -- routed: the pool a protected critical claim is measured
    -- against (#549).
    local function batteryDeliverableKW(self, dtHours)
        local total = 0
        for _, battery in ipairs(self.batteries) do
            total = total + deliverableKW(battery, dtHours)
        end
        return total
    end
    -- Route a power need through the batteries in declaration order and
    -- return what they actually delivered. dtHours is the crossing
    -- duration in HOURS: kW x hours = kWh, never kW x seconds (#547).
    local function drawBatteries(self, needKW, dtHours)
        if needKW <= 0 then return 0 end
        local remaining = needKW
        for _, battery in ipairs(self.batteries) do
            if remaining <= 1e-15 then break end
            if not batteryDisabled(battery) then
                local cap = deliverableKW(battery, dtHours)
                local give = math.min(remaining, cap)
                if give > 0 then
                    local drawn = give * dtHours / battery.dischargeEfficiency
                    battery.stored = battery.stored - drawn
                    battery.discharged = battery.discharged + drawn
                    battery.delivered = battery.delivered + give * dtHours
                    battery.last_discharge = battery.last_discharge + give
                end
                remaining = remaining - give
            end
        end
        return needKW - remaining
    end
    -- #554: declared material exchanges are operation dependencies.
    -- The material-feasible operating fraction is the largest fraction of
    -- the load's full-operation (profile-scaled) exchanges that its
    -- declared feed (qualified above the protected reserve) and
    -- destination headroom can sustain this tick; the second return value
    -- names the binding limiter. The electrical request is capped to that
    -- fraction before allocation, so a starved or backed-up load never
    -- draws power it cannot run on, and the shortfall stays explicit as
    -- unmet service and per-exchange unmet material.
    local function materialFeasible(load, fixed_seconds)
        local fraction = 1.0
        local limiter = ''
        for _, exchange in ipairs(load.exchanges) do
            local store = exchange.store
            local full = exchange.rate * fixed_seconds * load.lastLevel
            if full > 0 then
                if exchange.consumes then
                    local drawable = math.max(0, store.qualified - store.protected_reserve)
                    if drawable / full < fraction then
                        fraction = drawable / full
                        if drawable <= 1e-12 then
                            if store.protected_reserve > 0 then
                                limiter = "feed store '" .. store.id
                                    .. "' is at its protected reserve"
                            elseif store.unqualified > 1e-12 then
                                limiter = "feed store '" .. store.id
                                    .. "' holds only unqualified material"
                            else
                                limiter = "feed store '" .. store.id .. "' is empty"
                            end
                        else
                            limiter = "feed store '" .. store.id
                                .. "' supply limits the operating level"
                        end
                    end
                else
                    local headroom = math.max(0,
                        store.capacity - (store.qualified + store.unqualified))
                    if headroom / full < fraction then
                        fraction = headroom / full
                        if headroom <= 1e-12 then
                            limiter = "destination store '" .. store.id .. "' is full"
                        else
                            limiter = "destination store '" .. store.id
                                .. "' headroom limits the operating level"
                        end
                    end
                end
            end
        end
        return math.max(0.0, math.min(1.0, fraction)), limiter
    end
    -- Reserve the capped level's material against the live inventory when
    -- a grant is made: a feed draw and a destination claim that later
    -- loads in the allocation order must see, so competing loads never
    -- double-book the same kilograms (#554).
    local function reserveMaterial(load, fraction, fixed_seconds)
        for _, exchange in ipairs(load.exchanges) do
            local reserved = exchange.rate * fixed_seconds * load.lastLevel * fraction
            if exchange.consumes then
                exchange.store.qualified = exchange.store.qualified - reserved
            else
                exchange.store.qualified = exchange.store.qualified + reserved
            end
            exchange.lastReserved = reserved
        end
        load.materialReserved = true
    end
    local function applyPower(self, elapsed, fixed_seconds)
        -- Energy crossings use hours: a fixed tick is a fraction of a
        -- second, so its duration must be divided by 3600 before it
        -- multiplies or divides a kW value into kWh (#547).
        local fixed_hours = fixed_seconds / 3600
        local generation = 0
        for _, array in ipairs(self.solar) do
            local illumination = evaluateProfile(array.illumination, elapsed)
            local available = array.capacityKW > 0 and array.capacityKW * illumination or 0
            array.lastIllumination = illumination
            array.lastAvailable = available
            generation = generation + available
            -- #553: finite installed capacities do not guarantee a
            -- finite total; reject an overflowed generation pool before
            -- it drives any allocation, mirroring the host ledger.
            assert(isFinite(generation),
                'solar generation sum overflows to a non-finite magnitude')
        end
        for _, battery in ipairs(self.batteries) do
            battery.last_charge = 0
            battery.last_discharge = 0
        end
        local batterySupplied = 0
        -- #561: the cranking slice taken out of the generation pool is
        -- tracked here so the audit baseline below is the pool as it was
        -- originally available - solar plus generator output - rather
        -- than the pool already eaten into by the very crank it audits.
        local startupFromGeneration = 0
        -- Fuelled generators run ahead of the load allocation (#399):
        -- their electrical output joins the shared generation pool for
        -- this tick's loads, their startup cranking draws ride the same
        -- pool ahead of discretionary demand, and their waste, startup
        -- and decay heat land in the thermal integration below - each
        -- booked exactly once at the sink boundary. Output is bounded by
        -- the installed rating, the ramp limit and the fuel actually
        -- drawable, and the machine only runs once the startup energy
        -- has been received while cooling is qualified. Mirrors the host
        -- ledger exactly.
        for _, gen in ipairs(self.generators) do
            local disabled = gen.ratedKW <= 0
            local commanded = disabled and 0 or gen.commandedKW
            local sinkTemperature = gen.heatSink.temperature
            -- Cooling is qualified while the sink is below the maximum
            -- sink temperature: at or above it the interface cannot
            -- accept the generator's heat, so the generator cannot run.
            local coolingAvailable = not disabled
                and sinkTemperature + 1e-9 < gen.maxSinkTemperatureC
            local kwhPerUnit = energyUnitsToKWh(gen.fuelStore.unit)
            local drawableKWh = math.max(0, gen.fuelStore.qualified
                - gen.fuelStore.protected_reserve) * kwhPerUnit
            local startupRequiredKJ = gen.startupPowerKW * gen.startupSeconds
            gen.lastFuelDrawKWh = 0
            gen.lastWasteHeatKW = 0
            gen.lastStartupDrawKW = 0
            gen.lastStartupUnmetKW = 0
            gen.lastDecayHeatKW = 0
            gen.lastCoolingAvailable = coolingAvailable
            gen.blockReason = ''
            -- The phase as it stood before this tick's transitions, so
            -- the decay bookkeeping below can tell a generator that has
            -- been in Shutdown since last tick from one that just
            -- entered it (#563).
            local phaseBefore = gen.phase
            if gen.phase == 'stopped' then
                gen.outputKW = 0
                if commanded > 0 then
                    -- A commanded start proceeds only when every
                    -- precondition holds this tick: qualified cooling,
                    -- drawable fuel above the protected reserve, and a
                    -- pool that can actually supply the cranking draw.
                    -- A start is never self-funded: a stopped generator
                    -- produces nothing, so its own output can never
                    -- appear in the pool that starts it.
                    if not coolingAvailable then
                        gen.blockReason = 'cooling is not qualified: sink at or above the maximum sink temperature'
                    elseif drawableKWh <= 0 then
                        gen.blockReason = 'no fuel is drawable above the protected reserve'
                    elseif startupRequiredKJ > 0
                        and generation + batteryDeliverableKW(self, fixed_hours) + 1e-9
                            < gen.startupPowerKW then
                        gen.blockReason = 'the pool cannot supply the startup energy draw'
                    else
                        gen.startupReceivedKJ = 0
                        if startupRequiredKJ > 0 then
                            gen.phase = 'starting'
                        else
                            gen.phase = 'running'
                        end
                    end
                end
            elseif gen.phase == 'starting' then
                gen.outputKW = 0
                if not coolingAvailable then
                    -- Cooling lost mid-crank: the start fails. The
                    -- cranking energy already spent is accounted as
                    -- heat in the sink; the attempt restarts from zero.
                    gen.phase = 'stopped'
                    gen.startupReceivedKJ = 0
                    gen.blockReason = 'cooling lost during startup'
                elseif drawableKWh <= 0 then
                    gen.phase = 'stopped'
                    gen.startupReceivedKJ = 0
                    gen.blockReason = 'fuel depleted during startup'
                else
                    local requestKW = gen.startupPowerKW
                    local poolKW = generation + batteryDeliverableKW(self, fixed_hours)
                    if poolKW + 1e-9 < requestKW then
                        -- The startup energy support that qualified the
                        -- start has gone away mid-crank: the start fails
                        -- whole rather than cranking on a trickle.
                        gen.phase = 'stopped'
                        gen.startupReceivedKJ = 0
                        gen.blockReason = 'startup energy support lost mid-crank'
                    else
                        local fromGeneration = math.min(requestKW, generation)
                        generation = generation - fromGeneration
                        startupFromGeneration = startupFromGeneration + fromGeneration
                        local ask = requestKW - fromGeneration
                        local fromBattery = drawBatteries(self, ask, fixed_hours)
                        batterySupplied = batterySupplied + fromBattery
                        local suppliedKW = fromGeneration + fromBattery
                        gen.lastStartupDrawKW = suppliedKW
                        -- Cranking energy dissipates into the same sink
                        -- the running machine will use: booked once here.
                        gen.heatSink.generatorHeat = (gen.heatSink.generatorHeat or 0) + suppliedKW
                        gen.startupHeatKJ = gen.startupHeatKJ + suppliedKW * fixed_seconds
                        gen.startupReceivedKJ = gen.startupReceivedKJ + suppliedKW * fixed_seconds
                        if gen.startupReceivedKJ + 1e-9 >= startupRequiredKJ then
                            gen.phase = 'running'
                        end
                    end
                end
            elseif gen.phase == 'running' then
                if not coolingAvailable then
                    -- Cooling lost while running: the generator trips to
                    -- shutdown. Electrical output and fuel burn stop
                    -- immediately; the configured decay heat carries on
                    -- below.
                    gen.phase = 'shutdown'
                    gen.shutdownElapsed = 0
                    gen.outputKW = 0
                    gen.blockReason = 'cooling lost: generator tripped to shutdown'
                elseif commanded <= 0 then
                    gen.phase = 'shutdown'
                    gen.shutdownElapsed = 0
                    gen.outputKW = 0
                else
                    -- Ramp toward the commanded output: the output may
                    -- move at most ramp x dt this tick, up or down, so
                    -- no tick commands an instantaneous step the machine
                    -- cannot physically make.
                    local maxStepKW = gen.rampKWPerSecond * fixed_seconds
                    local targetKW = math.min(commanded, gen.ratedKW)
                    gen.outputKW = gen.outputKW
                        + math.max(-maxStepKW, math.min(maxStepKW, targetKW - gen.outputKW))
                    -- Fuel feasibility: the fuel drawable above the
                    -- protected reserve caps the output this tick. A
                    -- generator with no fuel left cannot run at all.
                    local neededFuelKWh = gen.outputKW * fixed_hours / gen.efficiency
                    if neededFuelKWh > drawableKWh then
                        if drawableKWh <= 1e-15 then
                            -- Fuel depletion stops electrical output
                            -- immediately; the decay period follows.
                            gen.phase = 'shutdown'
                            gen.shutdownElapsed = 0
                            gen.outputKW = 0
                            gen.blockReason = 'fuel depleted: generator shut down'
                        else
                            gen.outputKW = drawableKWh * gen.efficiency / fixed_hours
                        end
                    end
                    if gen.phase == 'running' then
                        local drawnKWh = gen.outputKW * fixed_hours / gen.efficiency
                        local drawnUnits = drawnKWh / kwhPerUnit
                        gen.fuelStore.qualified = gen.fuelStore.qualified - drawnUnits
                        self.unit_loss[gen.fuelStore.unit] =
                            (self.unit_loss[gen.fuelStore.unit] or 0) + drawnUnits
                        gen.lastFuelDrawKWh = drawnKWh
                        local fuelKJ = drawnKWh * 3600
                        local electricalKJ = gen.outputKW * fixed_seconds
                        -- Waste heat is the fuel energy the conversion
                        -- did not deliver as electricity: booked once
                        -- into the sink, never also exported elsewhere.
                        local wasteKJ = fuelKJ - electricalKJ
                        gen.fuelEnergyKJ = gen.fuelEnergyKJ + fuelKJ
                        gen.electricalKJ = gen.electricalKJ + electricalKJ
                        gen.wasteHeatKJ = gen.wasteHeatKJ + wasteKJ
                        local wasteKW = wasteKJ / fixed_seconds
                        gen.lastWasteHeatKW = wasteKW
                        gen.heatSink.generatorHeat = (gen.heatSink.generatorHeat or 0) + wasteKW
                        -- The generator's output joins the shared pool
                        -- the loads allocate against this tick -
                        -- generation earned with fuel, not sunshine.
                        generation = generation + gen.outputKW
                    end
                end
            else
                gen.outputKW = 0
            end
            -- #563 mirror: decay heat books from the very tick the
            -- electrical output stopped - no transition-tick gap - and
            -- the final decay interval is prorated to the remaining
            -- configured time, so the sink receives exactly
            -- shutdownHeatKW * shutdownSeconds, never a rounded-up
            -- tick. The decay heat cannot be refused - it happens
            -- whether or not cooling is available - so it books
            -- regardless. A zero decay period books nothing. The
            -- Shutdown phase persists until the tick after the last
            -- decay interval so the phase transitions still bracket
            -- the decay period.
            local decayedThisTick = false
            if gen.phase == 'shutdown' then
                local remainingSeconds = gen.shutdownSeconds - gen.shutdownElapsed
                if remainingSeconds <= 1e-9 then
                    -- Nothing left to decay. A generator that entered
                    -- Shutdown only this tick keeps the phase for it so
                    -- the transition events still bracket even an
                    -- empty decay period; one already in Shutdown stops.
                    if phaseBefore == 'shutdown' then
                        gen.phase = 'stopped'
                    end
                else
                    local intervalSeconds = math.min(fixed_seconds, remainingSeconds)
                    local bookedKJ = gen.shutdownHeatKW * intervalSeconds
                    -- The published rate is the booked energy spread
                    -- over the fixed tick, so rate x dt equals the
                    -- energy the sink actually received this tick even
                    -- on the prorated final interval.
                    gen.lastDecayHeatKW = bookedKJ / fixed_seconds
                    gen.heatSink.generatorHeat =
                        (gen.heatSink.generatorHeat or 0) + gen.lastDecayHeatKW
                    gen.decayHeatKJ = gen.decayHeatKJ + bookedKJ
                    gen.shutdownElapsed = gen.shutdownElapsed + intervalSeconds
                    decayedThisTick = bookedKJ > 0
                end
            end
            -- The cooling obligation is exposed on every interval that
            -- actually produces decay heat - including the tick on
            -- which the last of it is booked, whatever phase that tick
            -- ends in - and while the machine is running (#563).
            gen.lastCoolingRequired = gen.phase == 'running' or decayedThisTick
        end
        -- The audit baseline is the source pool as it was originally
        -- available this tick - solar plus generator output - before the
        -- cranking draws took their slice. The crank is counted exactly
        -- once, on the demand side in totalSupplied; leaving it subtracted
        -- from the baseline too would book it twice and make a
        -- source-funded startup fail its own audit (#561).
        local availableGeneration = generation + startupFromGeneration
        local supplied = {}
        for _, index in ipairs(self.load_order) do
            local load = self.loads[index]
            load.lastLevel = evaluateProfile(load.profile, elapsed)
            load.requested = load.demandKW * load.lastLevel
            load.admitted = load.cooling == 'exported'
            load.unmetCritical = 0
            load.shortfall = ''
            load.materialFraction = 1
            load.cappedRequest = load.requested
            load.materialComputed = false
            load.materialRefused = false
            load.materialReserved = false
            for _, exchange in ipairs(load.exchanges) do exchange.lastReserved = 0 end
            supplied[index] = 0
            if not load.admitted and load.requested > 0 then
                -- A cooling-blocked load receives no electrical service,
                -- so every watt of its protected floor stays unmet rather
                -- than vanishing with the refusal (#550).
                load.unmetCritical = math.min(load.criticalFloorKW, load.requested)
            elseif load.admitted and load.requested > 0 then
                local floor = math.min(load.criticalFloorKW, load.requested)
                if floor > 0 then
                    local minOperating = load.requested * (1 - load.maxCurtailment)
                    -- #554: the request is capped to the fraction of the
                    -- full-operation exchanges the declared feed and
                    -- destination limits sustain; a load whose capped
                    -- request cannot reach its minimum operable level is
                    -- refused whole and spends no power or material.
                    local material, limiter = materialFeasible(load, fixed_seconds)
                    load.materialFraction = material
                    load.cappedRequest = load.requested * material
                    load.materialComputed = true
                    if load.cappedRequest <= 1e-12
                        or load.cappedRequest + 1e-12 < minOperating then
                        load.unmetCritical = floor
                        load.shortfall = limiter
                        load.materialRefused = true
                    else
                        -- The protected claim never exceeds what the
                        -- material can feed: a floor above the capped
                        -- request stays partly unmet rather than buying
                        -- power the equipment cannot use.
                        local claim = math.min(math.max(floor, minOperating),
                            load.cappedRequest)
                        local reachable = math.min(claim,
                            generation + batteryDeliverableKW(self, fixed_hours))
                        if reachable + 1e-12 < minOperating then
                            -- The pools cannot run this load at all: it is
                            -- refused whole, spends no scarce energy, and
                            -- every watt of its protected floor stays unmet
                            -- rather than just the slice the pool happened
                            -- to fall short of.
                            load.unmetCritical = floor
                            load.shortfall =
                                'critical floor cannot reach the minimum operable supply level'
                        else
                            local fromGeneration = math.min(reachable, generation)
                            generation = generation - fromGeneration
                            local stillNeeded = reachable - fromGeneration
                            local fromBattery = drawBatteries(self, stillNeeded, fixed_hours)
                            supplied[index] = fromGeneration + fromBattery
                            batterySupplied = batterySupplied + fromBattery
                            reserveMaterial(load, material, fixed_seconds)
                            load.unmetCritical = math.max(0, floor - supplied[index])
                        end
                    end
                end
            end
        end
        for _, index in ipairs(self.load_order) do
            local load = self.loads[index]
            if load.admitted and load.requested > 0 and not load.materialRefused then
                local minOperating = load.requested * (1 - load.maxCurtailment)
                -- Loads with no protected floor were not evaluated in
                -- pass 1: their material feasibility is read against the
                -- inventory as it stands after the protected grants
                -- ahead of them (#554).
                if not load.materialComputed then
                    local material, limiter = materialFeasible(load, fixed_seconds)
                    load.materialFraction = material
                    load.cappedRequest = load.requested * material
                    load.materialComputed = true
                    if load.cappedRequest <= 1e-12
                        or load.cappedRequest + 1e-12 < minOperating then
                        load.shortfall = limiter
                        load.materialRefused = true
                    end
                end
                if not load.materialRefused then
                    local remaining = load.cappedRequest - supplied[index]
                    if remaining > 0 then
                        local fromGeneration = math.min(remaining, generation)
                        local afterGeneration = remaining - fromGeneration
                        local batteryAsk = math.min(afterGeneration,
                            batteryDeliverableKW(self, fixed_hours))
                        if supplied[index] + fromGeneration + batteryAsk + 1e-12 >= minOperating then
                            supplied[index] = supplied[index] + fromGeneration
                            generation = generation - fromGeneration
                            -- Route the full remaining need, not just the
                            -- capped ask, so each battery's recorded request
                            -- and shortfall show the limitation the
                            -- allocation actually ran into.
                            local drawn = drawBatteries(self, afterGeneration, fixed_hours)
                            supplied[index] = supplied[index] + drawn
                            batterySupplied = batterySupplied + drawn
                            if not load.materialReserved then
                                reserveMaterial(load, load.materialFraction, fixed_seconds)
                            end
                        elseif load.shortfall == '' then
                            load.shortfall = 'below the minimum operable supply level'
                        end
                    end
                end
            end
        end
        -- Coolant loops run after the external load allocation and
        -- before battery charging (#396): the pump and freeze heater
        -- draw from the same generation-and-battery pool as external
        -- loads — never a second budget — and their heat must be in the
        -- loop before the reservoir integrates this tick's flows. Each
        -- loop circulates only the coolant it actually holds: the fill
        -- fraction caps the pump demand, so a dry loop draws nothing
        -- and flows nothing, and an underfilled loop is capped
        -- proportionally. Pump work and heater heat enter the loop
        -- exactly once and reach the reservoir only through the
        -- circulation-gated exchanger. Loops run in declaration order.
        for _, loop in ipairs(self.coolant_loops) do
            local disabled = loop.coolant <= 0
            local fill = disabled and 0 or math.min(1, loop.coolant / loop.capacity)
            -- A dry loop cannot run a pump: no fluid means no draw and
            -- no flow, never an invented one.
            local pumpDemand = disabled and 0 or loop.pumpKW * fill
            local pumpSupplied = 0
            local pumpFromGeneration = math.min(pumpDemand, generation)
            pumpSupplied = pumpSupplied + pumpFromGeneration
            generation = generation - pumpFromGeneration
            local pumpFromBattery = drawBatteries(self, pumpDemand - pumpFromGeneration, fixed_hours)
            pumpSupplied = pumpSupplied + pumpFromBattery
            batterySupplied = batterySupplied + pumpFromBattery
            -- Freeze-protection thermostat: demand up to capacity while
            -- the loop is below its setpoint. A dry loop heats nothing.
            local heaterDemand = 0
            if not disabled and loop.temperature < loop.setpoint then
                heaterDemand = loop.heaterKW
            end
            local heaterSupplied = 0
            local heaterFromGeneration = math.min(heaterDemand, generation)
            heaterSupplied = heaterSupplied + heaterFromGeneration
            generation = generation - heaterFromGeneration
            local heaterFromBattery = drawBatteries(self, heaterDemand - heaterFromGeneration, fixed_hours)
            heaterSupplied = heaterSupplied + heaterFromBattery
            batterySupplied = batterySupplied + heaterFromBattery
            -- Circulation is capped by the fluid present, the
            -- electricity supplied and the pump's rated flow: the
            -- fraction of rated flow that is actually moving. A zero
            -- rated flow is a zero-flow machine - fill and a powered
            -- pump notwithstanding, no coolant moves, so every
            -- circulation-gated exchange and evaporator admission is
            -- zero rather than a fraction of a flow that does not
            -- exist (#558).
            local pumpElectrical = pumpDemand > 0 and pumpSupplied / pumpDemand or 0
            local circulation = (disabled or loop.ratedFlow <= 0) and 0
                or fill * pumpElectrical
            -- What the exchanger is worth this tick. The exchange itself
            -- is not computed here: it is solved once per reservoir
            -- below, together with every other body coupled to that
            -- reservoir, so the whole star shares one finite heat
            -- budget (#559). No circulation means no conductance.
            loop.exchangeConductance = circulation * loop.conductance
            loop.lastPumpDemand = pumpDemand
            loop.lastPumpSupplied = pumpSupplied
            loop.lastHeaterDemand = heaterDemand
            loop.lastHeaterSupplied = heaterSupplied
            loop.lastCirculation = circulation
            loop.lastFlow = loop.ratedFlow * circulation
            assert(isFinite(loop.lastFlow) and isFinite(loop.exchangeConductance)
                and isFinite(circulation)
                and isFinite(pumpSupplied) and isFinite(heaterSupplied),
                "coolant loop '" .. loop.id .. "' allocation result is not finite")
        end
        -- Passive conductance exchange (#559) is integrated once per
        -- reservoir over the whole star of bodies heat-only coupled to
        -- it — every coolant loop and every pressurised compartment —
        -- rather than edge by edge against a tick-start temperature
        -- nobody updates. A tick-start rate applied whole moves a small
        -- body past the equilibrium it shares with a large one: past the
        -- crossing the formula keeps pushing, so a stiff conductance
        -- reverses hot and cold and drives temperatures below absolute
        -- zero, and several leaves on one reservoir each drain a
        -- temperature none of them ever sees move. Solving the star
        -- implicitly instead cannot overshoot whatever the conductance or
        -- the capacity ratio; see solvePassiveExchange. The solve runs
        -- before the chiller pass below so the exchanger's claim on each
        -- loop this tick is known when the chiller's finite cold-side
        -- bound is sized (#560); it reads only tick-start temperatures
        -- and the circulation-gated conductance, so the chillers change
        -- nothing it sees.
        local stars = {}
        for _, loop in ipairs(self.coolant_loops) do
            loop.lastExchange = 0
            if loop.exchangeConductance > 0 then
                local star = stars[loop.reservoir]
                if not star then
                    star = {}
                    stars[loop.reservoir] = star
                end
                star[#star + 1] = passiveExchangeLeaf(
                    loop.coolant * loop.cp, loop.exchangeConductance, loop.temperature)
                star[#star].kind = 'loop'
                star[#star].ref = loop
            end
        end
        for _, compartment in ipairs(self.atmospheres) do
            compartment.pendingExchangeKW = 0
            if compartment.reservoir and compartment.conductance > 0 then
                local star = stars[compartment.reservoir]
                if not star then
                    star = {}
                    stars[compartment.reservoir] = star
                end
                star[#star + 1] = passiveExchangeLeaf(
                    compartment.capacity, compartment.conductance, compartment.temperature)
                star[#star].kind = 'atmosphere'
                star[#star].ref = compartment
            end
        end
        for _, reservoir in ipairs(self.reservoirs) do
            local leaves = stars[reservoir]
            if leaves then
                local coupled = solvePassiveExchange(leaves, reservoir.capacity,
                    reservoir.temperature, fixed_seconds)
                -- Each leaf's heat is booked on the leaf as a rate over
                -- the tick and negated onto the reservoir by its own
                -- pass below, so the two ledgers close on one number
                -- rather than two rounded ones.
                for _, leaf in ipairs(leaves) do
                    local heatKJ = passiveExchangeHeatKJ(leaf, coupled)
                    assert(isFinite(heatKJ),
                        "passive exchange for '" .. leaf.ref.id .. "' is not finite")
                    if leaf.kind == 'loop' then
                        leaf.ref.lastExchange = heatKJ / fixed_seconds
                    else
                        leaf.ref.pendingExchangeKW = heatKJ / fixed_seconds
                    end
                end
            end
        end
        -- Chillers (#397) run between the loop allocation and the loop's
        -- own integration, in declaration order: every loop's circulation
        -- and exchanger share this tick are known, and the extraction
        -- lands in the loop's temperature integration below. A chiller's
        -- cold service demand is its rated capacity; the deliverable duty
        -- is capped by the cold loop's circulation fraction (heat must
        -- reach the evaporator) and by the heat the coolant actually
        -- holds that can be extracted above the chilled setpoint (#560),
        -- it is gated off at or below the chilled setpoint, and it is
        -- refused whole while the condenser sink sits at or above the
        -- maximum condensing temperature. The electrical
        -- request - work at the configured COP plus the running
        -- auxiliaries for the deliverable duty - is drawn from the same
        -- generation-and-battery pool as every other load, never a
        -- second budget. A curtailed grant duty-cycles the whole machine
        -- at one delivered fraction, so extracted heat, work and
        -- condenser heat all scale together and the condenser booking
        -- equals extracted heat plus the work and auxiliaries actually
        -- supplied at one consistent boundary.
        for _, chiller in ipairs(self.chillers) do
            local loop = chiller.coldLoop
            local sink = chiller.condenser
            local disabled = chiller.ratedKW <= 0
            chiller.atSetpoint = not disabled
                and loop.temperature <= chiller.chilledSetpoint + 1e-9
            chiller.sinkOver = not disabled
                and sink.temperature + 1e-9 >= chiller.maxCondensing
            local coolingDemand = (disabled or chiller.atSetpoint)
                and 0 or chiller.ratedKW
            chiller.lastDemand = coolingDemand
            chiller.lastCooling = 0
            chiller.lastDeliverable = 0
            chiller.lastPowerDemand = 0
            chiller.lastPowerSupplied = 0
            chiller.lastCondenserKW = 0
            chiller.lastFraction = 0
            chiller.admitted = not disabled and not chiller.sinkOver
            if chiller.admitted and coolingDemand > 0 then
                -- A nameplate rating is not a heat source (#560). Over
                -- the tick the cold loop can give up only the heat its
                -- own thermal mass holds above the chilled setpoint -
                -- the cold-side interface the machine exists to hold -
                -- plus the heat legitimately entering it this tick: pump
                -- work, freeze-heater heat and the exchanger's implicit-
                -- solve share, each counted exactly once. Extracting
                -- more than that would end the tick below the setpoint,
                -- so the loop may be cooled to the interface and no
                -- further; the setpoint is validated above absolute
                -- zero, so the coolant can never be driven below it.
                -- Work, auxiliaries and the condenser booking all size
                -- off the capped deliverable, so every ledger scales
                -- with the duty that actually moves.
                local loopCapacityKJ = loop.coolant * loop.cp
                local extractableKJ = math.max(0,
                    loopCapacityKJ * math.max(0, loop.temperature - chiller.chilledSetpoint)
                    + (loop.lastPumpSupplied + loop.lastHeaterSupplied) * fixed_seconds
                    + loop.lastExchange * fixed_seconds)
                local circulationCapped = chiller.ratedKW * loop.lastCirculation
                local deliverable = math.min(circulationCapped,
                    extractableKJ / fixed_seconds)
                chiller.lastDeliverable = deliverable
                if deliverable > 1e-12 then
                    local powerDemand = deliverable / chiller.cop + chiller.auxKW
                    chiller.lastPowerDemand = powerDemand
                    local fromGeneration = math.min(powerDemand, generation)
                    generation = generation - fromGeneration
                    local fromBattery = drawBatteries(self,
                        powerDemand - fromGeneration, fixed_hours)
                    local supplied = fromGeneration + fromBattery
                    batterySupplied = batterySupplied + fromBattery
                    chiller.lastPowerSupplied = supplied
                    local fraction = supplied / powerDemand
                    chiller.lastFraction = fraction
                    local cooling = deliverable * fraction
                    chiller.lastCooling = cooling
                    -- Condenser heat equals extracted heat plus the work
                    -- and auxiliaries actually supplied: one boundary,
                    -- booked once.
                    chiller.lastCondenserKW = cooling + supplied
                    loop.chillerExtraction = (loop.chillerExtraction or 0) + cooling
                    chiller.extractedKJ = chiller.extractedKJ + cooling * fixed_seconds
                    chiller.workKJ = chiller.workKJ + supplied * fixed_seconds
                    chiller.condenserKJ = chiller.condenserKJ
                        + chiller.lastCondenserKW * fixed_seconds
                end
            end
            assert(isFinite(chiller.lastDemand) and isFinite(chiller.lastCooling)
                and isFinite(chiller.lastDeliverable) and isFinite(chiller.lastPowerDemand)
                and isFinite(chiller.lastPowerSupplied) and isFinite(chiller.lastCondenserKW)
                and isFinite(chiller.lastFraction) and isFinite(chiller.extractedKJ)
                and isFinite(chiller.workKJ) and isFinite(chiller.condenserKJ),
                "chiller '" .. chiller.id .. "' result is not finite")
        end
        -- Second loop pass: integrate the loop's temperature from the
        -- pump work, heater heat, exchanger exchange and the chiller
        -- extraction that actually moved this tick, and expose the
        -- freeze condition. The extraction leaves the loop exactly once;
        -- the chiller books it as the cold side of its own ledger and
        -- the sink gains it through the condenser term, so the heat is
        -- never double-counted.
        for _, loop in ipairs(self.coolant_loops) do
            local disabled = loop.coolant <= 0
            local extraction = loop.chillerExtraction or 0
            loop.chillerExtraction = 0
            if not disabled then
                local heatIntoLoop = loop.lastPumpSupplied + loop.lastHeaterSupplied
                local loopCapacityKJ = loop.coolant * loop.cp
                loop.temperature = loop.temperature
                    + (heatIntoLoop + loop.lastExchange - extraction)
                        * fixed_seconds / loopCapacityKJ
                loop.pumpWorkKJ = loop.pumpWorkKJ + loop.lastPumpSupplied * fixed_seconds
                loop.heaterKJ = loop.heaterKJ + loop.lastHeaterSupplied * fixed_seconds
                loop.exchangedKJ = loop.exchangedKJ + loop.lastExchange * fixed_seconds
                loop.extractedKJ = (loop.extractedKJ or 0) + extraction * fixed_seconds
            end
            -- The freeze condition is exposed, never clamped: the loop
            -- stays integrated and the shortfall stays visible.
            loop.belowFreeze = not disabled
                and loop.temperature < loop.setpoint - 1e-9
            loop.circulating = loop.lastFlow > 1e-12
            assert(isFinite(loop.temperature) and isFinite(loop.extractedKJ),
                "coolant loop '" .. loop.id .. "' integrated result is not finite")
        end
        -- Charge from the surplus left after every allocation: batteries
        -- take only generation no load took, within the charge power
        -- limit, the headroom below nameplate and the charge efficiency.
        -- A battery that discharged this tick never charges in the same
        -- tick, so no cycle can circulate energy.
        for _, battery in ipairs(self.batteries) do
            if not batteryDisabled(battery) and generation > 0 and battery.last_discharge <= 0 then
                local headroom = battery.nameplateKWh - battery.stored
                if headroom > 0 then
                    local busKW = math.min(battery.maxChargeKW, generation)
                    if busKW > 0 then
                        local storedGain = math.min(busKW * fixed_hours
                            * battery.chargeEfficiency, headroom)
                        if storedGain > 0 then
                            local busUsed = storedGain / battery.chargeEfficiency
                            battery.stored = battery.stored + storedGain
                            battery.charged = battery.charged + storedGain
                            battery.charge_input = battery.charge_input + busUsed
                            battery.last_charge = busUsed / fixed_hours
                            -- The pool is in power units: subtract the bus
                            -- power the battery actually took.
                            generation = generation - busUsed / fixed_hours
                        end
                    end
                end
            end
        end
        for index, load in ipairs(self.loads) do
            local amount = supplied[index] or 0
            load.supplied = amount
            load.deliveredFraction = load.requested > 0 and amount / load.requested or 0
            -- Authored heat is a full-operation rate: export scales by the
            -- operating-profile level times the delivered fraction of the
            -- profile-scaled request (#548).
            -- Emitted heat is booked exactly once (#395): a load with a
            -- declared heat sink routes its produced heat into the
            -- reservoir and exports nothing directly; without a sink the
            -- heat leaves through the declared export boundary as
            -- before. Electrical input is never duplicate local heat.
            local producedHeat = load.admitted and load.heatKW * load.lastLevel
                * load.deliveredFraction or 0
            load.heatRouted = load.heatSink and producedHeat or 0
            load.heatExported = load.heatSink and 0 or producedHeat
            load.unmetCooling = load.admitted and 0 or load.heatKW * load.lastLevel
            -- Declared material exchanges are full-operation rates and scale
            -- by the full-operation delivered fraction (profile level times
            -- the delivered fraction) (#548). The exchange's requested
            -- amount is the full-operation profile need and what completes
            -- is that need at the delivered fraction, bounded by the
            -- amount reserved against the declared feed and destination
            -- limits when the grant ran (#554); a refused load never
            -- reserved, so it completes nothing and the whole need stands
            -- as unmet material. The unused reservation returns to the
            -- live inventory before transfers and conversions run.
            -- Completed amounts book at the external boundary: draws are
            -- losses, deliveries are negative losses, so the station
            -- ledgers stay closed.
            for _, exchange in ipairs(load.exchanges) do
                local store = exchange.store
                local requested = exchange.rate * fixed_seconds * load.lastLevel
                local completed = math.min(requested * load.deliveredFraction,
                    exchange.lastReserved)
                if exchange.consumes then
                    store.qualified = store.qualified + exchange.lastReserved - completed
                    self.unit_loss[store.unit] = (self.unit_loss[store.unit] or 0) + completed
                    if store.composition then
                        addConstituents(self.constituent_loss, store.composition,
                            toKg(store.unit, completed), 1)
                    end
                else
                    store.qualified = store.qualified - (exchange.lastReserved - completed)
                    self.unit_loss[store.unit] = (self.unit_loss[store.unit] or 0) - completed
                    if store.composition then
                        addConstituents(self.constituent_loss, store.composition,
                            toKg(store.unit, completed), -1)
                    end
                end
                exchange.lastRequested = requested
                exchange.lastCompleted = completed
                exchange.lastUnmet = requested - completed
                exchange.lastReserved = 0
            end
        end
        -- Allocation audit: allocated power never exceeds generation plus
        -- actual battery output and no load receives more than it
        -- requested. Coolant pump and heater draws ride the same pool
        -- as external loads (#396), so they are inside this audit too —
        -- the loop is never a second power budget. Shortages are valid;
        -- an over-allocation is a software failure and fails contained.
        local totalSupplied = 0
        for index = 1, #self.loads do totalSupplied = totalSupplied + (supplied[index] or 0) end
        for _, loop in ipairs(self.coolant_loops) do
            totalSupplied = totalSupplied + loop.lastPumpSupplied + loop.lastHeaterSupplied
        end
        for _, chiller in ipairs(self.chillers) do
            totalSupplied = totalSupplied + chiller.lastPowerSupplied
        end
        -- Generator startup cranking draws ride the same pool as every
        -- other service (#399); generator output is generation inside
        -- it. Neither side is ever a second budget.
        for _, gen in ipairs(self.generators) do
            totalSupplied = totalSupplied + gen.lastStartupDrawKW
        end
        local generationBound = BALANCE_TOLERANCE
            * math.max(1.0, math.abs(availableGeneration) + batterySupplied)
        assert(totalSupplied <= availableGeneration + batterySupplied + generationBound,
            'power allocation exceeded generation plus battery output')
        for index = 1, #self.loads do
            assert((supplied[index] or 0) <= self.loads[index].requested + generationBound,
                'load supplied above its requested service')
        end
        -- The minimum-operable audit (#549): a load is either off or at
        -- least its minimum operable supply, requested * (1 - allowed
        -- curtailment). A grant landing strictly between nothing and that
        -- minimum buys a state the equipment cannot physically run in, so
        -- it is a software failure rather than a shortage.
        for index = 1, #self.loads do
            local load = self.loads[index]
            if load.admitted and load.requested > 0 then
                local minOperating = load.requested * (1 - load.maxCurtailment)
                local bound = BALANCE_TOLERANCE * math.max(1.0, load.requested)
                local amount = supplied[index] or 0
                assert(amount <= 1e-12 or amount + bound >= minOperating,
                    'load supplied below its minimum operable supply level')
            end
        end
        -- Battery energy audit: stored energy stays inside the nameplate,
        -- the ledger closes over charge and discharge, nothing is
        -- delivered for free, nothing is stored from nothing, and the
        -- reserve floor is never breached.
        for _, battery in ipairs(self.batteries) do
            local bound = BALANCE_TOLERANCE * math.max(1.0, math.abs(battery.startingStored))
            assert(battery.stored == battery.stored and battery.stored >= 0,
                'battery stored energy went negative')
            assert(battery.stored <= battery.nameplateKWh + bound,
                'battery stored energy exceeded its nameplate capacity')
            local floor = math.min(battery.startingStored, batteryReserve(battery))
            assert(battery.stored >= floor - bound,
                'battery drawn below its reserve floor')
            local residual = battery.startingStored - battery.discharged
                + battery.charged - battery.stored
            assert(math.abs(residual) <= bound,
                'battery energy ledger residual exceeds tolerance')
            assert(battery.delivered <= battery.discharged + bound,
                'battery delivered more energy than it drew from storage')
            assert(battery.charged <= battery.charge_input + bound,
                'battery stored more energy than the bus supplied to it')
        end
        -- #553: finiteness audit of every committed power/material/
        -- energy output and ledger intermediate. Finite inputs do not
        -- guarantee finite sums or products, and a non-finite
        -- magnitude passes every inequality audit unnoticed, so
        -- finiteness is checked explicitly. Finite shortages are
        -- valid results and are untouched here.
        for index = 1, #self.loads do
            local load = self.loads[index]
            local results = { load.requested, load.supplied, load.deliveredFraction,
                load.materialFraction, load.cappedRequest, load.unmetCritical,
                load.heatExported, load.heatRouted, load.unmetCooling }
            for _, value in ipairs(results) do
                assert(isFinite(value),
                    "equipment load '" .. load.id .. "' result is not finite")
            end
            for _, exchange in ipairs(load.exchanges) do
                assert(isFinite(exchange.lastRequested) and isFinite(exchange.lastCompleted)
                    and isFinite(exchange.lastUnmet),
                    "equipment load '" .. load.id .. "' material exchange amount is not finite")
            end
        end
        for _, battery in ipairs(self.batteries) do
            local amounts = { battery.stored, battery.charged, battery.discharged,
                battery.delivered, battery.charge_input, battery.last_charge,
                battery.last_discharge }
            for _, value in ipairs(amounts) do
                assert(isFinite(value),
                    "battery '" .. battery.id .. "' energy amount is not finite")
            end
        end
        assert(isFinite(availableGeneration), 'remaining generation pool is not finite')
        assert(isFinite(totalSupplied), 'total supplied power is not finite')
        assert(isFinite(batterySupplied), 'total battery supplied power is not finite')
    end
    -- Mirror of the host thermal ledger (#395): explicit Euler on the
    -- fixed World tick. Each reservoir accumulates its authored absorbed
    -- heat plus the equipment heat routed into it, against radiator
    -- rejection following the Stefan-Boltzmann law at the current
    -- emitting temperature with explicit environmental back-radiation.
    -- A body cannot radiatively cool below its coldest sink nor warm
    -- above its warmest one; the clamp is booked, not hidden. The energy
    -- ledger closes over exactly the terms that moved.
    local function applyThermal(self, fixed_seconds)
        local routedByReservoir = {}
        for _, load in ipairs(self.loads) do
            if load.heatSink then
                routedByReservoir[load.heatSink] =
                    (routedByReservoir[load.heatSink] or 0) + load.heatRouted
            end
        end
        -- Coolant loops exchange heat with their host reservoir through
        -- the circulation-gated exchanger (#396): the loop side was
        -- computed in applyPower, and the reservoir books the exact
        -- negation here — one exchange, booked once on each side.
        local loopHeatByReservoir = {}
        for _, loop in ipairs(self.coolant_loops) do
            loopHeatByReservoir[loop.reservoir] =
                (loopHeatByReservoir[loop.reservoir] or 0) - loop.lastExchange
        end
        -- Chiller condenser heat enters the sink reservoir exactly once
        -- (#397): the chiller booked extracted + supplied work here, and
        -- the reservoir integrates it as its own ledger term.
        local condenserByReservoir = {}
        for _, chiller in ipairs(self.chillers) do
            condenserByReservoir[chiller.condenser] =
                (condenserByReservoir[chiller.condenser] or 0) + chiller.lastCondenserKW
        end
        -- Generator heat enters the sink reservoir exactly once (#399):
        -- the generator booked waste, startup dissipation and decay heat
        -- here in applyPower, and the reservoir integrates it as its own
        -- ledger term.
        -- Compartment exchange enters the host reservoir exactly once
        -- (#400): the compartment booked its gained heat in
        -- applyAtmospheres and left the exact negation here, so the
        -- heat-only crossing is one exchange accounted on each side.
        for _, reservoir in ipairs(self.reservoirs) do
            local capacity = reservoir.capacity
            local temperatureK = reservoir.temperature + KELVIN_OFFSET
            local routed = routedByReservoir[reservoir] or 0
            local loopHeat = loopHeatByReservoir[reservoir] or 0
            local condenser = condenserByReservoir[reservoir] or 0
            local generatorHeat = reservoir.generatorHeat or 0
            reservoir.generatorHeat = 0
            local atmosphereHeat = reservoir.atmosphereHeat or 0
            reservoir.atmosphereHeat = 0
            local heatIn = reservoir.absorbedKW + routed + loopHeat + condenser
                + generatorHeat + atmosphereHeat
            local rejected = 0
            local minSink = reservoir.temperature
            local maxSink = reservoir.temperature
            local raw = {}
            for r = 1, #reservoir.radiators do
                local radiator = reservoir.radiators[r]
                local sinkK = radiator.sinkTemperatureC + KELVIN_OFFSET
                -- Multiplication, not x^4: mirrors the host's exact
                -- product so host and sandbox integrate identically.
                local value = radiator.emissivity * radiator.viewFactor * radiator.areaM2
                    * STEFAN_BOLTZMANN
                    * (temperatureK * temperatureK * temperatureK * temperatureK
                        - sinkK * sinkK * sinkK * sinkK)
                raw[r] = value
                rejected = rejected + value
                if radiator.sinkTemperatureC < minSink then minSink = radiator.sinkTemperatureC end
                if radiator.sinkTemperatureC > maxSink then maxSink = radiator.sinkTemperatureC end
            end
            local scale = 1
            if #reservoir.radiators > 0 then
                if rejected > 0 then
                    local cap = (reservoir.temperature - minSink) * capacity / fixed_seconds
                    if rejected > cap then scale = cap / rejected end
                elseif rejected < 0 then
                    local floor = (reservoir.temperature - maxSink) * capacity / fixed_seconds
                    if rejected < floor then scale = floor / rejected end
                end
            end
            for r = 1, #reservoir.radiators do
                reservoir.radiators[r].lastRejected = raw[r] * scale
            end
            rejected = rejected * scale
            local net = heatIn - rejected
            local newTemperature = reservoir.temperature + net * fixed_seconds / capacity
            reservoir.lastRouted = routed
            reservoir.lastLoopHeat = loopHeat
            reservoir.lastHeatIn = heatIn
            reservoir.lastRejected = rejected
            reservoir.lastNet = net
            reservoir.absorbedKJ = reservoir.absorbedKJ + reservoir.absorbedKW * fixed_seconds
            reservoir.routedKJ = reservoir.routedKJ + routed * fixed_seconds
            reservoir.loopKJ = (reservoir.loopKJ or 0) + loopHeat * fixed_seconds
            reservoir.condenserKJ = (reservoir.condenserKJ or 0) + condenser * fixed_seconds
            reservoir.generatorKJ = (reservoir.generatorKJ or 0) + generatorHeat * fixed_seconds
            reservoir.atmosphereKJ = (reservoir.atmosphereKJ or 0) + atmosphereHeat * fixed_seconds
            reservoir.rejectedKJ = reservoir.rejectedKJ + rejected * fixed_seconds
            reservoir.temperature = newTemperature
            -- Declared service limits are reported as crossings, never
            -- enforced by clamping the integrated state.
            reservoir.aboveLimit = newTemperature > reservoir.maxTemperature + 1e-9
            reservoir.belowLimit = newTemperature < reservoir.minTemperature - 1e-9
        end
    end
    -- Thermal energy boundary: starting + absorbed + routed + loop -
    -- rejected must equal capacity x temperature within the declared
    -- tolerance; a residual beyond it is a conservation violation,
    -- never silent creation or deletion of heat (#395). Coolant loops
    -- carry the same discipline: starting + pump work + heater +
    -- exchanged must equal the loop's current capacity x temperature,
    -- and the loop's finite coolant inventory never grows (#396).
    local function checkThermal(self)
        for _, reservoir in ipairs(self.reservoirs) do
            local startingEnergy = reservoir.capacity * reservoir.startingTemperature
            local bound = BALANCE_TOLERANCE * math.max(1, math.abs(startingEnergy))
            local residual = startingEnergy + reservoir.absorbedKJ + reservoir.routedKJ
                + (reservoir.loopKJ or 0) + (reservoir.condenserKJ or 0) + (reservoir.generatorKJ or 0)
                + (reservoir.atmosphereKJ or 0)
                - reservoir.rejectedKJ
                - reservoir.capacity * reservoir.temperature
            assert(residual == residual and math.abs(residual) <= bound,
                "thermal energy ledger residual for reservoir '"
                .. reservoir.id .. "' exceeds tolerance")
            local values = { reservoir.temperature, reservoir.lastRouted,
                reservoir.lastLoopHeat, reservoir.lastHeatIn, reservoir.lastRejected,
                reservoir.lastNet,
                reservoir.absorbedKJ, reservoir.routedKJ, reservoir.loopKJ,
                reservoir.rejectedKJ }
            for _, value in ipairs(values) do
                assert(isFinite(value),
                    "thermal reservoir '" .. reservoir.id .. "' value is not finite")
            end
        end
        for _, radiator in ipairs(self.radiators) do
            assert(isFinite(radiator.lastRejected),
                "radiator '" .. radiator.id .. "' rejected power is not finite")
        end
        for _, loop in ipairs(self.coolant_loops) do
            assert(loop.coolant >= 0 and loop.coolant == loop.coolant
                and loop.coolant <= loop.capacity + BALANCE_TOLERANCE
                    * math.max(1, loop.capacity),
                "coolant loop '" .. loop.id .. "' inventory left its finite bounds")
            local startingEnergy = loop.startingCoolant * loop.cp * loop.startingTemperature
            local bound = BALANCE_TOLERANCE * math.max(1, math.abs(startingEnergy))
            local residual = startingEnergy + loop.pumpWorkKJ + loop.heaterKJ
                + loop.exchangedKJ - (loop.extractedKJ or 0)
                - loop.coolant * loop.cp * loop.temperature
            assert(residual == residual and math.abs(residual) <= bound,
                "coolant loop '" .. loop.id .. "' thermal energy ledger residual "
                .. 'exceeds tolerance')
        end
        -- Chilled service ledger (#397): the chiller's condenser heat
        -- must equal the cold-side heat it extracted plus the work and
        -- auxiliaries it was actually supplied, cumulatively and within
        -- the declared tolerance. A residual beyond it means heat was
        -- booked twice or vanished once - a conservation violation,
        -- never a silent rounding.
        for _, chiller in ipairs(self.chillers) do
            local chillerBound = BALANCE_TOLERANCE * math.max(1,
                math.max(math.abs(chiller.extractedKJ), math.abs(chiller.condenserKJ)))
            local residual = chiller.extractedKJ + chiller.workKJ - chiller.condenserKJ
            assert(residual == residual and math.abs(residual) <= chillerBound,
                "chiller '" .. chiller.id .. "' condenser ledger residual "
                .. 'exceeds tolerance')
            local values = { chiller.lastDemand, chiller.lastCooling,
                chiller.lastDeliverable, chiller.lastPowerDemand,
                chiller.lastPowerSupplied, chiller.lastCondenserKW,
                chiller.lastFraction, chiller.extractedKJ, chiller.workKJ,
                chiller.condenserKJ }
            for _, value in ipairs(values) do
                assert(isFinite(value),
                    "chiller '" .. chiller.id .. "' value is not finite")
            end
            assert(chiller.lastCooling <= chiller.lastDeliverable + BALANCE_TOLERANCE,
                "chiller '" .. chiller.id .. "' extracted more than its circulation could deliver")
            assert(chiller.lastPowerSupplied <= chiller.lastPowerDemand + BALANCE_TOLERANCE,
                "chiller '" .. chiller.id .. "' was supplied more power than it demanded")
        end
        -- Generator fuel ledger (#399): every joule of fuel energy the
        -- generator consumed must appear as delivered electrical energy
        -- or as waste heat routed to the sink, cumulatively and within
        -- the declared tolerance. A residual beyond it means fuel energy
        -- vanished or electrical energy appeared from nowhere - a
        -- conservation violation, never a silent rounding. Output never
        -- exceeds the installed rating, and the machine never runs while
        -- its cooling interface is unqualified.
        for _, gen in ipairs(self.generators) do
            local genBound = BALANCE_TOLERANCE * math.max(1,
                math.max(math.abs(gen.fuelEnergyKJ), math.abs(gen.electricalKJ)))
            local residual = gen.fuelEnergyKJ - gen.electricalKJ - gen.wasteHeatKJ
            assert(residual == residual and math.abs(residual) <= genBound,
                "generator '" .. gen.id .. "' fuel ledger residual exceeds tolerance")
            local values = { gen.outputKW, gen.lastFuelDrawKWh, gen.lastWasteHeatKW,
                gen.lastStartupDrawKW, gen.lastDecayHeatKW, gen.startupReceivedKJ,
                gen.fuelEnergyKJ, gen.electricalKJ, gen.wasteHeatKJ,
                gen.startupHeatKJ, gen.decayHeatKJ }
            for _, value in ipairs(values) do
                assert(isFinite(value),
                    "generator '" .. gen.id .. "' value is not finite")
            end
            assert(gen.outputKW <= gen.ratedKW + BALANCE_TOLERANCE,
                "generator '" .. gen.id .. "' delivered above its installed capacity")
            assert(gen.phase ~= 'running' or gen.lastCoolingAvailable,
                "generator '" .. gen.id .. "' ran while its cooling interface was unqualified")
        end
    end
    -- Atmosphere containment ledger (#400): per-component closure and
    -- thermal closure within the declared tolerance, mirroring the host
    -- audit. A residual beyond it means gas vanished or appeared without
    -- a booked pathway - an unexplained disposal, a conservation
    -- violation, never a silent rounding.
    local function checkAtmospheres(self)
        for _, compartment in ipairs(self.atmospheres) do
            local startingTotal = compartment.startingOxygen + compartment.startingNitrogen
                + compartment.startingCarbonDioxide + compartment.startingWaterVapour
            local bound = BALANCE_TOLERANCE * math.max(1, startingTotal)
            assert(compartment.oxygen >= -bound and compartment.nitrogen >= -bound
                and compartment.carbonDioxide >= -bound and compartment.waterVapour >= -bound,
                "atmosphere '" .. compartment.id .. "' gas inventory went negative")
            local closures = {
                { 'oxygen', compartment.startingOxygen, 0,
                    compartment.oxygenConsumed + compartment.leakOxygen
                        + compartment.cycleOxygen,
                    compartment.oxygen },
                { 'nitrogen', compartment.startingNitrogen, 0,
                    compartment.leakNitrogen + compartment.cycleNitrogen,
                    compartment.nitrogen },
                { 'carbonDioxide', compartment.startingCarbonDioxide, compartment.co2Produced,
                    compartment.leakCarbonDioxide + compartment.cycleCarbonDioxide,
                    compartment.carbonDioxide },
                { 'waterVapour', compartment.startingWaterVapour, compartment.vapourAdded,
                    compartment.condensed + compartment.leakWaterVapour
                        + compartment.cycleWaterVapour,
                    compartment.waterVapour },
            }
            for _, closure in ipairs(closures) do
                local residual = closure[2] + closure[3] - closure[4] - closure[5]
                assert(residual == residual and math.abs(residual) <= bound,
                    "atmosphere '" .. compartment.id .. "' component '" .. closure[1]
                        .. "' conservation residual exceeds tolerance")
            end
            -- The airlock split closes over the removal: recovered and
            -- lost together are exactly the mass the cycles took out of
            -- the compartment - no unexplained disposal.
            local cycled = compartment.cycleOxygen + compartment.cycleNitrogen
                + compartment.cycleCarbonDioxide + compartment.cycleWaterVapour
            local splitResidual = compartment.recovered + compartment.lost - cycled
            assert(splitResidual == splitResidual and math.abs(splitResidual) <= bound,
                "atmosphere '" .. compartment.id
                    .. "' airlock recovered/lost split residual exceeds tolerance")
            -- Thermal closure: starting + occupant heat + latent heat +
            -- exchanged from the reservoir must equal the compartment's
            -- current capacity x temperature within the tolerance.
            local startingEnergy = compartment.capacity * compartment.startingTemperature
            local thermalBound = BALANCE_TOLERANCE * math.max(1, math.abs(startingEnergy))
            local thermalResidual = startingEnergy + compartment.occupantHeatKJ
                + compartment.latentHeatKJ + compartment.exchangedKJ
                - compartment.capacity * compartment.temperature
            assert(thermalResidual == thermalResidual and math.abs(thermalResidual) <= thermalBound,
                "atmosphere '" .. compartment.id
                    .. "' thermal energy ledger residual exceeds tolerance")
            local values = { compartment.temperature, compartment.oxygen,
                compartment.nitrogen, compartment.carbonDioxide, compartment.waterVapour,
                compartment.leaked, compartment.recovered, compartment.lost,
                compartment.condensed, compartment.oxygenConsumed, compartment.co2Produced,
                compartment.vapourAdded, compartment.occupantHeatKJ,
                compartment.latentHeatKJ, compartment.exchangedKJ }
            for _, value in ipairs(values) do
                assert(isFinite(value),
                    "atmosphere '" .. compartment.id .. "' value is not finite")
            end
        end
        for _, cycle in ipairs(self.airlock_cycles) do
            assert(isFinite(cycle.removedTotal) and isFinite(cycle.recoveredTotal)
                and isFinite(cycle.lostTotal),
                "airlock cycle '" .. cycle.id .. "' ledger is not finite")
            local bound = BALANCE_TOLERANCE * math.max(1, math.abs(cycle.removedTotal))
            assert(math.abs(cycle.recoveredTotal + cycle.lostTotal - cycle.removedTotal) <= bound,
                "airlock cycle '" .. cycle.id
                    .. "' recovered/lost totals do not close over the removed mass")
        end
    end
    function object:advance(world_tick, fixed_seconds)
        assert(world_tick > 0 and fixed_seconds > 0)
        checkFiniteLimits(self)
        local elapsed = world_tick * fixed_seconds
        -- Deliveries land first: what arrives this tick is available to
        -- this tick's equipment, transfers and conversions, matching the
        -- host ledger order.
        applyDeliveries(self, elapsed)
        applyPower(self, elapsed, fixed_seconds)
        -- Compartments run after the loops/chillers and before thermal
        -- integration: the compartment exchange must land inside this
        -- tick's reservoir integration, matching the host order (#400).
        applyAtmospheres(self, elapsed, fixed_seconds)
        applyThermal(self, fixed_seconds)
        applyTransfers(self, fixed_seconds)
        applyConversions(self, fixed_seconds)
        checkFiniteLimits(self)
        checkConservation(self)
        checkConstituentConservation(self)
        checkThermal(self)
        checkAtmospheres(self)
        self.ticks = self.ticks + 1
        return self.ticks
    end
    checkFiniteLimits(object)
    checkConservation(object)
    checkConstituentConservation(object)
    checkThermal(object)
    checkAtmospheres(object)
    return object
end
