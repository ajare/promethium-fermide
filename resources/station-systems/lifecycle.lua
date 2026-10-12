-- Station systems slice 5: lifecycle, authored finite Resource stores,
-- bounded authored transfers, compound-resource conversions, solar
-- generation, External equipment load supply, rechargeable battery banks,
-- scheduled resupply deliveries, thermal reservoirs with radiator
-- rejection, coolant loops with freeze-protection heating, chillers
-- supplying class-matched cold services, finite thermal buffers that
-- ride through cooling deficits, fuelled generators with
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
-- Molar mass of diatomic hydrogen gas in kg/mol (#406): a constant of
-- the declared species, mirroring the host's StationMolarMassHydrogenKgPerMol.
local MOLAR_MASS_HYDROGEN = 0.00201588
-- Stoichiometric oxygen mass per kilogram of split water: M_O2 / (2 x
-- M_H2O). Hydrogen is the declared mass-balance remainder, so the
-- electrolysis split closes exactly under the declared molar masses.
-- Mirrors the host's StationElectrolysisOxygenMassPerWaterKg (#404).
local ELECTROLYSIS_O2_KG_PER_WATER_KG = MOLAR_MASS_OXYGEN / (2.0 * MOLAR_MASS_WATER)
-- Lower heating value of hydrogen in kJ/kg: the declared chemical energy
-- retained per kilogram of produced H2. Mirrors the host's
-- StationElectrolysisChemicalEnergyHydrogenKJPerKg (#404).
local ELECTROLYSIS_H2_CHEMICAL_KJ_PER_KG = 119960.0
-- Chemical energy retained per kilogram of split water, in kJ.
local ELECTROLYSIS_CHEMICAL_KJ_PER_WATER_KG =
    (1.0 - ELECTROLYSIS_O2_KG_PER_WATER_KG) * ELECTROLYSIS_H2_CHEMICAL_KJ_PER_KG
-- Stoichiometric oxygen mass consumed per kilogram of hydrogen burnt:
-- M_O2 / (2 x M_H2). Product water is booked as the sum of the
-- reactants actually consumed, so the fuel-cell mass ledger closes
-- exactly under the declared molar masses. Mirrors the host's
-- StationFuelCellOxygenMassPerHydrogenKg (#406).
local FUEL_CELL_O2_KG_PER_H2_KG = MOLAR_MASS_OXYGEN / (2.0 * MOLAR_MASS_HYDROGEN)
-- Molar mass of solid carbon in kg/mol (#405): a constant of the
-- declared species, mirroring the host's StationMolarMassCarbonKgPerMol.
-- The declared methane molar mass is exactly the carbon plus two
-- hydrogen molecules, so the methane-processing split closes under the
-- declared molar masses.
local MOLAR_MASS_CARBON = 0.0120107
local MOLAR_MASS_METHANE = MOLAR_MASS_CARBON + 2.0 * MOLAR_MASS_HYDROGEN
-- Lower heating value of methane in kJ/kg: mirrors the host's
-- StationMethaneLowerHeatingValueKJPerKg (#405).
local METHANE_LHV_KJ_PER_KG = 50004.0
-- Stoichiometric masses of the Sabatier reaction CO2 + 4 H2 -> CH4 +
-- 2 H2O per kilogram of CO2 converted, mirroring the host's
-- StationSabatier* constants (#405): water is the mass-balance
-- remainder so the stage closes exactly.
local SABATIER_H2_KG_PER_CO2_KG = 4.0 * MOLAR_MASS_HYDROGEN / MOLAR_MASS_CARBON_DIOXIDE
local SABATIER_CH4_KG_PER_CO2_KG = MOLAR_MASS_METHANE / MOLAR_MASS_CARBON_DIOXIDE
local SABATIER_H2O_KG_PER_CO2_KG = 1.0 + SABATIER_H2_KG_PER_CO2_KG
    - SABATIER_CH4_KG_PER_CO2_KG
-- Exothermic heat released per kilogram of CO2 converted, mirroring
-- the host's StationSabatierReactionHeatKJPerKgCarbonDioxide (#405):
-- the chemical energy the hydrogen carried that the methane does not
-- retain, derived from the declared heating values.
local SABATIER_HEAT_KJ_PER_CO2_KG = SABATIER_H2_KG_PER_CO2_KG
    * ELECTROLYSIS_H2_CHEMICAL_KJ_PER_KG - SABATIER_CH4_KG_PER_CO2_KG * METHANE_LHV_KJ_PER_KG
-- Stoichiometric masses of methane processing CH4 -> C + 2 H2 per
-- kilogram of CH4 processed, mirroring the host's
-- StationMethaneProcessing* constants (#405): solid carbon is the
-- mass-balance remainder.
local METHANE_PROCESSING_H2_KG_PER_CH4_KG = 2.0 * MOLAR_MASS_HYDROGEN / MOLAR_MASS_METHANE
local METHANE_PROCESSING_C_KG_PER_CH4_KG = 1.0 - METHANE_PROCESSING_H2_KG_PER_CH4_KG
-- Endothermic reaction heat absorbed per kilogram of methane processed,
-- mirroring the host's StationMethaneProcessingReactionHeatKJPerKgMethane
-- (#405): the authored dose must at least cover this demand.
local METHANE_PROCESSING_HEAT_KJ_PER_CH4_KG = 74.8 / MOLAR_MASS_METHANE
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

-- Inverse of toKg: a magnitude expressed in kilograms rendered in a
-- store's own declared mass unit (#402, where the recovery rates are
-- authored in kg/s against stores that may be grams).
local function fromKg(unit, kg)
    if unit == 'g' then return kg * 1000.0 end
    return kg
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
    reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews,
    waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers,
    wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors)
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
    assert(type(crews) == "table", "crew descriptors missing")
    assert(type(waterCollectors) == "table", "water collector descriptors missing")
    assert(type(waterTreatments) == "table", "water treatment descriptors missing")
    assert(type(electrolyzers) == "table", "electrolyzer descriptors missing")
    assert(type(co2Captures) == "table", "CO2 capture descriptors missing")
    assert(type(fuelCells) == "table", "fuel cell descriptors missing")
    assert(type(thermalBuffers) == "table", "thermal buffer descriptors missing")
    assert(type(wasteEjections) == "table", "waste ejection descriptors missing")
    assert(type(sabatierReactors) == "table", "Sabatier reactor descriptors missing")
    assert(type(methaneProcessors) == "table", "methane processor descriptors missing")
    assert(type(microbialProcessors) == "table", "microbial processor descriptors missing")
    local object = {
        ticks = 0, stores = {}, transfers = {}, order = {},
        conversions = {}, conversion_order = {},
        starting_totals = {}, unit_loss = {}, unit_delivered = {},
        unit_collected = {},
        starting_constituents = {}, constituent_loss = {},
        constituent_delivered = {},
        solar = {}, loads = {}, load_order = {}, batteries = {},
        deliveries = {}, reservoirs = {}, radiators = {}, coolant_loops = {},
        chillers = {}, generators = {}, atmospheres = {}, airlock_cycles = {},
        crews = {},
        water_collectors = {}, water_treatments = {}, water_treatment_order = {},
        electrolyzers = {}, electrolyzer_order = {},
        co2_captures = {}, co2_capture_order = {},
        sabatier_reactors = {}, sabatier_order = {},
        methane_processors = {}, methane_processor_order = {},
        microbial_processors = {}, microbial_order = {},
        fuel_cells = {}, thermal_buffers = {},
        waste_ejections = {},
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
            buffers = {},
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
            collectorHeat = 0,
            collectorKJ = 0,
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
    -- Thermal buffers (#398): a finite store of thermal energy that
    -- absorbs the cooling deficit its host reservoir cannot reject and
    -- returns the stored heat only when real rejection capacity becomes
    -- available. The buffer's state is stored energy, never temperature:
    -- capacity is the energy storable across the declared usable band,
    -- and the buffer's temperature is derived from the stored fraction
    -- across that band (a sensible-storage model; a narrow band with a
    -- large capacity approximates an isothermal phase-change material).
    -- Stored energy is distinct from the host's instantaneous
    -- temperature. Access is configured, never free: a 'circulation'
    -- buffer is a powered-only store reachable only while its declared
    -- coolant loop - coupled to the same host reservoir - is actually
    -- circulating, so a blackout cannot absorb through it for free; a
    -- 'passive' buffer is reachable at any time through its declared
    -- transfer conductance, rate-limited by the temperature difference
    -- across the interface. Charging is capped by the host's
    -- instantaneous cooling deficit so the buffer absorbs what the
    -- rejection path cannot take and never overcools the reservoir;
    -- discharge is capped by the rejection surplus so recovery consumes
    -- real rejection capacity and cannot reset the store instantly.
    -- Both directions are thermally honest: heat only moves from the
    -- hotter side to the colder one.
    for index = 1, #thermalBuffers do
        local buffer = thermalBuffers[index]
        assert(type(buffer) == 'table', 'thermal buffer descriptor must be a table')
        assert(type(buffer.id) == 'string' and #buffer.id > 0, 'thermal buffer needs an id')
        local host = reservoir_by_id[buffer.reservoir]
        assert(host ~= nil, 'thermal buffer host reservoir is unknown')
        assert(buffer.accessMode == 'circulation' or buffer.accessMode == 'passive',
            'thermal buffer needs a declared access mode (circulation or passive)')
        local gateLoop = nil
        if buffer.accessMode == 'circulation' then
            gateLoop = coolant_loop_by_id[buffer.circulationLoop]
            assert(gateLoop ~= nil,
                'circulation-access thermal buffer needs a declared circulation loop')
            assert(gateLoop.reservoir == host,
                'thermal buffer circulation loop must be coupled to the same host reservoir')
        else
            assert(type(buffer.transferConductanceKWPerK) == 'number'
                and buffer.transferConductanceKWPerK == buffer.transferConductanceKWPerK
                and buffer.transferConductanceKWPerK > 0
                and buffer.transferConductanceKWPerK < math.huge,
                'passive thermal buffer transfer conductance must be finite and positive')
        end
        assert(type(buffer.capacityKJ) == 'number' and buffer.capacityKJ == buffer.capacityKJ
            and buffer.capacityKJ > 0 and buffer.capacityKJ < math.huge,
            'thermal buffer capacity must be finite and positive')
        assert(type(buffer.startingEnergyKJ) == 'number'
            and buffer.startingEnergyKJ == buffer.startingEnergyKJ
            and buffer.startingEnergyKJ >= 0
            and buffer.startingEnergyKJ <= buffer.capacityKJ,
            'thermal buffer starting energy must be finite and within capacity')
        assert(type(buffer.minUsableTemperatureC) == 'number'
            and buffer.minUsableTemperatureC == buffer.minUsableTemperatureC
            and buffer.minUsableTemperatureC < math.huge
            and buffer.minUsableTemperatureC > -KELVIN_OFFSET,
            'thermal buffer minimum usable temperature must be finite and above absolute zero')
        assert(type(buffer.maxUsableTemperatureC) == 'number'
            and buffer.maxUsableTemperatureC == buffer.maxUsableTemperatureC
            and buffer.maxUsableTemperatureC < math.huge
            and buffer.maxUsableTemperatureC > -KELVIN_OFFSET,
            'thermal buffer maximum usable temperature must be finite and above absolute zero')
        assert(buffer.minUsableTemperatureC < buffer.maxUsableTemperatureC,
            'thermal buffer usable temperature band needs the minimum below the maximum')
        assert(type(buffer.maxChargeKW) == 'number' and buffer.maxChargeKW == buffer.maxChargeKW
            and buffer.maxChargeKW >= 0 and buffer.maxChargeKW < math.huge,
            'thermal buffer charge rate must be finite and non-negative')
        assert(type(buffer.maxDischargeKW) == 'number'
            and buffer.maxDischargeKW == buffer.maxDischargeKW
            and buffer.maxDischargeKW >= 0 and buffer.maxDischargeKW < math.huge,
            'thermal buffer discharge rate must be finite and non-negative')
        assert(buffer.maxChargeKW > 0 or buffer.maxDischargeKW > 0,
            'a thermal buffer that can neither charge nor discharge is not a buffer')
        object.thermal_buffers[index] = {
            id = buffer.id,
            reservoir = host,
            accessMode = buffer.accessMode,
            gateLoop = gateLoop,
            capacity = buffer.capacityKJ,
            stored = buffer.startingEnergyKJ,
            startingEnergy = buffer.startingEnergyKJ,
            minUsable = buffer.minUsableTemperatureC,
            maxUsable = buffer.maxUsableTemperatureC,
            maxCharge = buffer.maxChargeKW,
            maxDischarge = buffer.maxDischargeKW,
            conductance = buffer.transferConductanceKWPerK or 0,
            accessible = false,
            lastCharge = 0,
            lastDischarge = 0,
            full = buffer.startingEnergyKJ >= buffer.capacityKJ,
            depleted = buffer.startingEnergyKJ <= 0,
            lastLimiter = '',
            chargedKJ = 0,
            dischargedKJ = 0,
        }
        host.buffers[#host.buffers + 1] = object.thermal_buffers[index]
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
    -- Terminal-waste ejection schedules (#403): an authored external
    -- removal of a declared quantity from one waste store at declared
    -- simulation times, once or recurring. Only disposal-ready
    -- (qualified) material is eligible; held (unqualified/untreated)
    -- waste is never ejected, and a store holding nothing eligible
    -- simply ejects nothing — a shortfall is a valid outcome, never a
    -- failure. What is ejected is booked as an external loss on the
    -- same per-unit and per-constituent boundary the host ledger
    -- closes over. The schedule rule and the deterministic scenario
    -- ordering are the ones resupply deliveries established; at equal
    -- time and priority, arrivals run before ejections.
    for index = 1, #wasteEjections do
        local ejection = wasteEjections[index]
        assert(type(ejection) == 'table', 'waste ejection descriptor must be a table')
        assert(type(ejection.id) == 'string' and #ejection.id > 0, 'waste ejection needs an id')
        local target = by_id[ejection.store]
        assert(target ~= nil, 'waste ejection target store is unknown')
        assert(ejection.unit == target.unit,
            'waste ejection unit must match its target store unit')
        assert(type(ejection.quantity) == 'number'
            and ejection.quantity == ejection.quantity and ejection.quantity > 0
            and ejection.quantity < math.huge,
            'waste ejection quantity must be finite and positive')
        assert(type(ejection.firstSeconds) == 'number'
            and ejection.firstSeconds == ejection.firstSeconds
            and ejection.firstSeconds >= 0 and ejection.firstSeconds < math.huge,
            'waste ejection first occurrence must be finite and non-negative')
        assert(type(ejection.recurrenceSeconds) == 'number'
            and ejection.recurrenceSeconds == ejection.recurrenceSeconds
            and ejection.recurrenceSeconds >= 0 and ejection.recurrenceSeconds < math.huge,
            'waste ejection recurrence must be finite and non-negative')
        assert(type(ejection.priority) == 'number'
            and ejection.priority == ejection.priority
            and ejection.priority == math.tointeger(ejection.priority),
            'waste ejection priority must be a finite integer')
        object.waste_ejections[index] = {
            id = ejection.id,
            store = target,
            unit = ejection.unit,
            quantity = ejection.quantity,
            firstSeconds = ejection.firstSeconds,
            recurrenceSeconds = ejection.recurrenceSeconds,
            priority = ejection.priority,
            fired = 0,
            lastEjected = 0,
            lastUnmet = 0,
            ejectedTotal = 0,
            unmetTotal = 0,
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
    -- Ideal-gas partial pressure and Magnus saturation formulas shared
    -- by the constructor audit and the per-tick integration; they
    -- mirror the host formulas exactly (#400, #565).
    local function partialPressureKPa(massKg, molarMassKgPerMol, volumeM3, temperatureC)
        return massKg / molarMassKgPerMol * GAS_CONSTANT * (temperatureC + KELVIN_OFFSET)
            / volumeM3 / 1000.0
    end
    local function saturationVapourPressureKPa(temperatureC)
        -- #565: the Magnus expression is singular at -243.04 C and its
        -- exponent overflows to infinity below that point. The
        -- documented supported domain is strictly above the singular
        -- temperature; at or below it the model carries a finite valid
        -- continuation - air that cold holds no vapour, which is
        -- exactly where the in-domain curve is already heading -
        -- instead of publishing an overflow as a shortage.
        if temperatureC <= -243.04 then return 0.0 end
        return 0.61094 * math.exp(17.625 * temperatureC / (temperatureC + 243.04))
    end
    local function saturatedVapourMassKg(saturationKPa, volumeM3, temperatureC)
        return saturationKPa * 1000.0 * volumeM3 * MOLAR_MASS_WATER
            / (GAS_CONSTANT * (temperatureC + KELVIN_OFFSET))
    end
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
        -- #565: mirror of the host authoring refusal. Every authored
        -- field being finite does not make the starting state
        -- publishable: the ideal-gas division by a tiny volume can
        -- overflow, and a compartment whose starting inventory already
        -- derives a non-finite pressure is an inconsistent authored
        -- input, refused rather than published as a healthy state.
        local startingOxygenKPa = partialPressureKPa(atmosphere.startingOxygenKg,
            MOLAR_MASS_OXYGEN, atmosphere.volumeM3, atmosphere.startingTemperatureC)
        local startingNitrogenKPa = partialPressureKPa(atmosphere.startingNitrogenKg,
            MOLAR_MASS_NITROGEN, atmosphere.volumeM3, atmosphere.startingTemperatureC)
        local startingCarbonDioxideKPa = partialPressureKPa(atmosphere.startingCarbonDioxideKg,
            MOLAR_MASS_CARBON_DIOXIDE, atmosphere.volumeM3, atmosphere.startingTemperatureC)
        local startingVapourKPa = partialPressureKPa(atmosphere.startingWaterVapourKg,
            MOLAR_MASS_WATER, atmosphere.volumeM3, atmosphere.startingTemperatureC)
        assert(isFinite(startingOxygenKPa) and isFinite(startingNitrogenKPa)
            and isFinite(startingCarbonDioxideKPa) and isFinite(startingVapourKPa)
            and isFinite(startingOxygenKPa + startingNitrogenKPa
                + startingCarbonDioxideKPa + startingVapourKPa),
            'atmosphere starting inventory derives a non-finite pressure in its volume and temperature')
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
            collected = 0,
            occupantHeatKJ = 0, latentHeatKJ = 0, exchangedKJ = 0,
            crewHeatKJ = 0,
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
    -- Crew contracts (#401): an authored population with an activity
    -- profile whose metabolism draws food and water from compound mass
    -- stores and oxygen from one compartment atmosphere, returning CO2,
    -- moisture and metabolic heat to that compartment and urine and
    -- organic waste to their own finite compound stores. The crew is a
    -- configured population, never derived from World Agents. The
    -- sandbox mirrors the host's uniform-scale metabolism and re-audits
    -- the authored closures: emitted mass may not exceed consumed mass,
    -- every returned constituent (urine, organic waste and respired
    -- moisture) must be funded by the consumed food and water, and the
    -- declared metabolic heat may not exceed the consumed food energy
    -- minus the energy still bound in the waste streams. A crew that
    -- would create mass, constituents or energy out of nothing is
    -- refused at construction, contained like any other bad descriptor.
    assert(#crews <= 8, 'station crews are limited to 8 entries')
    for index = 1, #crews do
        local crew = crews[index]
        assert(type(crew) == 'table', 'crew descriptor must be a table')
        assert(type(crew.id) == 'string' and #crew.id > 0, 'crew needs an id')
        local compartment = atmosphere_by_id[crew.atmosphere]
        assert(compartment ~= nil, 'crew atmosphere compartment is unknown')
        assert(type(crew.crewCount) == 'number'
            and crew.crewCount == math.tointeger(crew.crewCount)
            and crew.crewCount >= 0, 'crew count must be a non-negative integer')
        local activity = crew.activity
            or { constantLevel = 1, periodSeconds = 0, segments = {} }
        checkProfile(activity, 'crew ' .. crew.id .. ' activity profile')
        local rateFields = {
            { crew.foodRate, 'food rate' }, { crew.waterRate, 'water rate' },
            { crew.oxygenRateKgPerS, 'oxygen rate' }, { crew.co2RateKgPerS, 'CO2 rate' },
            { crew.moistureRateKgPerS, 'moisture rate' }, { crew.urineRate, 'urine rate' },
            { crew.organicWasteRate, 'organic waste rate' },
            { crew.metabolicHeatKW, 'metabolic heat' },
        }
        for _, entry in ipairs(rateFields) do
            assert(type(entry[1]) == 'number' and entry[1] == entry[1]
                and entry[1] >= 0 and entry[1] < math.huge,
                'crew ' .. entry[2] .. ' must be finite and non-negative')
        end
        local streamRoles = { 'food', 'water', 'urine', 'organic waste' }
        local streamIds = { crew.foodStore, crew.waterStore, crew.urineStore,
            crew.organicWasteStore }
        local streams = {}
        for stream = 1, 4 do
            local store = by_id[streamIds[stream]]
            assert(store ~= nil, 'crew ' .. streamRoles[stream] .. ' store is unknown')
            assert(store.composition ~= nil,
                'crew ' .. streamRoles[stream] .. ' store must declare a composition')
            streams[stream] = store
        end
        local food, water, urine, waste = streams[1], streams[2], streams[3], streams[4]
        local foodKg = toKg(food.unit, crew.foodRate)
        local waterKg = toKg(water.unit, crew.waterRate)
        local urineKg = toKg(urine.unit, crew.urineRate)
        local wasteKg = toKg(waste.unit, crew.organicWasteRate)
        local consumedKg = foodKg + waterKg + crew.oxygenRateKgPerS
        local emittedKg = urineKg + wasteKg + crew.moistureRateKgPerS + crew.co2RateKgPerS
        assert(emittedKg <= consumedKg + BALANCE_TOLERANCE * math.max(1, consumedKg),
            'crew ' .. crew.id
                .. ' declared returns exceed declared consumption: metabolism may not create mass')
        local funded = {}
        addConstituents(funded, food.composition, foodKg, 1)
        addConstituents(funded, water.composition, waterKg, 1)
        local returned = {}
        addConstituents(returned, urine.composition, urineKg, 1)
        addConstituents(returned, waste.composition, wasteKg, 1)
        returned.water = (returned.water or 0) + crew.moistureRateKgPerS
        for constituent, amount in pairs(returned) do
            local available = funded[constituent] or 0
            assert(amount <= available + BALANCE_TOLERANCE * math.max(1, math.abs(available)),
                'crew ' .. crew.id .. " returns more '" .. constituent
                    .. "' than the consumed food and water carry: the element balance may not be created")
        end
        local function streamEnergy(composition, wetKg)
            return wetKg * (1 - composition.moisture) * composition.energyKJPerKgDry
        end
        local releasedKJPerS = streamEnergy(food.composition, foodKg)
            - streamEnergy(urine.composition, urineKg)
            - streamEnergy(waste.composition, wasteKg)
        assert(crew.metabolicHeatKW <= releasedKJPerS
            + BALANCE_TOLERANCE * math.max(1, math.abs(releasedKJPerS)),
            'crew ' .. crew.id .. ' metabolic heat exceeds the consumed food energy minus the energy still bound in the waste streams')
        object.crews[index] = {
            id = crew.id,
            compartment = compartment,
            crewCount = crew.crewCount,
            activity = readProfile(activity),
            food = food, water = water, urine = urine, organicWaste = waste,
            foodRate = crew.foodRate, waterRate = crew.waterRate,
            oxygenRate = crew.oxygenRateKgPerS, co2Rate = crew.co2RateKgPerS,
            moistureRate = crew.moistureRateKgPerS, urineRate = crew.urineRate,
            organicWasteRate = crew.organicWasteRate, heatRate = crew.metabolicHeatKW,
            lastActivity = 0, lastScale = 0, lastLimiter = '',
            foodConsumed = 0, waterConsumed = 0, oxygenConsumed = 0,
            co2Produced = 0, moistureProduced = 0, urineProduced = 0,
            organicWasteProduced = 0, heatKJ = 0, retainedKg = 0,
            lastUnmet = false,
        }
    end
    -- Water recovery contracts (#402). A collector condenses vapour out of
    -- one compartment onto a coolant-loop cold surface; a treatment turns
    -- raw water into treated water plus a retained concentrate. The sandbox
    -- mirrors the collection gate, the recovery split and the quality rule
    -- and re-audits its own accounting: a collector may never take more
    -- than the compartment held above its humidity target, a treatment's
    -- processed mass must leave as treated water plus concentrate and
    -- nothing else, and every treated kilogram is either qualified or
    -- held. A descriptor that would create water, or mix hazard classes in
    -- one store, is refused at construction like any other bad descriptor.
    assert(#waterCollectors <= 8, 'station water collectors are limited to 8 entries')
    assert(#waterTreatments <= 8, 'station water treatments are limited to 8 entries')
    local waterStoreClass = {}
    local function declareWaterStore(storeId, role, hazardClass, allowCompound)
        local store = by_id[storeId]
        assert(store ~= nil, 'water ' .. role .. " store '" .. tostring(storeId) .. "' is unknown")
        assert(store.unit == 'g' or store.unit == 'kg',
            'water ' .. role .. " store '" .. store.id
                .. "' must be a mass unit; water recovery is mass-denominated")
        assert(allowCompound or store.composition == nil,
            'water ' .. role .. " store '" .. store.id
                .. "' must be a plain mass store: recovered water carries no dry constituents")
        local existing = waterStoreClass[store.id]
        if existing == nil then
            waterStoreClass[store.id] = hazardClass
        else
            assert(existing == hazardClass,
                'water ' .. role .. " store '" .. store.id .. "' is shared between "
                    .. existing .. '-class and ' .. hazardClass
                    .. '-class water processes: hazardous and normal water streams may not share a store')
        end
        return store
    end
    for index = 1, #waterCollectors do
        local collector = waterCollectors[index]
        assert(type(collector) == 'table', 'water collector descriptor must be a table')
        assert(type(collector.id) == 'string' and #collector.id > 0, 'water collector needs an id')
        assert(collector.hazardClass == 'normal' or collector.hazardClass == 'hazardous',
            'water collector needs a declared hazard class (normal or hazardous)')
        assert(type(collector.maxRateKgPerS) == 'number'
            and collector.maxRateKgPerS == collector.maxRateKgPerS
            and collector.maxRateKgPerS >= 0 and collector.maxRateKgPerS < math.huge,
            'water collector maximum rate must be finite and non-negative')
        assert(type(collector.targetRelativeHumidity) == 'number'
            and collector.targetRelativeHumidity == collector.targetRelativeHumidity
            and collector.targetRelativeHumidity >= 0 and collector.targetRelativeHumidity <= 1,
            'water collector target relative humidity must be finite and from 0 to 1')
        local compartment = atmosphere_by_id[collector.atmosphere]
        assert(compartment ~= nil, 'water collector atmosphere compartment is unknown')
        assert(compartment.hazardClass == collector.hazardClass,
            'water collector hazard class must match the compartment it condenses from: condensate inherits the hazard of the air')
        local loop = coolant_loop_by_id[collector.coolingLoop]
        assert(loop ~= nil, 'water collector cooling loop is unknown')
        assert(loop.reservoir.thermalClass == collector.thermalClass,
            'water collector thermal class must match its coolant loop host reservoir class')
        local destination = declareWaterStore(collector.destStore, 'raw water destination',
            collector.hazardClass, false)
        object.water_collectors[index] = {
            id = collector.id,
            hazardClass = collector.hazardClass,
            compartment = compartment,
            loop = loop,
            dest = destination,
            maxRateKgPerS = collector.maxRateKgPerS,
            targetRelativeHumidity = collector.targetRelativeHumidity,
            lastRequested = 0, lastCondensable = 0, lastCollected = 0,
            lastLatentHeatKW = 0, lastCirculation = 0,
            coldSurfaceAvailable = false, lastBlockReason = '',
            collectedTotal = 0, latentHeatKJ = 0,
        }
    end
    for index = 1, #waterTreatments do
        local treatment = waterTreatments[index]
        assert(type(treatment) == 'table', 'water treatment descriptor must be a table')
        assert(type(treatment.id) == 'string' and #treatment.id > 0, 'water treatment needs an id')
        assert(treatment.hazardClass == 'normal' or treatment.hazardClass == 'hazardous',
            'water treatment needs a declared hazard class (normal or hazardous)')
        assert(type(treatment.throughputKgPerS) == 'number'
            and treatment.throughputKgPerS == treatment.throughputKgPerS
            and treatment.throughputKgPerS >= 0 and treatment.throughputKgPerS < math.huge,
            'water treatment throughput must be finite and non-negative')
        assert(type(treatment.recoveryRatio) == 'number'
            and treatment.recoveryRatio == treatment.recoveryRatio
            and treatment.recoveryRatio > 0 and treatment.recoveryRatio <= 1,
            'water treatment recovery ratio must be finite and above zero up to 1')
        assert(type(treatment.specificEnergyKWhPerKg) == 'number'
            and treatment.specificEnergyKWhPerKg == treatment.specificEnergyKWhPerKg
            and treatment.specificEnergyKWhPerKg >= 0 and treatment.specificEnergyKWhPerKg < math.huge,
            'water treatment specific energy must be finite and non-negative')
        assert(type(treatment.consumablePerKg) == 'number'
            and treatment.consumablePerKg == treatment.consumablePerKg
            and treatment.consumablePerKg >= 0 and treatment.consumablePerKg < math.huge,
            'water treatment consumable per kg must be finite and non-negative')
        assert(type(treatment.priority) == 'number'
            and treatment.priority == math.tointeger(treatment.priority),
            'water treatment priority must be a finite integer')
        local streamRoles = {'source', 'treated', 'concentrate'}
        local streamIds = {treatment.sourceStore, treatment.treatedStore,
            treatment.concentrateStore}
        local streams = {}
        for stream = 1, 3 do
            for other = stream + 1, 3 do
                assert(streamIds[stream] ~= streamIds[other],
                    'water treatment ' .. streamRoles[stream] .. ' and '
                        .. streamRoles[other] .. ' stores must be distinct')
            end
            streams[stream] = declareWaterStore(streamIds[stream], streamRoles[stream],
                treatment.hazardClass, false)
        end
        local consumable = declareWaterStore(treatment.consumableStore, 'consumable',
            treatment.hazardClass, true)
        object.water_treatments[index] = {
            id = treatment.id,
            hazardClass = treatment.hazardClass,
            source = streams[1], treated = streams[2], concentrate = streams[3],
            consumable = consumable,
            throughputKgPerS = treatment.throughputKgPerS,
            recoveryRatio = treatment.recoveryRatio,
            specificEnergyKWhPerKg = treatment.specificEnergyKWhPerKg,
            consumablePerKg = treatment.consumablePerKg,
            priority = treatment.priority,
            lastScale = 0, lastLimiter = '',
            lastProcessed = 0, lastTreated = 0, lastConcentrate = 0,
            lastPowerDemand = 0, lastPowerSupplied = 0,
            lastConsumableRequested = 0, lastConsumableUsed = 0,
            qualityAccepted = false, lastHoldReason = '',
            processedTotal = 0, treatedTotal = 0, acceptedTotal = 0,
            heldTotal = 0, concentrateTotal = 0,
            energySuppliedKJ = 0, energyOwedKJ = 0, consumablesUsedTotal = 0,
        }
    end
    -- Treatments compete for the same power pool and the same stores, so
    -- they run in the declared deterministic order: priority descending,
    -- declaration index ascending.
    for index = 1, #object.water_treatments do object.water_treatment_order[index] = index end
    table.sort(object.water_treatment_order, function(left, right)
        local a, b = object.water_treatments[left], object.water_treatments[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    -- The oxygen production and CO2 capture contracts (#404). The
    -- sandbox mirrors the host's refusals: a cell whose electrical dose
    -- would not cover the chemical energy its products retain, a cell
    -- without exactly one oxygen destination, a compound gas store, a
    -- hazard-class mismatch anywhere in the chain, or a capture machine
    -- that declares a consumable it cannot hold are all refused at
    -- construction like any other bad descriptor.
    assert(#electrolyzers <= 8, 'station electrolyzers are limited to 8 entries')
    assert(#co2Captures <= 8, 'station CO2 capture machines are limited to 8 entries')
    local gasStoreClass = {}
    local function declareGasStore(storeId, role, hazardClass, allowCompound)
        local store = by_id[storeId]
        assert(store ~= nil, 'gas ' .. role .. " store '" .. tostring(storeId) .. "' is unknown")
        assert(store.unit == 'g' or store.unit == 'kg',
            'gas ' .. role .. " store '" .. store.id
                .. "' must be a mass unit; gas handling is mass-denominated")
        assert(allowCompound or store.composition == nil,
            'gas ' .. role .. " store '" .. store.id
                .. "' must be a plain mass store: handled gas streams carry no dry constituents")
        local existing = gasStoreClass[store.id]
        if existing == nil then
            gasStoreClass[store.id] = hazardClass
        else
            assert(existing == hazardClass,
                'gas ' .. role .. " store '" .. store.id .. "' is shared between "
                    .. existing .. '-class and ' .. hazardClass
                    .. '-class gas processes: hazardous and normal gas streams may not share a store')
        end
        return store
    end
    for index = 1, #electrolyzers do
        local cell = electrolyzers[index]
        assert(type(cell) == 'table', 'electrolyzer descriptor must be a table')
        assert(type(cell.id) == 'string' and #cell.id > 0, 'electrolyzer needs an id')
        assert(cell.hazardClass == 'normal' or cell.hazardClass == 'hazardous',
            'electrolyzer needs a declared hazard class (normal or hazardous)')
        assert(type(cell.thermalClass) == 'string'
            and (cell.thermalClass == 'low' or cell.thermalClass == 'moderate'
                or cell.thermalClass == 'warm' or cell.thermalClass == 'generation'),
            'electrolyzer needs a declared thermal service class')
        assert(type(cell.rateKgPerS) == 'number'
            and cell.rateKgPerS == cell.rateKgPerS
            and cell.rateKgPerS >= 0 and cell.rateKgPerS < math.huge,
            'electrolyzer rated split must be finite and non-negative')
        assert(type(cell.specificEnergyKWhPerKgWater) == 'number'
            and cell.specificEnergyKWhPerKgWater == cell.specificEnergyKWhPerKgWater
            and cell.specificEnergyKWhPerKgWater * 3600.0
                >= ELECTROLYSIS_CHEMICAL_KJ_PER_WATER_KG - 1e-9,
            'electrolyzer specific energy is below the chemical energy the produced hydrogen retains')
        assert(type(cell.maxSinkTemperatureC) == 'number'
            and cell.maxSinkTemperatureC == cell.maxSinkTemperatureC
            and cell.maxSinkTemperatureC > -273.15,
            'electrolyzer maximum sink temperature must be finite and above absolute zero')
        assert((cell.oxygenStore == nil or #cell.oxygenStore == 0)
            ~= (cell.atmosphere == nil or #cell.atmosphere == 0),
            'electrolyzer must configure exactly one oxygen destination: a gas store or a compartment')
        local feed = declareWaterStore(cell.waterStore, 'feed water', cell.hazardClass, false)
        local oxygen = nil
        if cell.oxygenStore ~= nil and #cell.oxygenStore > 0 then
            oxygen = declareGasStore(cell.oxygenStore, 'oxygen', cell.hazardClass, false)
        end
        local hydrogen = declareGasStore(cell.hydrogenStore, 'hydrogen', cell.hazardClass, false)
        local compartment = nil
        if cell.atmosphere ~= nil and #cell.atmosphere > 0 then
            compartment = atmosphere_by_id[cell.atmosphere]
            assert(compartment ~= nil, 'electrolyzer oxygen delivery compartment is unknown')
            assert(compartment.hazardClass == cell.hazardClass,
                'electrolyzer hazard class must match the delivery compartment: oxygen may only be delivered to a compartment of the cell\'s own class')
        end
        local sink = reservoir_by_id[cell.heatSink]
        assert(sink ~= nil, 'electrolyzer heat sink reservoir is unknown')
        assert(sink.thermalClass == cell.thermalClass,
            'electrolyzer thermal class must match its heat sink reservoir class')
        object.electrolyzers[index] = {
            id = cell.id,
            hazardClass = cell.hazardClass,
            feed = feed, oxygen = oxygen, hydrogen = hydrogen,
            compartment = compartment, sink = sink,
            rateKgPerS = cell.rateKgPerS,
            specificEnergyKWhPerKgWater = cell.specificEnergyKWhPerKgWater,
            maxSinkTemperatureC = cell.maxSinkTemperatureC,
            priority = cell.priority,
            lastScale = 0, lastLimiter = '',
            lastRequested = 0, lastSplit = 0, lastOxygen = 0, lastHydrogen = 0,
            lastPowerDemand = 0, lastPowerSupplied = 0, lastUnmetProduction = 0,
            coolingAvailable = false, lastBlockReason = '',
            splitTotal = 0, oxygenTotal = 0, hydrogenTotal = 0,
            energySuppliedKJ = 0, chemicalEnergyKJ = 0, heatRoutedKJ = 0,
        }
    end
    for index = 1, #co2Captures do
        local capture = co2Captures[index]
        assert(type(capture) == 'table', 'CO2 capture descriptor must be a table')
        assert(type(capture.id) == 'string' and #capture.id > 0, 'CO2 capture needs an id')
        assert(capture.hazardClass == 'normal' or capture.hazardClass == 'hazardous',
            'CO2 capture needs a declared hazard class (normal or hazardous)')
        assert(type(capture.maxRateKgPerS) == 'number'
            and capture.maxRateKgPerS == capture.maxRateKgPerS
            and capture.maxRateKgPerS >= 0 and capture.maxRateKgPerS < math.huge,
            'CO2 capture capacity must be finite and non-negative')
        assert(type(capture.specificEnergyKWhPerKg) == 'number'
            and capture.specificEnergyKWhPerKg == capture.specificEnergyKWhPerKg
            and capture.specificEnergyKWhPerKg >= 0 and capture.specificEnergyKWhPerKg < math.huge,
            'CO2 capture specific energy must be finite and non-negative')
        assert(type(capture.consumablePerKg) == 'number'
            and capture.consumablePerKg == capture.consumablePerKg
            and capture.consumablePerKg >= 0 and capture.consumablePerKg < math.huge,
            'CO2 capture consumable per kg must be finite and non-negative')
        assert(capture.consumablePerKg <= 0
            or (capture.consumableStore ~= nil and #capture.consumableStore > 0),
            'CO2 capture declares a consumable demand but names no consumable store')
        assert(type(capture.priority) == 'number'
            and capture.priority == math.tointeger(capture.priority),
            'CO2 capture priority must be a finite integer')
        local compartment = atmosphere_by_id[capture.atmosphere]
        assert(compartment ~= nil, 'CO2 capture atmosphere compartment is unknown')
        assert(compartment.hazardClass == capture.hazardClass,
            'CO2 capture hazard class must match the compartment it scrubs')
        local destination = declareGasStore(capture.captureStore, 'capture',
            capture.hazardClass, false)
        local consumable = nil
        if capture.consumableStore ~= nil and #capture.consumableStore > 0 then
            consumable = declareGasStore(capture.consumableStore, 'consumable',
                capture.hazardClass, true)
        end
        object.co2_captures[index] = {
            id = capture.id,
            hazardClass = capture.hazardClass,
            compartment = compartment, destination = destination, consumable = consumable,
            maxRateKgPerS = capture.maxRateKgPerS,
            specificEnergyKWhPerKg = capture.specificEnergyKWhPerKg,
            consumablePerKg = capture.consumablePerKg,
            priority = capture.priority,
            lastScale = 0, lastLimiter = '',
            lastRequested = 0, lastCaptured = 0,
            lastPowerDemand = 0, lastPowerSupplied = 0,
            lastConsumableRequested = 0, lastConsumableUsed = 0,
            lastBlockReason = '',
            capturedTotal = 0, energySuppliedKJ = 0, consumablesUsedTotal = 0,
        }
    end
    -- Fuel-cell backup contracts (#406). The cell burns its two dedicated
    -- reactant stores for dispatchable electrical power; every axis of
    -- finiteness is enforced at runtime, and the whole chain - both
    -- reactant stores and the product water store - carries the cell's
    -- own hazard class.
    for index = 1, #fuelCells do
        local cell = fuelCells[index]
        assert(type(cell) == 'table', 'fuel cell descriptor must be a table')
        assert(type(cell.id) == 'string' and #cell.id > 0, 'fuel cell needs an id')
        assert(cell.hazardClass == 'normal' or cell.hazardClass == 'hazardous',
            'fuel cell needs a declared hazard class (normal or hazardous)')
        assert(type(cell.thermalClass) == 'string'
            and (cell.thermalClass == 'low' or cell.thermalClass == 'moderate'
                or cell.thermalClass == 'warm' or cell.thermalClass == 'generation'),
            'fuel cell needs a declared thermal service class')
        assert(type(cell.ratedOutputKW) == 'number'
            and cell.ratedOutputKW == cell.ratedOutputKW
            and cell.ratedOutputKW >= 0 and cell.ratedOutputKW < math.huge,
            'fuel cell rated output must be finite and non-negative')
        assert(type(cell.efficiency) == 'number'
            and cell.efficiency == cell.efficiency
            and cell.efficiency > 0 and cell.efficiency <= 1,
            'fuel cell efficiency must be greater than 0 and at most 1: a cell cannot deliver more electrical energy than the reactants chemical energy')
        assert(type(cell.commandedOutputKW) == 'number'
            and cell.commandedOutputKW == cell.commandedOutputKW
            and cell.commandedOutputKW >= 0 and cell.commandedOutputKW < math.huge
            and cell.commandedOutputKW <= cell.ratedOutputKW,
            'fuel cell commanded output must be finite and within the installed rating')
        assert(type(cell.maxSinkTemperatureC) == 'number'
            and cell.maxSinkTemperatureC == cell.maxSinkTemperatureC
            and cell.maxSinkTemperatureC > -KELVIN_OFFSET,
            'fuel cell maximum sink temperature must be finite and above absolute zero')
        local hydrogen = declareGasStore(cell.hydrogenStore, 'hydrogen reactant',
            cell.hazardClass, false)
        local oxygen = declareGasStore(cell.oxygenStore, 'oxygen reactant',
            cell.hazardClass, false)
        local water = declareWaterStore(cell.waterStore, 'product water',
            cell.hazardClass, false)
        local sink = reservoir_by_id[cell.heatSink]
        assert(sink ~= nil, 'fuel cell heat sink reservoir is unknown')
        assert(sink.thermalClass == cell.thermalClass,
            'fuel cell thermal class must match its heat sink reservoir class')
        object.fuel_cells[index] = {
            id = cell.id,
            hazardClass = cell.hazardClass,
            hydrogen = hydrogen, oxygen = oxygen, water = water, sink = sink,
            ratedOutputKW = cell.ratedOutputKW,
            efficiency = cell.efficiency,
            commandedOutputKW = cell.commandedOutputKW,
            maxSinkTemperatureC = cell.maxSinkTemperatureC,
            lastOutputKW = 0, lastHydrogenKg = 0, lastOxygenKg = 0,
            lastWaterKg = 0, lastWasteHeatKW = 0, lastUnmetOutputKW = 0,
            coolingAvailable = false, lastLimiter = '', lastBlockReason = '',
            hydrogenConsumedKg = 0, oxygenConsumedKg = 0, waterProducedKg = 0,
            chemicalEnergyKJ = 0, electricalKJ = 0, wasteHeatKJ = 0,
            enduranceSeconds = 0,
        }
    end
    -- Carbon recovery contracts (#405). The sandbox mirrors the host's
    -- refusals: a reactor or processor with an unknown or shared store, a
    -- hazard-class mismatch, a compound store in the chain, or a methane
    -- processor whose electrical dose would not cover the endothermic
    -- reaction heat the split must absorb are all refused at
    -- construction like any other bad descriptor.
    assert(#sabatierReactors <= 8, 'station Sabatier reactors are limited to 8 entries')
    assert(#methaneProcessors <= 8, 'station methane processors are limited to 8 entries')
    for index = 1, #sabatierReactors do
        local reactor = sabatierReactors[index]
        assert(type(reactor) == 'table', 'Sabatier reactor descriptor must be a table')
        assert(type(reactor.id) == 'string' and #reactor.id > 0, 'Sabatier reactor needs an id')
        assert(reactor.hazardClass == 'normal' or reactor.hazardClass == 'hazardous',
            'Sabatier reactor needs a declared hazard class (normal or hazardous)')
        assert(type(reactor.thermalClass) == 'string'
            and (reactor.thermalClass == 'low' or reactor.thermalClass == 'moderate'
                or reactor.thermalClass == 'warm' or reactor.thermalClass == 'generation'),
            'Sabatier reactor needs a declared thermal service class')
        assert(type(reactor.rateKgPerS) == 'number'
            and reactor.rateKgPerS == reactor.rateKgPerS
            and reactor.rateKgPerS >= 0 and reactor.rateKgPerS < math.huge,
            'Sabatier reactor rated CO2 conversion must be finite and non-negative')
        assert(type(reactor.specificEnergyKWhPerKgCO2) == 'number'
            and reactor.specificEnergyKWhPerKgCO2 == reactor.specificEnergyKWhPerKgCO2
            and reactor.specificEnergyKWhPerKgCO2 >= 0 and reactor.specificEnergyKWhPerKgCO2 < math.huge,
            'Sabatier reactor specific energy must be finite and non-negative')
        assert(type(reactor.maxSinkTemperatureC) == 'number'
            and reactor.maxSinkTemperatureC == reactor.maxSinkTemperatureC
            and reactor.maxSinkTemperatureC > -273.15,
            'Sabatier reactor maximum sink temperature must be finite and above absolute zero')
        local co2 = declareGasStore(reactor.co2Store, 'CO2 feed', reactor.hazardClass, false)
        local hydrogen = declareGasStore(reactor.hydrogenStore, 'hydrogen feed',
            reactor.hazardClass, false)
        local methane = declareGasStore(reactor.methaneStore, 'methane', reactor.hazardClass, false)
        local water = declareWaterStore(reactor.waterStore, 'product water',
            reactor.hazardClass, false)
        local sink = reservoir_by_id[reactor.heatSink]
        assert(sink ~= nil, 'Sabatier reactor heat sink reservoir is unknown')
        assert(sink.thermalClass == reactor.thermalClass,
            'Sabatier reactor thermal class must match its heat sink reservoir class')
        object.sabatier_reactors[index] = {
            id = reactor.id,
            hazardClass = reactor.hazardClass,
            co2 = co2, hydrogen = hydrogen, methane = methane, water = water, sink = sink,
            rateKgPerS = reactor.rateKgPerS,
            specificEnergyKWhPerKgCO2 = reactor.specificEnergyKWhPerKgCO2,
            maxSinkTemperatureC = reactor.maxSinkTemperatureC,
            priority = reactor.priority,
            lastScale = 0, lastLimiter = '',
            lastRequested = 0, lastCo2Consumed = 0, lastHydrogenConsumed = 0,
            lastMethaneProduced = 0, lastWaterProduced = 0,
            lastPowerDemand = 0, lastPowerSupplied = 0, lastUnmetConversion = 0,
            lastReactionHeatKW = 0,
            coolingAvailable = false, lastBlockReason = '',
            co2ConsumedTotal = 0, hydrogenConsumedTotal = 0,
            methaneProducedTotal = 0, waterProducedTotal = 0,
            energySuppliedKJ = 0, reactionHeatKJ = 0, heatRoutedKJ = 0,
        }
    end
    for index = 1, #methaneProcessors do
        local processor = methaneProcessors[index]
        assert(type(processor) == 'table', 'methane processor descriptor must be a table')
        assert(type(processor.id) == 'string' and #processor.id > 0, 'methane processor needs an id')
        assert(processor.hazardClass == 'normal' or processor.hazardClass == 'hazardous',
            'methane processor needs a declared hazard class (normal or hazardous)')
        assert(type(processor.thermalClass) == 'string'
            and (processor.thermalClass == 'low' or processor.thermalClass == 'moderate'
                or processor.thermalClass == 'warm' or processor.thermalClass == 'generation'),
            'methane processor needs a declared thermal service class')
        assert(type(processor.rateKgPerS) == 'number'
            and processor.rateKgPerS == processor.rateKgPerS
            and processor.rateKgPerS >= 0 and processor.rateKgPerS < math.huge,
            'methane processor rated processing must be finite and non-negative')
        assert(type(processor.specificEnergyKWhPerKgCH4) == 'number'
            and processor.specificEnergyKWhPerKgCH4 == processor.specificEnergyKWhPerKgCH4
            and processor.specificEnergyKWhPerKgCH4 * 3600.0
                >= METHANE_PROCESSING_HEAT_KJ_PER_CH4_KG - 1e-9,
            'methane processor specific energy is below the endothermic reaction heat the split must absorb')
        assert(type(processor.maxSinkTemperatureC) == 'number'
            and processor.maxSinkTemperatureC == processor.maxSinkTemperatureC
            and processor.maxSinkTemperatureC > -273.15,
            'methane processor maximum sink temperature must be finite and above absolute zero')
        local methane = declareGasStore(processor.methaneStore, 'methane feed',
            processor.hazardClass, false)
        local hydrogen = declareGasStore(processor.hydrogenStore, 'recovered hydrogen',
            processor.hazardClass, false)
        local carbon = declareGasStore(processor.carbonStore, 'solid carbon',
            processor.hazardClass, false)
        local sink = reservoir_by_id[processor.heatSink]
        assert(sink ~= nil, 'methane processor heat sink reservoir is unknown')
        assert(sink.thermalClass == processor.thermalClass,
            'methane processor thermal class must match its heat sink reservoir class')
        object.methane_processors[index] = {
            id = processor.id,
            hazardClass = processor.hazardClass,
            methane = methane, hydrogen = hydrogen, carbon = carbon, sink = sink,
            rateKgPerS = processor.rateKgPerS,
            specificEnergyKWhPerKgCH4 = processor.specificEnergyKWhPerKgCH4,
            maxSinkTemperatureC = processor.maxSinkTemperatureC,
            priority = processor.priority,
            lastScale = 0, lastLimiter = '',
            lastRequested = 0, lastMethaneConsumed = 0, lastHydrogenReturned = 0,
            lastCarbonProduced = 0,
            lastPowerDemand = 0, lastPowerSupplied = 0, lastUnmetProcessing = 0,
            lastReactionHeatKW = 0,
            coolingAvailable = false, lastBlockReason = '',
            methaneConsumedTotal = 0, hydrogenReturnedTotal = 0, carbonProducedTotal = 0,
            energySuppliedKJ = 0, reactionHeatKJ = 0, heatRoutedKJ = 0,
        }
    end
    -- Nutrient recovery contracts (#407). A microbial processor digests
    -- a compound organic-waste feedstock and recovers its bound N/P/K/
    -- minerals at the declared per-constituent yields, its moisture at
    -- the declared water recovery, the declared CO2 share, and leaves
    -- the unrecoverable residue as the mass-balance remainder. The
    -- sandbox mirrors the host's boundary checks and re-audits its own
    -- closure: the feed must declare a composition (there is nothing to
    -- recover from an uncharacterised stream), the product stores must
    -- be plain mass (a compound destination would create tracked
    -- constituents out of nothing), the declared yields may not create
    -- mass (the residue balance may not go negative), the released
    -- reaction heat may not exceed the feedstock's declared chemical
    -- energy, and the operating window must be a real window. The
    -- recovered nutrients are funded by the feed's bound nutrient
    -- constituents only: atmospheric nitrogen is never a nutrient
    -- source here. A descriptor that would create mass, mix hazard
    -- classes in one store or release unfunded heat is refused at
    -- construction like any other bad descriptor.
    assert(#microbialProcessors <= 8, 'station microbial processors are limited to 8 entries')
    local microbialStoreClass = {}
    local function declareMicrobialStore(storeId, role, hazardClass, requireCompound)
        local store = by_id[storeId]
        assert(store ~= nil, 'nutrient ' .. role .. " store '" .. tostring(storeId) .. "' is unknown")
        assert(store.unit == 'g' or store.unit == 'kg',
            'nutrient ' .. role .. " store '" .. store.id
                .. "' must be a mass unit; nutrient recovery is mass-denominated")
        assert((store.composition ~= nil) == requireCompound,
            'nutrient ' .. role .. " store '" .. store.id
                .. (requireCompound
                    and "' must declare a composition: a microbial processor recovers bound nutrients from a declared compound feedstock"
                    or "' must be a plain mass store: recovered product streams land as plain mass, so a compound destination would create tracked constituents out of nothing"))
        local existing = microbialStoreClass[store.id]
        if existing == nil then
            microbialStoreClass[store.id] = hazardClass
        else
            assert(existing == hazardClass,
                'nutrient ' .. role .. " store '" .. store.id .. "' is shared between "
                    .. existing .. '-class and ' .. hazardClass
                    .. '-class nutrient processes: hazardous and normal nutrient streams may not share a store')
        end
        return store
    end
    for index = 1, #microbialProcessors do
        local processor = microbialProcessors[index]
        assert(type(processor) == 'table', 'microbial processor descriptor must be a table')
        assert(type(processor.id) == 'string' and #processor.id > 0, 'microbial processor needs an id')
        assert(processor.hazardClass == 'normal' or processor.hazardClass == 'hazardous',
            'microbial processor needs a declared hazard class (normal or hazardous)')
        assert(type(processor.thermalClass) == 'string'
            and (processor.thermalClass == 'low' or processor.thermalClass == 'moderate'
                or processor.thermalClass == 'warm' or processor.thermalClass == 'generation'),
            'microbial processor needs a declared thermal service class')
        local rateFields = {
            { processor.rateKgPerS, 'rated feed processing' },
            { processor.specificEnergyKWhPerKgFeed, 'specific energy' },
            { processor.oxygenPerKgFeedKg, 'oxygen per kg feed' },
            { processor.processWaterPerKgFeedKg, 'process water per kg feed' },
            { processor.consumablePerKgFeed, 'consumable per kg feed' },
            { processor.co2PerKgFeedKg, 'CO2 per kg feed' },
            { processor.reactionHeatKJPerKgFeed, 'reaction heat per kg feed' },
        }
        for _, entry in ipairs(rateFields) do
            assert(type(entry[1]) == 'number' and entry[1] == entry[1]
                and entry[1] >= 0 and entry[1] < math.huge,
                'microbial processor ' .. entry[2] .. ' must be finite and non-negative')
        end
        local recoveryFields = {
            { processor.nitrogenRecovery, 'nitrogen recovery' },
            { processor.phosphorusRecovery, 'phosphorus recovery' },
            { processor.potassiumRecovery, 'potassium recovery' },
            { processor.mineralsRecovery, 'minerals recovery' },
            { processor.waterRecovery, 'water recovery' },
        }
        for _, entry in ipairs(recoveryFields) do
            assert(type(entry[1]) == 'number' and entry[1] == entry[1]
                and entry[1] >= 0 and entry[1] <= 1,
                'microbial processor ' .. entry[2]
                    .. ' must be finite and from 0 to 1: a yield cannot recover more than the feed carries')
        end
        assert(type(processor.minOperatingTemperatureC) == 'number'
            and processor.minOperatingTemperatureC == processor.minOperatingTemperatureC
            and processor.minOperatingTemperatureC > -273.15,
            'microbial processor minimum operating temperature must be finite and above absolute zero')
        assert(type(processor.maxSinkTemperatureC) == 'number'
            and processor.maxSinkTemperatureC == processor.maxSinkTemperatureC
            and processor.maxSinkTemperatureC > -273.15,
            'microbial processor maximum sink temperature must be finite and above absolute zero')
        assert(processor.minOperatingTemperatureC < processor.maxSinkTemperatureC,
            'microbial processor minimum operating temperature must be below the maximum sink temperature: an empty operating window is never a service')
        local chainRoles = { 'feed', 'oxygen', 'process water', 'consumable',
            'nutrient', 'recovered water', 'CO2', 'residue' }
        local chainIds = { processor.feedStore, processor.oxygenStore, processor.waterStore,
            processor.consumableStore, processor.nutrientStore, processor.recoveredWaterStore,
            processor.co2Store, processor.residueStore }
        for stream = 1, 8 do
            for other = stream + 1, 8 do
                assert(chainIds[stream] ~= chainIds[other],
                    'microbial processor ' .. chainRoles[stream] .. ' store and '
                        .. chainRoles[other] .. ' store are the same store '
                        .. tostring(chainIds[stream])
                        .. "'; the recovery chain needs distinct stores for every stream")
            end
        end
        local feed = declareMicrobialStore(processor.feedStore, 'feed',
            processor.hazardClass, true)
        local oxygen = declareGasStore(processor.oxygenStore, 'oxygen',
            processor.hazardClass, false)
        local water = declareWaterStore(processor.waterStore, 'process water',
            processor.hazardClass, false)
        local consumable = by_id[processor.consumableStore]
        assert(consumable ~= nil, 'microbial processor consumable store is unknown')
        local consumableExisting = microbialStoreClass[consumable.id]
        if consumableExisting == nil then
            microbialStoreClass[consumable.id] = processor.hazardClass
        else
            assert(consumableExisting == processor.hazardClass,
                'microbial processor consumable store ' .. consumable.id
                    .. ' is shared between ' .. consumableExisting .. '-class and '
                    .. processor.hazardClass
                    .. '-class nutrient processes: hazardous and normal nutrient streams may not share a store')
        end
        local nutrient = declareMicrobialStore(processor.nutrientStore, 'nutrient',
            processor.hazardClass, false)
        local recoveredWater = declareWaterStore(processor.recoveredWaterStore, 'recovered water',
            processor.hazardClass, false)
        local co2 = declareGasStore(processor.co2Store, 'CO2',
            processor.hazardClass, false)
        local residue = declareMicrobialStore(processor.residueStore, 'residue',
            processor.hazardClass, false)
        local composition = feed.composition
        local dryPerKgFeed = 1 - composition.moisture
        local nutrientPerKgFeed = dryPerKgFeed * (composition.nutrientNitrogen
            * processor.nitrogenRecovery + composition.phosphorus * processor.phosphorusRecovery
            + composition.potassium * processor.potassiumRecovery
            + composition.otherMinerals * processor.mineralsRecovery)
        local recoveredWaterPerKgFeed = composition.moisture * processor.waterRecovery
        local residuePerKgFeed = 1 + processor.oxygenPerKgFeedKg
            + processor.processWaterPerKgFeedKg - nutrientPerKgFeed
            - recoveredWaterPerKgFeed - processor.co2PerKgFeedKg
        assert(residuePerKgFeed >= -BALANCE_TOLERANCE,
            'microbial processor ' .. processor.id
                .. ' declared yields would create mass: the outputs exceed the feed plus oxygen plus process water, and an unrecoverable residue cannot be negative')
        local feedEnergyKJPerKgFeed = dryPerKgFeed * composition.energyKJPerKgDry
        assert(processor.reactionHeatKJPerKgFeed <= feedEnergyKJPerKgFeed
            + BALANCE_TOLERANCE * math.max(1, math.abs(feedEnergyKJPerKgFeed)),
            'microbial processor ' .. processor.id
                .. ' declared reaction heat exceeds the feedstock declared chemical energy: the digestion may not release more heat than the feed carries')
        local sink = reservoir_by_id[processor.heatSink]
        assert(sink ~= nil, 'microbial processor heat sink reservoir is unknown')
        assert(sink.thermalClass == processor.thermalClass,
            'microbial processor thermal class must match its heat sink reservoir class')
        object.microbial_processors[index] = {
            id = processor.id,
            hazardClass = processor.hazardClass,
            feed = feed, oxygen = oxygen, water = water, consumable = consumable,
            nutrient = nutrient, recoveredWater = recoveredWater, co2 = co2,
            residue = residue, sink = sink,
            rateKgPerS = processor.rateKgPerS,
            specificEnergyKWhPerKgFeed = processor.specificEnergyKWhPerKgFeed,
            oxygenPerKgFeedKg = processor.oxygenPerKgFeedKg,
            processWaterPerKgFeedKg = processor.processWaterPerKgFeedKg,
            consumablePerKgFeed = processor.consumablePerKgFeed,
            co2PerKgFeedKg = processor.co2PerKgFeedKg,
            nitrogenRecovery = processor.nitrogenRecovery,
            phosphorusRecovery = processor.phosphorusRecovery,
            potassiumRecovery = processor.potassiumRecovery,
            mineralsRecovery = processor.mineralsRecovery,
            waterRecovery = processor.waterRecovery,
            reactionHeatKJPerKgFeed = processor.reactionHeatKJPerKgFeed,
            minOperatingTemperatureC = processor.minOperatingTemperatureC,
            maxSinkTemperatureC = processor.maxSinkTemperatureC,
            priority = processor.priority,
            lastScale = 0, lastLimiter = '',
            lastRequested = 0, lastFeedConsumed = 0, lastOxygenConsumed = 0,
            lastWaterConsumed = 0, lastConsumableRequested = 0, lastConsumableUsed = 0,
            lastConsumableUnmet = 0, lastNutrientProduced = 0, lastRecoveredWaterProduced = 0,
            lastCo2Produced = 0, lastResidueProduced = 0,
            lastPowerDemand = 0, lastPowerSupplied = 0, lastUnmetProcessing = 0,
            lastReactionHeatKW = 0,
            temperatureEligible = false, lastBlockReason = '',
            feedConsumedTotal = 0, oxygenConsumedTotal = 0, waterConsumedTotal = 0,
            consumablesUsedTotal = 0, nutrientProducedTotal = 0, recoveredWaterTotal = 0,
            co2ProducedTotal = 0, residueProducedTotal = 0,
            nitrogenRecoveredTotal = 0, phosphorusRecoveredTotal = 0,
            potassiumRecoveredTotal = 0, mineralsRecoveredTotal = 0,
            energySuppliedKJ = 0, reactionHeatKJ = 0, heatRoutedKJ = 0,
        }
    end
    -- Cells and capture machines compete for the same power pool and the
    -- same stores, so each runs in the declared deterministic order:
    -- priority descending, declaration index ascending.
    for index = 1, #object.electrolyzers do object.electrolyzer_order[index] = index end
    table.sort(object.electrolyzer_order, function(left, right)
        local a, b = object.electrolyzers[left], object.electrolyzers[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    for index = 1, #object.co2_captures do object.co2_capture_order[index] = index end
    table.sort(object.co2_capture_order, function(left, right)
        local a, b = object.co2_captures[left], object.co2_captures[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    -- Carbon-recovery stages compete for the same power pool and share
    -- the hydrogen and methane stores, so each runs in the declared
    -- deterministic order: priority descending, declaration index
    -- ascending (#405).
    for index = 1, #object.sabatier_reactors do object.sabatier_order[index] = index end
    table.sort(object.sabatier_order, function(left, right)
        local a, b = object.sabatier_reactors[left], object.sabatier_reactors[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    for index = 1, #object.methane_processors do object.methane_processor_order[index] = index end
    table.sort(object.methane_processor_order, function(left, right)
        local a, b = object.methane_processors[left], object.methane_processors[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
    -- Microbial processors compete for the same power pool and share the
    -- waste, water and CO2 stores, so each runs in the declared
    -- deterministic order: priority descending, declaration index
    -- ascending (#407).
    for index = 1, #object.microbial_processors do object.microbial_order[index] = index end
    table.sort(object.microbial_order, function(left, right)
        local a, b = object.microbial_processors[left], object.microbial_processors[right]
        if a.priority ~= b.priority then return a.priority > b.priority end
        return left < right
    end)
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
                - (self.unit_delivered[unit] or 0)
                - (self.unit_collected[unit] or 0) - starting
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
    -- station's own inventory (#392). Terminal-waste ejections due at the
    -- same time join the same ordered pass (#403): at equal time and
    -- priority an arrival lands before an ejection runs, so material
    -- delivered this tick is disposal-ready for the ejection that
    -- follows it.
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
        for index = 1, #self.waste_ejections do
            local ejection = self.waste_ejections[index]
            if not ejection.exhausted then
                local due = occurrencesDue(ejection.firstSeconds,
                    ejection.recurrenceSeconds, elapsed)
                for occurrence = ejection.fired + 1, due do
                    local when = ejection.recurrenceSeconds > 0
                        and ejection.firstSeconds + (occurrence - 1) * ejection.recurrenceSeconds
                        or ejection.firstSeconds
                    arrivals[#arrivals + 1] = {
                        when = when,
                        priority = ejection.priority,
                        declaration = index,
                        occurrence = occurrence,
                        ejection = true,
                    }
                end
            end
        end
        table.sort(arrivals, function(left, right)
            if left.when ~= right.when then return left.when < right.when end
            if left.priority ~= right.priority then return left.priority > right.priority end
            local leftEjection = left.ejection and 1 or 0
            local rightEjection = right.ejection and 1 or 0
            if leftEjection ~= rightEjection then return leftEjection < rightEjection end
            return left.declaration < right.declaration
        end)
        for _, arrival in ipairs(arrivals) do
            if arrival.ejection then
                local ejection = self.waste_ejections[arrival.declaration]
                local store = ejection.store
                -- Only disposal-ready (qualified) material is eligible:
                -- held (unqualified/untreated) waste is never touched.
                -- The ejection removes at most what the store can give,
                -- so a shortfall is reported, never forced, and never
                -- deletes held material. What is ejected crosses the
                -- station boundary as an external loss, booked on the
                -- same per-unit and per-constituent ledgers the host
                -- closes over (#403).
                local eligible = math.max(0.0, store.qualified)
                local ejected = math.min(ejection.quantity, eligible)
                local unmet = ejection.quantity - ejected
                assert(ejected >= 0 and unmet >= 0
                    and ejected == ejected and unmet == unmet,
                    "waste ejection amounts are not finite and non-negative")
                store.qualified = store.qualified - ejected
                self.unit_loss[store.unit] =
                    (self.unit_loss[store.unit] or 0) + ejected
                if store.composition then
                    addConstituents(self.constituent_loss, store.composition,
                        toKg(store.unit, ejected), 1)
                end
                ejection.fired = arrival.occurrence
                ejection.lastEjected = ejected
                ejection.lastUnmet = unmet
                ejection.ejectedTotal = ejection.ejectedTotal + ejected
                ejection.unmetTotal = ejection.unmetTotal + unmet
                if ejection.recurrenceSeconds <= 0 then ejection.exhausted = true end
            else
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
    end
    -- Mirror of the host ledger: same deterministic order, same limits.
    -- Unmet material never moves and is never deleted; shortages are valid
    -- outcomes, while a conservation violation is a software failure.
    -- Compartment atmospheres (#400): scheduled airlock cycles run first
    -- against the tick-start inventory in the shared scenario schedule
    -- order (occurrence time, priority descending, declaration order),
    -- then each compartment runs its occupancy draw, leakage and the
    -- coupled vapour-phase/thermal integration in the documented
    -- order (#564). The
    -- ideal-gas partial pressure and Magnus saturation formulas mirror
    -- the host exactly; the reservoir books the exact negation of the
    -- compartment exchange in applyThermal, so the crossing is one
    -- exchange accounted once on each side.
    -- the ideal-gas partial pressure and Magnus saturation formulas
    -- moved above the constructor so the authoring audit can use them.
    -- Crew metabolism (#401): one uniform metabolic scale per crew per
    -- tick - the smallest of the input-supply fractions (food and water
    -- drawable above the protected reserve, the compartment's oxygen
    -- actually present) and the output-headroom fractions (the urine
    -- and organic-waste stores' remaining capacity). Every pathway -
    -- consumption and return alike - scales by that one number
    -- together, so absent material never generates nominal waste and a
    -- backed-up waste tank throttles consumption with its returns.
    -- The requested-versus-actual remainder is explicit per pathway.
    -- The crew reads only the authored stores and compartment; no Agent
    -- state is touched. Runs after the scheduled airlock cycles and
    -- before the compartment integrates, mirroring the host order.
    local function applyCrews(self, elapsed, fixed_seconds)
        for _, crew in ipairs(self.crews) do
            local compartment = crew.compartment
            local activity = evaluateProfile(crew.activity, elapsed)
            local crewSeconds = crew.crewCount * activity * fixed_seconds
            local foodRequested = crew.foodRate * crewSeconds
            local waterRequested = crew.waterRate * crewSeconds
            local oxygenRequested = crew.oxygenRate * crewSeconds
            local co2Requested = crew.co2Rate * crewSeconds
            local moistureRequested = crew.moistureRate * crewSeconds
            local urineRequested = crew.urineRate * crewSeconds
            local wasteRequested = crew.organicWasteRate * crewSeconds
            local heatFull = crew.heatRate * crew.crewCount * activity
            local scale = 1
            local limiter = ''
            local function capSupply(store, requested, label)
                if requested <= 0 then return end
                local drawable = math.max(0, store.qualified - store.protected_reserve)
                local fraction = drawable / requested
                if fraction < scale then
                    scale = fraction
                    limiter = label .. " store '" .. store.id
                        .. (store.protected_reserve > 0
                            and "' usable inventory is at its protected reserve"
                            or "' usable inventory is empty")
                end
            end
            local function capHeadroom(store, produced, label)
                if produced <= 0 then return end
                local headroom = store.capacity - (store.qualified + store.unqualified)
                local fraction = headroom / produced
                if fraction < scale then
                    scale = fraction
                    limiter = label .. " store '" .. store.id .. "' is at capacity"
                end
            end
            capSupply(crew.food, foodRequested, 'food')
            capSupply(crew.water, waterRequested, 'water')
            if oxygenRequested > 0 then
                local fraction = math.max(0, compartment.oxygen) / oxygenRequested
                if fraction < scale then
                    scale = fraction
                    limiter = "compartment '" .. compartment.id
                        .. "' oxygen inventory cannot supply the draw"
                end
            end
            capHeadroom(crew.urine, urineRequested, 'urine')
            capHeadroom(crew.organicWaste, wasteRequested, 'organic waste')
            if not (scale > 0) then scale = 0 end
            if scale > 1 then scale = 1 end
            local function drawCapped(store, requested)
                local drawable = math.max(0, store.qualified - store.protected_reserve)
                local completed = math.min(requested * scale, drawable)
                store.qualified = store.qualified - completed
                return completed
            end
            local function deliverCapped(store, requested)
                local headroom = store.capacity - (store.qualified + store.unqualified)
                local completed = math.min(requested * scale, headroom)
                store.qualified = store.qualified + completed
                return completed
            end
            local foodActual = drawCapped(crew.food, foodRequested)
            local waterActual = drawCapped(crew.water, waterRequested)
            local oxygenActual = math.min(oxygenRequested * scale,
                math.max(0, compartment.oxygen))
            compartment.oxygen = compartment.oxygen - oxygenActual
            compartment.oxygenConsumed = compartment.oxygenConsumed + oxygenActual
            -- The returns scale with the metabolism that actually ran,
            -- never with the nominal demand.
            local co2Actual = co2Requested * scale
            compartment.carbonDioxide = compartment.carbonDioxide + co2Actual
            compartment.co2Produced = compartment.co2Produced + co2Actual
            local moistureActual = moistureRequested * scale
            compartment.waterVapour = compartment.waterVapour + moistureActual
            compartment.vapourAdded = compartment.vapourAdded + moistureActual
            local urineActual = deliverCapped(crew.urine, urineRequested)
            local wasteActual = deliverCapped(crew.organicWaste, wasteRequested)
            local heatKW = heatFull * scale
            compartment.crewHeat = (compartment.crewHeat or 0) + heatKW
            -- Boundary ledgers: consumption is a loss and the waste
            -- returns are negative loss, exactly like conversion flows,
            -- so the unit and constituent boundaries stay closed.
            self.unit_loss[crew.food.unit] = (self.unit_loss[crew.food.unit] or 0) + foodActual
            self.unit_loss[crew.water.unit] = (self.unit_loss[crew.water.unit] or 0) + waterActual
            self.unit_loss[crew.urine.unit] = (self.unit_loss[crew.urine.unit] or 0) - urineActual
            self.unit_loss[crew.organicWaste.unit] =
                (self.unit_loss[crew.organicWaste.unit] or 0) - wasteActual
            local net = {}
            addConstituents(net, crew.food.composition, toKg(crew.food.unit, foodActual), -1)
            addConstituents(net, crew.water.composition, toKg(crew.water.unit, waterActual), -1)
            addConstituents(net, crew.urine.composition, toKg(crew.urine.unit, urineActual), 1)
            addConstituents(net, crew.organicWaste.composition,
                toKg(crew.organicWaste.unit, wasteActual), 1)
            for constituent, change in pairs(net) do
                self.constituent_loss[constituent] =
                    (self.constituent_loss[constituent] or 0) - change
            end
            local retained = toKg(crew.food.unit, foodActual)
                + toKg(crew.water.unit, waterActual) + oxygenActual
                - co2Actual - moistureActual
                - toKg(crew.urine.unit, urineActual)
                - toKg(crew.organicWaste.unit, wasteActual)
            crew.lastActivity = activity
            crew.lastScale = scale
            crew.lastLimiter = scale >= 1 - 1e-12 and '' or limiter
            crew.foodConsumed = crew.foodConsumed + foodActual
            crew.waterConsumed = crew.waterConsumed + waterActual
            crew.oxygenConsumed = crew.oxygenConsumed + oxygenActual
            crew.co2Produced = crew.co2Produced + co2Actual
            crew.moistureProduced = crew.moistureProduced + moistureActual
            crew.urineProduced = crew.urineProduced + urineActual
            crew.organicWasteProduced = crew.organicWasteProduced + wasteActual
            crew.heatKJ = crew.heatKJ + heatKW * fixed_seconds
            crew.retainedKg = crew.retainedKg + retained
            crew.lastUnmet = scale < 1 - 1e-12
        end
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
        -- Crews metabolise after the scheduled cycles and before the
        -- compartments integrate (#401): the oxygen draw sees the
        -- post-cycle inventory and the returns enter the coupled
        -- vapour/thermal solve.
        applyCrews(self, elapsed, fixed_seconds)
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
            -- Thermal and phase state are integrated together (#564),
            -- mirroring the host's coupled solve. The saturation the air
            -- holds depends on the temperature and the condensation it
            -- forces releases latent heat into that same temperature;
            -- solving them in sequence left the gas supersaturated by
            -- whatever the temperature fell during the tick. The
            -- condensate x is the fixed point
            -- x = m0 - m_sat(T_free + L*x/C) with
            -- T_free = T + Q*dt/C, found by a fixed-count bisection of
            -- the strictly decreasing residual, so the published
            -- vapour never exceeds the saturation of the temperature
            -- the condensation itself produced and the thermal ledger
            -- books exactly the latent heat that left the gas. The
            -- condensate leaves the gas phase and is reported, never
            -- destroyed; it does not re-evaporate in this slice.
            local occupantHeat = occupants * compartment.sensibleHeat * metabolicFraction
            local crewHeat = compartment.crewHeat or 0
            compartment.crewHeat = 0
            local exchange = compartment.pendingExchangeKW or 0
            if compartment.reservoir then
                compartment.reservoir.atmosphereHeat =
                    (compartment.reservoir.atmosphereHeat or 0) - exchange
            end
            local freeTemperature = compartment.temperature
                + (occupantHeat + crewHeat + exchange) * fixed_seconds / compartment.capacity
            local condensed = 0
            if compartment.waterVapour > saturatedVapourMassKg(
                saturationVapourPressureKPa(freeTemperature),
                compartment.volumeM3, freeTemperature) then
                local low = 0
                local high = compartment.waterVapour
                for _ = 1, 100 do
                    local mid = (low + high) * 0.5
                    local midTemperature = freeTemperature
                        + LATENT_HEAT_VAPORIZATION * mid / compartment.capacity
                    local saturatedAtMid = saturatedVapourMassKg(
                        saturationVapourPressureKPa(midTemperature),
                        compartment.volumeM3, midTemperature)
                    if compartment.waterVapour - saturatedAtMid - mid > 0 then
                        low = mid
                    else
                        high = mid
                    end
                end
                condensed = high
            end
            local latentHeat = condensed * LATENT_HEAT_VAPORIZATION / fixed_seconds
            compartment.waterVapour = compartment.waterVapour - condensed
            compartment.condensed = compartment.condensed + condensed
            compartment.temperature = freeTemperature
                + LATENT_HEAT_VAPORIZATION * condensed / compartment.capacity
            compartment.occupantHeatKJ = compartment.occupantHeatKJ + occupantHeat * fixed_seconds
            compartment.crewHeatKJ = compartment.crewHeatKJ + crewHeat * fixed_seconds
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
                    -- #562 mirror: the crank buys only the energy still
                    -- owed. A startup duration that is not a whole number
                    -- of fixed ticks must not round the final purchase up
                    -- to a full tick at full startup power: the last
                    -- interval is prorated to the remaining requirement,
                    -- so the machine receives and dissipates exactly
                    -- startupPowerKW * startupSeconds, never more. The
                    -- prorated average bus draw stays within the authored
                    -- startup power, and the pool only has to fund what
                    -- is actually owed this tick.
                    local remainingStartupKJ = math.max(0, startupRequiredKJ - gen.startupReceivedKJ)
                    local requestKW = gen.startupPowerKW
                    if fixed_seconds > 0 then
                        requestKW = math.min(gen.startupPowerKW,
                            remainingStartupKJ / fixed_seconds)
                    end
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
        -- Fuel-cell backup (#406) runs on the supply side alongside the
        -- generators: its electrical output joins the shared generation
        -- pool the loads allocate against, its waste heat lands in the
        -- class-matched sink booked exactly once, and its reactants are
        -- drawn only from its own dedicated stores above their protected
        -- reserves - the cell can never reach protected breathing
        -- oxygen because it can only ever see its own oxygen store,
        -- which is refilled only through controlled surplus transfers
        -- that themselves preserve the supplying reserve. Output is
        -- bounded by the installed rating, the dispatch setpoint, the
        -- hydrogen and oxygen actually drawable, and the product water
        -- store's remaining headroom. Cooling is required: a sink at or
        -- above the maximum sink temperature refuses the cell whole.
        -- Mirrors the host ledger exactly.
        for _, cell in ipairs(self.fuel_cells) do
            local disabled = cell.ratedOutputKW <= 0
            local commanded = disabled and 0 or cell.commandedOutputKW
            local o2_per_h2 = FUEL_CELL_O2_KG_PER_H2_KG
            local elec_kj_per_kg_h2 = cell.efficiency * ELECTROLYSIS_H2_CHEMICAL_KJ_PER_KG
            local hydrogen = cell.hydrogen
            local oxygen = cell.oxygen
            local water = cell.water
            cell.lastOutputKW = 0
            cell.lastHydrogenKg = 0
            cell.lastOxygenKg = 0
            cell.lastWaterKg = 0
            cell.lastWasteHeatKW = 0
            cell.lastUnmetOutputKW = 0
            cell.lastLimiter = ''
            cell.lastBlockReason = ''
            -- The backup clock: usable reactants are the qualified
            -- contents drawable above each store's own protected
            -- reserve, balanced stoichiometrically, converted to the
            -- electrical energy this cell's efficiency can extract and
            -- divided by the reference output - the cell's actual
            -- output this tick, or its installed rating when the cell
            -- is not running. Protected oxygen never counts toward it.
            local function publishEndurance()
                local remainingH2Kg = math.max(0, toKg(hydrogen.unit,
                    hydrogen.qualified - hydrogen.protected_reserve))
                local remainingO2Kg = math.max(0, toKg(oxygen.unit,
                    oxygen.qualified - oxygen.protected_reserve))
                local usableH2Kg = math.min(remainingH2Kg, remainingO2Kg / o2_per_h2)
                local referenceKW = cell.lastOutputKW > 0 and cell.lastOutputKW
                    or cell.ratedOutputKW
                cell.enduranceSeconds = referenceKW > 0
                    and usableH2Kg * elec_kj_per_kg_h2 / referenceKW or 0
            end
            cell.coolingAvailable = not disabled
                and cell.sink.temperature + 1e-9 < cell.maxSinkTemperatureC
            if disabled then
                publishEndurance()
            elseif not cell.coolingAvailable then
                cell.lastBlockReason =
                    'cooling is not qualified: sink at or above the maximum sink temperature'
                cell.lastUnmetOutputKW = commanded
                publishEndurance()
            elseif commanded <= 0 then
                publishEndurance()
            else
                local h2_drawable_kg = math.max(0, toKg(hydrogen.unit,
                    hydrogen.qualified - hydrogen.protected_reserve))
                local o2_drawable_kg = math.max(0, toKg(oxygen.unit,
                    oxygen.qualified - oxygen.protected_reserve))
                local water_headroom_kg = math.max(0, toKg(water.unit,
                    water.capacity - (water.qualified + water.unqualified)))
                local by_h2_kw = h2_drawable_kg * elec_kj_per_kg_h2 / fixed_seconds
                local by_o2_kw = (o2_drawable_kg / o2_per_h2)
                    * elec_kj_per_kg_h2 / fixed_seconds
                local by_water_kw = (water_headroom_kg / (1.0 + o2_per_h2))
                    * elec_kj_per_kg_h2 / fixed_seconds
                local scale = 1.0
                local limiter = ''
                local function constrain(capKW, reason)
                    if capKW >= scale * commanded then return end
                    scale = capKW / commanded
                    limiter = reason
                end
                constrain(by_h2_kw, "hydrogen store '" .. hydrogen.id
                    .. "' has nothing drawable above its protected reserve")
                constrain(by_o2_kw, "oxygen store '" .. oxygen.id
                    .. "' has nothing drawable above its protected reserve")
                constrain(by_water_kw, "product water store '" .. water.id
                    .. "' is full: the water has nowhere to go")
                if scale < 0 then scale = 0 end
                if scale > 1 then scale = 1 end
                cell.lastLimiter = limiter
                local outputKW = commanded * scale
                if outputKW <= 1e-15 then
                    -- Nothing can burn this tick: the cell is refused
                    -- whole, consumes nothing and delivers nothing, and
                    -- the whole commanded output stays visible as unmet
                    -- service.
                    cell.lastUnmetOutputKW = commanded
                    cell.lastBlockReason = limiter
                    publishEndurance()
                else
                    -- Derive the reaction from the capped output: the
                    -- reaction stoichiometry fixes the oxygen against
                    -- the hydrogen, product water is the sum of the
                    -- reactants actually consumed, and the delivered
                    -- output is recomputed from the hydrogen that
                    -- actually moved - the pool never receives more
                    -- than the reactants paid for. Each flow is
                    -- clamped to its own drawable or headroom so a
                    -- one-ulp roundoff can never overshoot (#556).
                    local h2Kg = math.min(outputKW * fixed_seconds
                        / elec_kj_per_kg_h2, h2_drawable_kg)
                    local o2Kg = math.min(h2Kg * o2_per_h2, o2_drawable_kg)
                    h2Kg = math.min(h2Kg, o2Kg / o2_per_h2)
                    local waterKg = h2Kg + o2Kg
                    local chemicalKJ = h2Kg * ELECTROLYSIS_H2_CHEMICAL_KJ_PER_KG
                    outputKW = cell.efficiency * chemicalKJ / fixed_seconds
                    local electricalKJ = outputKW * fixed_seconds
                    local wasteKJ = chemicalKJ - electricalKJ
                    local h2Units = math.min(fromKg(hydrogen.unit, h2Kg),
                        math.max(0, hydrogen.qualified - hydrogen.protected_reserve))
                    local h2ActualKg = toKg(hydrogen.unit, h2Units)
                    local o2Units = math.min(fromKg(oxygen.unit, o2Kg),
                        math.max(0, oxygen.qualified - oxygen.protected_reserve))
                    local o2ActualKg = toKg(oxygen.unit, o2Units)
                    local waterUnits = math.min(fromKg(water.unit, waterKg),
                        math.max(0, water.capacity
                            - (water.qualified + water.unqualified)))
                    local waterActualKg = toKg(water.unit, waterUnits)
                    hydrogen.qualified = hydrogen.qualified - h2Units
                    self.unit_loss[hydrogen.unit] =
                        (self.unit_loss[hydrogen.unit] or 0) + h2Units
                    oxygen.qualified = oxygen.qualified - o2Units
                    self.unit_loss[oxygen.unit] =
                        (self.unit_loss[oxygen.unit] or 0) + o2Units
                    water.qualified = water.qualified + waterUnits
                    self.unit_loss[water.unit] =
                        (self.unit_loss[water.unit] or 0) - waterUnits
                    -- Energy: the reactants' chemical energy left as
                    -- electricity into the shared pool or as waste heat
                    -- into the sink - booked once, never a third
                    -- destination.
                    cell.sink.fuelCellHeat = (cell.sink.fuelCellHeat or 0)
                        + wasteKJ / fixed_seconds
                    generation = generation + outputKW
                    cell.lastOutputKW = outputKW
                    cell.lastHydrogenKg = h2ActualKg
                    cell.lastOxygenKg = o2ActualKg
                    cell.lastWaterKg = waterActualKg
                    cell.lastWasteHeatKW = wasteKJ / fixed_seconds
                    cell.lastUnmetOutputKW = math.max(0, commanded - outputKW)
                    cell.hydrogenConsumedKg = cell.hydrogenConsumedKg + h2ActualKg
                    cell.oxygenConsumedKg = cell.oxygenConsumedKg + o2ActualKg
                    cell.waterProducedKg = cell.waterProducedKg + waterActualKg
                    cell.chemicalEnergyKJ = cell.chemicalEnergyKJ + chemicalKJ
                    cell.electricalKJ = cell.electricalKJ + electricalKJ
                    cell.wasteHeatKJ = cell.wasteHeatKJ + wasteKJ
                    publishEndurance()
                end
            end
            assert(isFinite(cell.lastOutputKW) and isFinite(cell.lastHydrogenKg)
                and isFinite(cell.lastOxygenKg) and isFinite(cell.lastWaterKg)
                and isFinite(cell.lastWasteHeatKW) and isFinite(cell.lastUnmetOutputKW)
                and isFinite(cell.hydrogenConsumedKg) and isFinite(cell.oxygenConsumedKg)
                and isFinite(cell.waterProducedKg) and isFinite(cell.chemicalEnergyKJ)
                and isFinite(cell.electricalKJ) and isFinite(cell.wasteHeatKJ)
                and isFinite(cell.enduranceSeconds),
                "fuel cell '" .. cell.id .. "' result is not finite")
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
        -- Water treatment (#402) runs after the chillers and before the
        -- loop's second pass, mirroring the host: it competes for the same
        -- generation-and-battery pool, and the collectors that feed heat
        -- into those loops must land before the loops integrate. One
        -- uniform scale per treatment per tick - the smallest fraction of
        -- rated throughput the drawable raw water, the treated and
        -- concentrate headroom and the drawable consumables can sustain -
        -- and the process is paid for before a kilogram moves. A tick that
        -- cannot pay at all is refused whole: no water moves and no
        -- consumable is drawn.
        for _, order in ipairs(self.water_treatment_order) do
            local treatment = self.water_treatments[order]
            local source = treatment.source
            local treated = treatment.treated
            local concentrate = treatment.concentrate
            local consumable = treatment.consumable
            treatment.lastScale = 0
            treatment.lastLimiter = ''
            treatment.lastProcessed = 0
            treatment.lastTreated = 0
            treatment.lastConcentrate = 0
            treatment.lastPowerDemand = 0
            treatment.lastPowerSupplied = 0
            treatment.lastConsumableRequested = 0
            treatment.lastConsumableUsed = 0
            treatment.qualityAccepted = false
            treatment.lastHoldReason = ''
            if treatment.throughputKgPerS > 0 then
                local fullKg = treatment.throughputKgPerS * fixed_seconds
                local drawableSource = math.max(0,
                    source.qualified - source.protected_reserve)
                local treatedHeadroom = math.max(0,
                    treated.capacity - (treated.qualified + treated.unqualified))
                local concentrateHeadroom = math.max(0,
                    concentrate.capacity - (concentrate.qualified + concentrate.unqualified))
                local drawableConsumable = math.max(0,
                    consumable.qualified - consumable.protected_reserve)
                local scale = 1.0
                local limiter = ''
                local function constrain(available, required, reason)
                    if required <= 0 then return end
                    local fraction = available / required
                    if fraction < scale then
                        scale = fraction
                        limiter = reason
                    end
                end
                constrain(drawableSource, fromKg(source.unit, fullKg),
                    "raw water store '" .. source.id
                        .. "' has nothing drawable above its protected reserve")
                constrain(treatedHeadroom,
                    fromKg(treated.unit, fullKg * treatment.recoveryRatio),
                    "treated water store '" .. treated.id .. "' is full")
                constrain(concentrateHeadroom,
                    fromKg(concentrate.unit, fullKg * (1 - treatment.recoveryRatio)),
                    "concentrate store '" .. concentrate.id
                        .. "' is full: the retained stream has nowhere to go")
                constrain(drawableConsumable, treatment.consumablePerKg * fullKg,
                    "treatment consumable store '" .. consumable.id .. "' is exhausted")
                if scale < 0 then scale = 0 end
                if scale > 1 then scale = 1 end
                treatment.lastScale = scale
                treatment.lastLimiter = limiter
                if scale > 0 then
                    -- The dose is kWh per kg, so the draw is the dose
                    -- times the throughput converted to the pool's kW
                    -- (kWh/s x 3600), mirroring the host (#566).
                    local powerDemand = scale * treatment.throughputKgPerS
                        * treatment.specificEnergyKWhPerKg * 3600.0
                    treatment.lastPowerDemand = powerDemand
                    local fromGeneration = math.min(powerDemand, generation)
                    generation = generation - fromGeneration
                    local fromBattery = drawBatteries(self,
                        powerDemand - fromGeneration, fixed_hours)
                    local supplied = fromGeneration + fromBattery
                    batterySupplied = batterySupplied + fromBattery
                    treatment.lastPowerSupplied = supplied
                    if powerDemand <= 1e-12 or supplied > 1e-12 then
                        -- Move the water. Each output is clamped to its
                        -- own headroom so a one-ulp roundoff at the limit
                        -- can never overshoot capacity (#556), and the kg
                        -- ledger is recomputed from what actually moved.
                        local processedUnits = math.min(
                            fromKg(source.unit, fullKg * scale), drawableSource)
                        local processedKg = toKg(source.unit, processedUnits)
                        local treatedUnits = math.min(
                            fromKg(treated.unit, processedKg * treatment.recoveryRatio),
                            treatedHeadroom)
                        local treatedKg = toKg(treated.unit, treatedUnits)
                        local concentrateUnits = math.min(
                            fromKg(concentrate.unit, math.max(0, processedKg - treatedKg)),
                            concentrateHeadroom)
                        local concentrateKg = toKg(concentrate.unit, concentrateUnits)
                        source.qualified = source.qualified - processedUnits
                        -- Quality: the full rated energy dose is what
                        -- qualifies. A curtailed dose runs the machine but
                        -- does not finish the water, so the output is held
                        -- unqualified rather than passed through.
                        local curtailed = supplied + 1e-12 < powerDemand
                        treatment.qualityAccepted = not curtailed
                        if curtailed then
                            treated.unqualified = treated.unqualified + treatedUnits
                            treatment.lastHoldReason = 'treatment energy curtailed: '
                                .. 'the treated output is held unqualified'
                        else
                            treated.qualified = treated.qualified + treatedUnits
                        end
                        -- The concentrate is retained, not usable: it lands
                        -- on the unqualified side of its store and stays
                        -- accounted until a disposal capability exists.
                        concentrate.unqualified = concentrate.unqualified + concentrateUnits
                        treatment.lastConsumableRequested = treatment.consumablePerKg * processedKg
                        local consumableUsed = math.min(treatment.lastConsumableRequested,
                            drawableConsumable)
                        consumable.qualified = consumable.qualified - consumableUsed
                        treatment.lastConsumableUsed = consumableUsed
                        -- Unit and constituent boundaries: the source gave
                        -- the processed mass up, the treated and
                        -- concentrate stores took their shares on, and the
                        -- consumable was used out of existence.
                        self.unit_loss[source.unit] =
                            (self.unit_loss[source.unit] or 0) + processedUnits
                        self.unit_loss[treated.unit] =
                            (self.unit_loss[treated.unit] or 0) - treatedUnits
                        self.unit_loss[concentrate.unit] =
                            (self.unit_loss[concentrate.unit] or 0) - concentrateUnits
                        self.unit_loss[consumable.unit] =
                            (self.unit_loss[consumable.unit] or 0) + consumableUsed
                        if consumable.composition then
                            local net = {}
                            addConstituents(net, consumable.composition,
                                toKg(consumable.unit, consumableUsed), -1)
                            for constituent, change in pairs(net) do
                                self.constituent_loss[constituent] =
                                    (self.constituent_loss[constituent] or 0) - change
                            end
                        end
                        treatment.lastProcessed = processedKg
                        treatment.lastTreated = treatedKg
                        treatment.lastConcentrate = concentrateKg
                        treatment.processedTotal = treatment.processedTotal + processedKg
                        treatment.treatedTotal = treatment.treatedTotal + treatedKg
                        treatment.concentrateTotal = treatment.concentrateTotal + concentrateKg
                        if curtailed then
                            treatment.heldTotal = treatment.heldTotal + treatedKg
                        else
                            treatment.acceptedTotal = treatment.acceptedTotal + treatedKg
                        end
                        treatment.energySuppliedKJ = treatment.energySuppliedKJ
                            + supplied * fixed_seconds
                        treatment.energyOwedKJ = treatment.energyOwedKJ
                            + powerDemand * fixed_seconds
                        treatment.consumablesUsedTotal = treatment.consumablesUsedTotal
                            + consumableUsed
                    end
                end
            end
            assert(isFinite(treatment.lastScale) and isFinite(treatment.lastProcessed)
                and isFinite(treatment.lastTreated) and isFinite(treatment.lastConcentrate)
                and isFinite(treatment.lastPowerDemand) and isFinite(treatment.lastPowerSupplied)
                and isFinite(treatment.lastConsumableRequested)
                and isFinite(treatment.lastConsumableUsed)
                and isFinite(treatment.processedTotal) and isFinite(treatment.treatedTotal)
                and isFinite(treatment.acceptedTotal) and isFinite(treatment.heldTotal)
                and isFinite(treatment.concentrateTotal)
                and isFinite(treatment.energySuppliedKJ) and isFinite(treatment.energyOwedKJ)
                and isFinite(treatment.consumablesUsedTotal),
                "water treatment '" .. treatment.id .. "' result is not finite")
        end
        -- Water collectors (#402) run after the treatments and before the
        -- loop's second pass, so the latent heat they add is inside the
        -- same integration the loop's ledger closes over. The cold surface
        -- only works while coolant is actually circulating across it and is
        -- colder than the compartment air. The condensate is capped by the
        -- vapour held above the humidity target and by the destination's
        -- headroom, and the latent heat it gave up is booked into the
        -- loop's ledger and never into the compartment's - one
        -- condensation, one heat source.
        for _, collector in ipairs(self.water_collectors) do
            local compartment = collector.compartment
            local loop = collector.loop
            local dest = collector.dest
            collector.lastRequested = 0
            collector.lastCondensable = 0
            collector.lastCollected = 0
            collector.lastLatentHeatKW = 0
            collector.lastCirculation = loop.lastCirculation
            collector.coldSurfaceAvailable = loop.lastCirculation > 1e-12
                and loop.temperature < compartment.temperature - 1e-9
            collector.lastBlockReason = ''
            if collector.maxRateKgPerS > 0 then
                local requested = collector.maxRateKgPerS * fixed_seconds
                local saturated = saturatedVapourMassKg(
                    saturationVapourPressureKPa(compartment.temperature),
                    compartment.volumeM3, compartment.temperature)
                local condensable = math.max(0,
                    compartment.waterVapour - collector.targetRelativeHumidity * saturated)
                collector.lastRequested = requested
                collector.lastCondensable = condensable
                if not collector.coldSurfaceAvailable then
                    collector.lastBlockReason = loop.lastCirculation <= 1e-12
                        and 'no coolant circulation across the cold surface: nothing can be condensed'
                        or 'the cold surface is not colder than the compartment air: no condensation'
                elseif condensable <= 1e-12 then
                    collector.lastBlockReason =
                        'the compartment holds no vapour above the collection target'
                else
                    local destHeadroom = math.max(0, dest.capacity - (dest.qualified + dest.unqualified))
                    local collectedUnits = math.min(
                        fromKg(dest.unit, math.min(requested, condensable)), destHeadroom)
                    if collectedUnits <= 0 then
                        collector.lastBlockReason = "destination raw water store '" .. dest.id
                            .. "' is full: nowhere to put the condensate"
                    else
                        local collectedKg = toKg(dest.unit, collectedUnits)
                        collector.lastCollected = collectedKg
                        compartment.waterVapour = compartment.waterVapour - collectedKg
                        compartment.collected = (compartment.collected or 0) + collectedKg
                        dest.qualified = dest.qualified + collectedUnits
                        self.unit_collected[dest.unit] =
                            (self.unit_collected[dest.unit] or 0) + collectedUnits
                        -- The latent heat leaves the gas and enters the
                        -- coolant, once.
                        local latentHeatKW = collectedKg * LATENT_HEAT_VAPORIZATION / fixed_seconds
                        loop.collectorHeat = (loop.collectorHeat or 0) + latentHeatKW
                        collector.lastLatentHeatKW = latentHeatKW
                        collector.latentHeatKJ = collector.latentHeatKJ + latentHeatKW * fixed_seconds
                        collector.collectedTotal = collector.collectedTotal + collectedKg
                    end
                end
            end
            assert(isFinite(collector.lastRequested) and isFinite(collector.lastCondensable)
                and isFinite(collector.lastCollected) and isFinite(collector.lastLatentHeatKW)
                and isFinite(collector.collectedTotal) and isFinite(collector.latentHeatKJ),
                "water collector '" .. collector.id .. "' result is not finite")
        end
        -- Electrolysis (#404) runs after the water collectors and before
        -- the loop's second pass: it competes for the same generation-and-
        -- battery pool, and its process heat must land in the sink before
        -- the reservoir integrates. The cell is paid for before a
        -- kilogram splits, and it only splits water the actually supplied
        -- energy can make: the electrical dose either ends up bound in
        -- the produced hydrogen or routed into the class-matched sink as
        -- process heat, booked exactly once. A tick that demands power
        -- and receives none is refused whole, and a sink at or above the
        -- maximum sink temperature refuses the cell whole.
        for _, order in ipairs(self.electrolyzer_order) do
            local cell = self.electrolyzers[order]
            local water = cell.feed
            local hydrogen = cell.hydrogen
            cell.lastScale = 0
            cell.lastLimiter = ''
            cell.lastRequested = 0
            cell.lastSplit = 0
            cell.lastOxygen = 0
            cell.lastHydrogen = 0
            cell.lastPowerDemand = 0
            cell.lastPowerSupplied = 0
            cell.lastUnmetProduction = 0
            cell.coolingAvailable = false
            cell.lastBlockReason = ''
            if cell.rateKgPerS > 0 then
                local fullKg = cell.rateKgPerS * fixed_seconds
                -- Cooling is qualified while the sink sits below the
                -- maximum sink temperature: at or above it the interface
                -- cannot accept the cell's process heat.
                cell.coolingAvailable = cell.sink.temperature + 1e-9
                    < cell.maxSinkTemperatureC
                if not cell.coolingAvailable then
                    cell.lastBlockReason =
                        'cooling is not qualified: sink at or above the maximum sink temperature'
                else
                    local drawableWater = math.max(0,
                        water.qualified - water.protected_reserve)
                    local hydrogenHeadroom = math.max(0,
                        hydrogen.capacity - (hydrogen.qualified + hydrogen.unqualified))
                    local oxygenHeadroom = 0
                    if cell.oxygen ~= nil then
                        oxygenHeadroom = math.max(0, cell.oxygen.capacity
                            - (cell.oxygen.qualified + cell.oxygen.unqualified))
                    end
                    local scale = 1.0
                    local limiter = ''
                    local function constrain(available, required, reason)
                        if required <= 0 then return end
                        local fraction = available / required
                        if fraction < scale then
                            scale = fraction
                            limiter = reason
                        end
                    end
                    constrain(drawableWater, fromKg(water.unit, fullKg),
                        "feed water store '" .. water.id
                            .. "' has nothing drawable above its protected reserve")
                    if cell.oxygen ~= nil then
                        constrain(oxygenHeadroom,
                            fromKg(cell.oxygen.unit,
                                fullKg * ELECTROLYSIS_O2_KG_PER_WATER_KG),
                            "oxygen store '" .. cell.oxygen.id
                                .. "' is full: the product has nowhere to go")
                    end
                    constrain(hydrogenHeadroom,
                        fromKg(hydrogen.unit,
                            fullKg * (1 - ELECTROLYSIS_O2_KG_PER_WATER_KG)),
                        "hydrogen store '" .. hydrogen.id
                            .. "' is full: the product has nowhere to go")
                    if scale < 0 then scale = 0 end
                    if scale > 1 then scale = 1 end
                    cell.lastScale = scale
                    cell.lastLimiter = limiter
                    cell.lastRequested = fullKg
                    if scale > 0 then
                        -- The dose is authored in kWh per kilogram of
                        -- split water, the same physical unit as the
                        -- declared chemical energy, so the power the
                        -- cell draws is the dose times the split rate
                        -- converted to the pool's kW (kWh/s x 3600).
                        -- Mirrors the host (#404).
                        local powerDemand = scale * cell.rateKgPerS
                            * cell.specificEnergyKWhPerKgWater * 3600.0
                        cell.lastPowerDemand = powerDemand
                        local fromGeneration = math.min(powerDemand, generation)
                        generation = generation - fromGeneration
                        local fromBattery = drawBatteries(self,
                            powerDemand - fromGeneration, fixed_hours)
                        local supplied = fromGeneration + fromBattery
                        batterySupplied = batterySupplied + fromBattery
                        cell.lastPowerSupplied = supplied
                        if powerDemand > 1e-12 and supplied <= 1e-12 then
                            -- Nothing was paid: the tick is refused whole.
                            -- No water splits and no product appears.
                            cell.lastBlockReason =
                                'no electrical supply: the electrolyzer cannot run'
                        else
                            -- Energy feasibility: the cell only splits
                            -- water the supplied energy can actually make.
                            local energySuppliedKJ = supplied * fixed_seconds
                            local energyFeasibleKg = energySuppliedKJ
                                / ELECTROLYSIS_CHEMICAL_KJ_PER_WATER_KG
                            local feasibleSplitKg = math.min(fullKg * scale, energyFeasibleKg)
                            -- Move the water. Each flow is clamped to its
                            -- own drawable or headroom so a one-ulp
                            -- roundoff can never overshoot (#556).
                            local waterUnits = math.min(
                                fromKg(water.unit, feasibleSplitKg), drawableWater)
                            local splitKg = toKg(water.unit, waterUnits)
                            local oxygenKg = splitKg * ELECTROLYSIS_O2_KG_PER_WATER_KG
                            local oxygenActualKg = oxygenKg
                            if cell.oxygen ~= nil then
                                local oxygenUnits = math.min(
                                    fromKg(cell.oxygen.unit, oxygenKg), oxygenHeadroom)
                                oxygenActualKg = toKg(cell.oxygen.unit, oxygenUnits)
                                cell.oxygen.qualified = cell.oxygen.qualified + oxygenUnits
                                self.unit_loss[cell.oxygen.unit] =
                                    (self.unit_loss[cell.oxygen.unit] or 0) - oxygenUnits
                            else
                                -- Delivered straight into the
                                -- compartment: booked as an addition
                                -- pathway of the compartment's own
                                -- closure, so every gram the cell made
                                -- is visible in the air it was produced
                                -- for.
                                cell.compartment.oxygen = cell.compartment.oxygen + oxygenKg
                                cell.compartment.electrolysisOxygen =
                                    (cell.compartment.electrolysisOxygen or 0) + oxygenKg
                            end
                            local hydrogenKg = splitKg - oxygenActualKg
                            local hydrogenUnits = math.min(
                                fromKg(hydrogen.unit, hydrogenKg), hydrogenHeadroom)
                            local hydrogenActualKg = toKg(hydrogen.unit, hydrogenUnits)
                            water.qualified = water.qualified - waterUnits
                            hydrogen.qualified = hydrogen.qualified + hydrogenUnits
                            self.unit_loss[water.unit] =
                                (self.unit_loss[water.unit] or 0) + waterUnits
                            self.unit_loss[hydrogen.unit] =
                                (self.unit_loss[hydrogen.unit] or 0) - hydrogenUnits
                            -- Energy: the dose either stays bound in the
                            -- produced hydrogen or leaves as process heat
                            -- routed into the sink, once.
                            local chemicalKJ = hydrogenActualKg
                                * ELECTROLYSIS_H2_CHEMICAL_KJ_PER_KG
                            local heatKJ = math.max(0, energySuppliedKJ - chemicalKJ)
                            cell.sink.electrolysisHeat =
                                (cell.sink.electrolysisHeat or 0) + heatKJ / fixed_seconds
                            cell.lastSplit = splitKg
                            cell.lastOxygen = oxygenActualKg
                            cell.lastHydrogen = hydrogenActualKg
                            cell.lastUnmetProduction = math.max(0, fullKg * scale - splitKg)
                            cell.splitTotal = cell.splitTotal + splitKg
                            cell.oxygenTotal = cell.oxygenTotal + oxygenActualKg
                            cell.hydrogenTotal = cell.hydrogenTotal + hydrogenActualKg
                            cell.energySuppliedKJ = cell.energySuppliedKJ + energySuppliedKJ
                            cell.chemicalEnergyKJ = cell.chemicalEnergyKJ + chemicalKJ
                            cell.heatRoutedKJ = cell.heatRoutedKJ + heatKJ
                        end
                    end
                end
            end
            assert(isFinite(cell.lastScale) and isFinite(cell.lastRequested)
                and isFinite(cell.lastSplit) and isFinite(cell.lastOxygen)
                and isFinite(cell.lastHydrogen) and isFinite(cell.lastPowerDemand)
                and isFinite(cell.lastPowerSupplied) and isFinite(cell.lastUnmetProduction)
                and isFinite(cell.splitTotal) and isFinite(cell.oxygenTotal)
                and isFinite(cell.hydrogenTotal) and isFinite(cell.energySuppliedKJ)
                and isFinite(cell.chemicalEnergyKJ) and isFinite(cell.heatRoutedKJ),
                "electrolyzer '" .. cell.id .. "' result is not finite")
        end
        -- CO2 removal (#404) runs in the same phase: captured gas is
        -- inventory, not vanished carbon. The machine can only scrub the
        -- CO2 the compartment actually holds, into the capture store's
        -- remaining headroom, on the consumable it can actually draw and
        -- the power the pool can actually supply. Every axis of
        -- finiteness is a scale cap, and the unmet atmospheric service is
        -- the explicit remainder. A tick that demands power and receives
        -- none is refused whole: no CO2 is scrubbed and no consumable is
        -- drawn.
        for _, order in ipairs(self.co2_capture_order) do
            local capture = self.co2_captures[order]
            local compartment = capture.compartment
            local destination = capture.destination
            capture.lastScale = 0
            capture.lastLimiter = ''
            capture.lastRequested = 0
            capture.lastCaptured = 0
            capture.lastPowerDemand = 0
            capture.lastPowerSupplied = 0
            capture.lastConsumableRequested = 0
            capture.lastConsumableUsed = 0
            capture.lastBlockReason = ''
            if capture.maxRateKgPerS > 0 then
                local fullKg = capture.maxRateKgPerS * fixed_seconds
                local co2Available = math.max(0, compartment.carbonDioxide)
                local captureHeadroom = math.max(0, destination.capacity
                    - (destination.qualified + destination.unqualified))
                local drawableConsumable = 0
                if capture.consumable ~= nil then
                    drawableConsumable = math.max(0, capture.consumable.qualified
                        - capture.consumable.protected_reserve)
                end
                local scale = 1.0
                local limiter = ''
                local function constrain(available, required, reason)
                    if required <= 0 then return end
                    local fraction = available / required
                    if fraction < scale then
                        scale = fraction
                        limiter = reason
                    end
                end
                constrain(co2Available, fullKg,
                    "compartment '" .. compartment.id
                        .. "' holds no carbon dioxide to capture")
                constrain(captureHeadroom, fromKg(destination.unit, fullKg),
                    "capture store '" .. destination.id
                        .. "' is full: the captured gas has nowhere to go")
                if capture.consumable ~= nil then
                    constrain(drawableConsumable, capture.consumablePerKg * fullKg,
                        "capture consumable store '"
                            .. capture.consumable.id .. "' is exhausted")
                end
                if scale < 0 then scale = 0 end
                if scale > 1 then scale = 1 end
                capture.lastScale = scale
                capture.lastLimiter = limiter
                capture.lastRequested = fullKg
                if scale > 0 then
                    local powerDemand = scale * capture.maxRateKgPerS
                        * capture.specificEnergyKWhPerKg
                    capture.lastPowerDemand = powerDemand
                    local fromGeneration = math.min(powerDemand, generation)
                    generation = generation - fromGeneration
                    local fromBattery = drawBatteries(self,
                        powerDemand - fromGeneration, fixed_hours)
                    local supplied = fromGeneration + fromBattery
                    batterySupplied = batterySupplied + fromBattery
                    capture.lastPowerSupplied = supplied
                    if powerDemand > 1e-12 and supplied <= 1e-12 then
                        capture.lastBlockReason =
                            'no electrical supply: the capture machine cannot run'
                    else
                        -- Capture only what the compartment holds and the
                        -- store can take, clamped per flow so roundoff
                        -- never overshoots.
                        local wantedKg = math.min(fullKg * scale, co2Available)
                        local capturedUnits = math.min(
                            fromKg(destination.unit, wantedKg), captureHeadroom)
                        local capturedKg = toKg(destination.unit, capturedUnits)
                        compartment.carbonDioxide = compartment.carbonDioxide - capturedKg
                        compartment.capturedCarbonDioxide =
                            (compartment.capturedCarbonDioxide or 0) + capturedKg
                        destination.qualified = destination.qualified + capturedUnits
                        -- The captured gas enters the store network from
                        -- outside it, exactly like collector condensate:
                        -- the boundary closes as current + lost -
                        -- delivered - collected, so the carbon is
                        -- accounted inflow, never implicit creation.
                        self.unit_collected[destination.unit] =
                            (self.unit_collected[destination.unit] or 0) + capturedUnits
                        if capture.consumable ~= nil then
                            capture.lastConsumableRequested =
                                capture.consumablePerKg * capturedKg
                            local consumableUsed = math.min(capture.lastConsumableRequested,
                                drawableConsumable)
                            capture.consumable.qualified = capture.consumable.qualified
                                - consumableUsed
                            capture.lastConsumableUsed = consumableUsed
                            self.unit_loss[capture.consumable.unit] =
                                (self.unit_loss[capture.consumable.unit] or 0) + consumableUsed
                            if capture.consumable.composition then
                                local net = {}
                                addConstituents(net, capture.consumable.composition,
                                    toKg(capture.consumable.unit, consumableUsed), -1)
                                for constituent, change in pairs(net) do
                                    self.constituent_loss[constituent] =
                                        (self.constituent_loss[constituent] or 0) - change
                                end
                            end
                            capture.consumablesUsedTotal = capture.consumablesUsedTotal
                                + consumableUsed
                        end
                        capture.lastCaptured = capturedKg
                        capture.capturedTotal = capture.capturedTotal + capturedKg
                        capture.energySuppliedKJ = capture.energySuppliedKJ
                            + supplied * fixed_seconds
                    end
                end
            end
            assert(isFinite(capture.lastScale) and isFinite(capture.lastRequested)
                and isFinite(capture.lastCaptured) and isFinite(capture.lastPowerDemand)
                and isFinite(capture.lastPowerSupplied)
                and isFinite(capture.lastConsumableRequested)
                and isFinite(capture.lastConsumableUsed)
                and isFinite(capture.capturedTotal) and isFinite(capture.energySuppliedKJ)
                and isFinite(capture.consumablesUsedTotal),
                "CO2 capture machine '" .. capture.id .. "' result is not finite")
        end
        -- Carbon recovery (#405) runs in the same phase after CO2
        -- capture: the Sabatier reactor converts captured CO2 and
        -- hydrogen into methane and water, and the methane processor
        -- splits methane into recovered hydrogen and solid carbon. Both
        -- compete for the same generation-and-battery pool, and their
        -- heat must land in the sink before the reservoir integrates.
        -- Each stage is paid for before a kilogram converts, and only
        -- converts what the actually supplied energy can drive at the
        -- declared dose. Mirrors the host exactly.
        for _, order in ipairs(self.sabatier_order) do
            local reactor = self.sabatier_reactors[order]
            local co2 = reactor.co2
            local hydrogen = reactor.hydrogen
            reactor.lastScale = 0
            reactor.lastLimiter = ''
            reactor.lastRequested = 0
            reactor.lastCo2Consumed = 0
            reactor.lastHydrogenConsumed = 0
            reactor.lastMethaneProduced = 0
            reactor.lastWaterProduced = 0
            reactor.lastPowerDemand = 0
            reactor.lastPowerSupplied = 0
            reactor.lastUnmetConversion = 0
            reactor.lastReactionHeatKW = 0
            reactor.coolingAvailable = false
            reactor.lastBlockReason = ''
            if reactor.rateKgPerS > 0 then
                local fullKg = reactor.rateKgPerS * fixed_seconds
                reactor.coolingAvailable = reactor.sink.temperature + 1e-9
                    < reactor.maxSinkTemperatureC
                if not reactor.coolingAvailable then
                    reactor.lastBlockReason =
                        'cooling is not qualified: sink at or above the maximum sink temperature'
                else
                    local drawableCo2 = math.max(0, co2.qualified - co2.protected_reserve)
                    local drawableHydrogen = math.max(0,
                        hydrogen.qualified - hydrogen.protected_reserve)
                    local methaneHeadroom = math.max(0, reactor.methane.capacity
                        - (reactor.methane.qualified + reactor.methane.unqualified))
                    local waterHeadroom = math.max(0, reactor.water.capacity
                        - (reactor.water.qualified + reactor.water.unqualified))
                    local scale = 1.0
                    local limiter = ''
                    local function constrain(available, required, reason)
                        if required <= 0 then return end
                        local fraction = available / required
                        if fraction < scale then
                            scale = fraction
                            limiter = reason
                        end
                    end
                    constrain(drawableCo2, fromKg(co2.unit, fullKg),
                        "CO2 feed store '" .. co2.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(drawableHydrogen,
                        fromKg(hydrogen.unit,
                            fullKg * SABATIER_H2_KG_PER_CO2_KG),
                        "hydrogen feed store '" .. hydrogen.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(methaneHeadroom,
                        fromKg(reactor.methane.unit,
                            fullKg * SABATIER_CH4_KG_PER_CO2_KG),
                        "methane store '" .. reactor.methane.id
                            .. "' is full: the product has nowhere to go")
                    constrain(waterHeadroom,
                        fromKg(reactor.water.unit,
                            fullKg * SABATIER_H2O_KG_PER_CO2_KG),
                        "product water store '" .. reactor.water.id
                            .. "' is full: the water has nowhere to go")
                    if scale < 0 then scale = 0 end
                    if scale > 1 then scale = 1 end
                    reactor.lastScale = scale
                    reactor.lastLimiter = limiter
                    reactor.lastRequested = fullKg
                    if scale > 0 then
                        local powerDemand = scale * reactor.rateKgPerS
                            * reactor.specificEnergyKWhPerKgCO2 * 3600.0
                        reactor.lastPowerDemand = powerDemand
                        local fromGeneration = math.min(powerDemand, generation)
                        generation = generation - fromGeneration
                        local fromBattery = drawBatteries(self,
                            powerDemand - fromGeneration, fixed_hours)
                        local supplied = fromGeneration + fromBattery
                        batterySupplied = batterySupplied + fromBattery
                        reactor.lastPowerSupplied = supplied
                        if powerDemand > 1e-12 and supplied <= 1e-12 then
                            reactor.lastBlockReason =
                                'no electrical supply: the Sabatier reactor cannot run'
                        else
                            local energySuppliedKJ = supplied * fixed_seconds
                            -- Energy feasibility: the reactor only
                            -- converts the CO2 the supplied energy can
                            -- pay for at the declared dose.
                            local doseKJ = reactor.specificEnergyKWhPerKgCO2 * 3600.0
                            local energyFeasibleKg = doseKJ > 0
                                and energySuppliedKJ / doseKJ or fullKg * scale
                            -- The extent is capped by the reactants that
                            -- are actually drawable above their protected
                            -- reserves.
                            local extentKg = math.min(energyFeasibleKg,
                                toKg(co2.unit, drawableCo2))
                            extentKg = math.min(extentKg,
                                toKg(hydrogen.unit, drawableHydrogen)
                                    / SABATIER_H2_KG_PER_CO2_KG)
                            -- Move the flows, clamped per flow so
                            -- roundoff never overshoots (#556).
                            local co2Units = math.min(
                                fromKg(co2.unit, extentKg), drawableCo2)
                            local co2ActualKg = toKg(co2.unit, co2Units)
                            local hydrogenUnits = math.min(
                                fromKg(hydrogen.unit,
                                    co2ActualKg * SABATIER_H2_KG_PER_CO2_KG),
                                drawableHydrogen)
                            local hydrogenActualKg = toKg(hydrogen.unit, hydrogenUnits)
                            -- Products follow the limiting reactant that
                            -- actually moved.
                            local extentActualKg = math.min(co2ActualKg,
                                hydrogenActualKg / SABATIER_H2_KG_PER_CO2_KG)
                            local methaneUnits = math.min(
                                fromKg(reactor.methane.unit,
                                    extentActualKg * SABATIER_CH4_KG_PER_CO2_KG),
                                methaneHeadroom)
                            local methaneActualKg = toKg(reactor.methane.unit, methaneUnits)
                            local waterUnits = math.min(
                                fromKg(reactor.water.unit,
                                    extentActualKg * SABATIER_H2O_KG_PER_CO2_KG),
                                waterHeadroom)
                            local waterActualKg = toKg(reactor.water.unit, waterUnits)
                            co2.qualified = co2.qualified - co2Units
                            hydrogen.qualified = hydrogen.qualified - hydrogenUnits
                            reactor.methane.qualified = reactor.methane.qualified + methaneUnits
                            reactor.water.qualified = reactor.water.qualified + waterUnits
                            self.unit_loss[co2.unit] =
                                (self.unit_loss[co2.unit] or 0) + co2Units
                            self.unit_loss[hydrogen.unit] =
                                (self.unit_loss[hydrogen.unit] or 0) + hydrogenUnits
                            self.unit_loss[reactor.methane.unit] =
                                (self.unit_loss[reactor.methane.unit] or 0) - methaneUnits
                            self.unit_loss[reactor.water.unit] =
                                (self.unit_loss[reactor.water.unit] or 0) - waterUnits
                            -- Energy: the dose plus the exothermic heat
                            -- the products do not retain leave through
                            -- the sink, once.
                            local reactionHeatKJ = extentActualKg * SABATIER_HEAT_KJ_PER_CO2_KG
                            local heatKJ = energySuppliedKJ + reactionHeatKJ
                            reactor.sink.sabatierHeat =
                                (reactor.sink.sabatierHeat or 0) + heatKJ / fixed_seconds
                            reactor.lastReactionHeatKW = reactionHeatKJ / fixed_seconds
                            reactor.lastCo2Consumed = co2ActualKg
                            reactor.lastHydrogenConsumed = hydrogenActualKg
                            reactor.lastMethaneProduced = methaneActualKg
                            reactor.lastWaterProduced = waterActualKg
                            reactor.lastUnmetConversion = math.max(0, fullKg * scale - extentActualKg)
                            reactor.co2ConsumedTotal = reactor.co2ConsumedTotal + co2ActualKg
                            reactor.hydrogenConsumedTotal =
                                reactor.hydrogenConsumedTotal + hydrogenActualKg
                            reactor.methaneProducedTotal =
                                reactor.methaneProducedTotal + methaneActualKg
                            reactor.waterProducedTotal = reactor.waterProducedTotal + waterActualKg
                            reactor.energySuppliedKJ = reactor.energySuppliedKJ + energySuppliedKJ
                            reactor.reactionHeatKJ = reactor.reactionHeatKJ + reactionHeatKJ
                            reactor.heatRoutedKJ = reactor.heatRoutedKJ + heatKJ
                        end
                    end
                end
            end
            assert(isFinite(reactor.lastScale) and isFinite(reactor.lastRequested)
                and isFinite(reactor.lastCo2Consumed) and isFinite(reactor.lastHydrogenConsumed)
                and isFinite(reactor.lastMethaneProduced) and isFinite(reactor.lastWaterProduced)
                and isFinite(reactor.lastPowerDemand) and isFinite(reactor.lastPowerSupplied)
                and isFinite(reactor.lastUnmetConversion) and isFinite(reactor.lastReactionHeatKW)
                and isFinite(reactor.co2ConsumedTotal) and isFinite(reactor.hydrogenConsumedTotal)
                and isFinite(reactor.methaneProducedTotal) and isFinite(reactor.waterProducedTotal)
                and isFinite(reactor.energySuppliedKJ) and isFinite(reactor.reactionHeatKJ)
                and isFinite(reactor.heatRoutedKJ),
                "Sabatier reactor '" .. reactor.id .. "' result is not finite")
        end
        for _, order in ipairs(self.methane_processor_order) do
            local processor = self.methane_processors[order]
            local methane = processor.methane
            local hydrogen = processor.hydrogen
            processor.lastScale = 0
            processor.lastLimiter = ''
            processor.lastRequested = 0
            processor.lastMethaneConsumed = 0
            processor.lastHydrogenReturned = 0
            processor.lastCarbonProduced = 0
            processor.lastPowerDemand = 0
            processor.lastPowerSupplied = 0
            processor.lastUnmetProcessing = 0
            processor.lastReactionHeatKW = 0
            processor.coolingAvailable = false
            processor.lastBlockReason = ''
            if processor.rateKgPerS > 0 then
                local fullKg = processor.rateKgPerS * fixed_seconds
                processor.coolingAvailable = processor.sink.temperature + 1e-9
                    < processor.maxSinkTemperatureC
                if not processor.coolingAvailable then
                    processor.lastBlockReason =
                        'cooling is not qualified: sink at or above the maximum sink temperature'
                else
                    local drawableMethane = math.max(0,
                        methane.qualified - methane.protected_reserve)
                    local hydrogenHeadroom = math.max(0, hydrogen.capacity
                        - (hydrogen.qualified + hydrogen.unqualified))
                    local carbonHeadroom = math.max(0, processor.carbon.capacity
                        - (processor.carbon.qualified + processor.carbon.unqualified))
                    local scale = 1.0
                    local limiter = ''
                    local function constrain(available, required, reason)
                        if required <= 0 then return end
                        local fraction = available / required
                        if fraction < scale then
                            scale = fraction
                            limiter = reason
                        end
                    end
                    constrain(drawableMethane, fromKg(methane.unit, fullKg),
                        "methane feed store '" .. methane.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(hydrogenHeadroom,
                        fromKg(hydrogen.unit,
                            fullKg * METHANE_PROCESSING_H2_KG_PER_CH4_KG),
                        "recovered hydrogen store '" .. hydrogen.id
                            .. "' is full: the recovered hydrogen has nowhere to go")
                    constrain(carbonHeadroom,
                        fromKg(processor.carbon.unit,
                            fullKg * METHANE_PROCESSING_C_KG_PER_CH4_KG),
                        "carbon store '" .. processor.carbon.id
                            .. "' is full: the solid carbon has nowhere to go")
                    if scale < 0 then scale = 0 end
                    if scale > 1 then scale = 1 end
                    processor.lastScale = scale
                    processor.lastLimiter = limiter
                    processor.lastRequested = fullKg
                    if scale > 0 then
                        local powerDemand = scale * processor.rateKgPerS
                            * processor.specificEnergyKWhPerKgCH4 * 3600.0
                        processor.lastPowerDemand = powerDemand
                        local fromGeneration = math.min(powerDemand, generation)
                        generation = generation - fromGeneration
                        local fromBattery = drawBatteries(self,
                            powerDemand - fromGeneration, fixed_hours)
                        local supplied = fromGeneration + fromBattery
                        batterySupplied = batterySupplied + fromBattery
                        processor.lastPowerSupplied = supplied
                        if powerDemand > 1e-12 and supplied <= 1e-12 then
                            processor.lastBlockReason =
                                'no electrical supply: the methane processor cannot run'
                        else
                            local energySuppliedKJ = supplied * fixed_seconds
                            -- Energy feasibility: the processor only
                            -- splits the methane the supplied energy can
                            -- drive against the endothermic reaction
                            -- demand.
                            local energyFeasibleKg = energySuppliedKJ
                                / METHANE_PROCESSING_HEAT_KJ_PER_CH4_KG
                            local processedKg = math.min(fullKg * scale, energyFeasibleKg)
                            local methaneUnits = math.min(
                                fromKg(methane.unit, processedKg), drawableMethane)
                            local methaneActualKg = toKg(methane.unit, methaneUnits)
                            local hydrogenUnits = math.min(
                                fromKg(hydrogen.unit,
                                    methaneActualKg * METHANE_PROCESSING_H2_KG_PER_CH4_KG),
                                hydrogenHeadroom)
                            local hydrogenActualKg = toKg(hydrogen.unit, hydrogenUnits)
                            local carbonUnits = math.min(
                                fromKg(processor.carbon.unit,
                                    methaneActualKg * METHANE_PROCESSING_C_KG_PER_CH4_KG),
                                carbonHeadroom)
                            local carbonActualKg = toKg(processor.carbon.unit, carbonUnits)
                            methane.qualified = methane.qualified - methaneUnits
                            hydrogen.qualified = hydrogen.qualified + hydrogenUnits
                            processor.carbon.qualified = processor.carbon.qualified + carbonUnits
                            self.unit_loss[methane.unit] =
                                (self.unit_loss[methane.unit] or 0) + methaneUnits
                            self.unit_loss[hydrogen.unit] =
                                (self.unit_loss[hydrogen.unit] or 0) - hydrogenUnits
                            self.unit_loss[processor.carbon.unit] =
                                (self.unit_loss[processor.carbon.unit] or 0) - carbonUnits
                            -- Energy: the dose either drove the
                            -- endothermic reaction or leaves as process
                            -- heat routed into the sink, once.
                            local reactionHeatKJ = methaneActualKg
                                * METHANE_PROCESSING_HEAT_KJ_PER_CH4_KG
                            local heatKJ = math.max(0, energySuppliedKJ - reactionHeatKJ)
                            processor.sink.methaneHeat =
                                (processor.sink.methaneHeat or 0) + heatKJ / fixed_seconds
                            processor.lastReactionHeatKW = reactionHeatKJ / fixed_seconds
                            processor.lastMethaneConsumed = methaneActualKg
                            processor.lastHydrogenReturned = hydrogenActualKg
                            processor.lastCarbonProduced = carbonActualKg
                            processor.lastUnmetProcessing =
                                math.max(0, fullKg * scale - methaneActualKg)
                            processor.methaneConsumedTotal =
                                processor.methaneConsumedTotal + methaneActualKg
                            processor.hydrogenReturnedTotal =
                                processor.hydrogenReturnedTotal + hydrogenActualKg
                            processor.carbonProducedTotal =
                                processor.carbonProducedTotal + carbonActualKg
                            processor.energySuppliedKJ =
                                processor.energySuppliedKJ + energySuppliedKJ
                            processor.reactionHeatKJ = processor.reactionHeatKJ + reactionHeatKJ
                            processor.heatRoutedKJ = processor.heatRoutedKJ + heatKJ
                        end
                    end
                end
            end
            assert(isFinite(processor.lastScale) and isFinite(processor.lastRequested)
                and isFinite(processor.lastMethaneConsumed) and isFinite(processor.lastHydrogenReturned)
                and isFinite(processor.lastCarbonProduced)
                and isFinite(processor.lastPowerDemand) and isFinite(processor.lastPowerSupplied)
                and isFinite(processor.lastUnmetProcessing) and isFinite(processor.lastReactionHeatKW)
                and isFinite(processor.methaneConsumedTotal) and isFinite(processor.hydrogenReturnedTotal)
                and isFinite(processor.carbonProducedTotal) and isFinite(processor.energySuppliedKJ)
                and isFinite(processor.reactionHeatKJ) and isFinite(processor.heatRoutedKJ),
                "methane processor '" .. processor.id .. "' result is not finite")
        end
        -- Nutrient recovery (#407) runs in the same phase after carbon
        -- recovery: the microbial processor digests the compound
        -- organic-waste feed, recovers its bound N/P/K/minerals at the
        -- declared per-constituent yields and its moisture at the
        -- declared water recovery, releases the declared CO2 share, and
        -- leaves the unrecoverable residue as the mass-balance
        -- remainder - so the digestion closes on mass by construction
        -- and the feed's atmospheric nitrogen can only pass through to
        -- the residue, never into the recovered nutrients. The stage
        -- competes for the same generation-and-battery pool, pays for
        -- itself before a kilogram digests, and only digests what the
        -- actually supplied energy can drive at the declared dose. Its
        -- exothermic reaction heat plus the electrical dose route into
        -- the class-matched sink exactly once. Feeds draw only above
        -- their protected reserves; saturated destinations cap the
        -- whole stage through the uniform scale instead of venting; the
        -- static operating window gates temperature eligibility against
        -- the sink reservoir without modelling any dynamic culture
        -- condition. Mirrors the host exactly.
        for _, order in ipairs(self.microbial_order) do
            local proc = self.microbial_processors[order]
            local feed = proc.feed
            local oxygen = proc.oxygen
            local water = proc.water
            local consumable = proc.consumable
            proc.lastScale = 0
            proc.lastLimiter = ''
            proc.lastRequested = 0
            proc.lastFeedConsumed = 0
            proc.lastOxygenConsumed = 0
            proc.lastWaterConsumed = 0
            proc.lastConsumableRequested = 0
            proc.lastConsumableUsed = 0
            proc.lastConsumableUnmet = 0
            proc.lastNutrientProduced = 0
            proc.lastRecoveredWaterProduced = 0
            proc.lastCo2Produced = 0
            proc.lastResidueProduced = 0
            proc.lastPowerDemand = 0
            proc.lastPowerSupplied = 0
            proc.lastUnmetProcessing = 0
            proc.lastReactionHeatKW = 0
            proc.temperatureEligible = false
            proc.lastBlockReason = ''
            if proc.rateKgPerS > 0 then
                local fullKg = proc.rateKgPerS * fixed_seconds
                -- Static operating window: the culture runs at or above
                -- the minimum operating temperature and below the
                -- maximum sink temperature. Outside the window the stage
                -- is refused whole and spends nothing.
                proc.temperatureEligible = proc.sink.temperature + 1e-9
                    >= proc.minOperatingTemperatureC
                    and proc.sink.temperature + 1e-9 < proc.maxSinkTemperatureC
                if not proc.temperatureEligible then
                    proc.lastBlockReason = proc.sink.temperature + 1e-9
                        < proc.minOperatingTemperatureC
                        and 'below the minimum operating temperature: the culture is dormant'
                        or 'cooling is not qualified: sink at or above the maximum sink temperature'
                else
                    local composition = feed.composition
                    local dryPerKgFeed = 1 - composition.moisture
                    -- The recovered nutrients are funded by the feed's
                    -- bound constituents only: bound nitrogen,
                    -- phosphorus, potassium and the grouped mineral
                    -- term, each at its declared recovery fraction.
                    -- Atmospheric nitrogen is not in this sum.
                    local nutrientPerKgFeed = dryPerKgFeed * (composition.nutrientNitrogen
                        * proc.nitrogenRecovery + composition.phosphorus * proc.phosphorusRecovery
                        + composition.potassium * proc.potassiumRecovery
                        + composition.otherMinerals * proc.mineralsRecovery)
                    local recoveredWaterPerKgFeed = composition.moisture * proc.waterRecovery
                    -- The residue is the mass-balance remainder of the
                    -- declared inputs and outputs per kilogram of feed.
                    local residuePerKgFeed = 1 + proc.oxygenPerKgFeedKg
                        + proc.processWaterPerKgFeedKg - nutrientPerKgFeed
                        - recoveredWaterPerKgFeed - proc.co2PerKgFeedKg
                    local drawableFeed = math.max(0, feed.qualified - feed.protected_reserve)
                    local drawableOxygen = math.max(0,
                        oxygen.qualified - oxygen.protected_reserve)
                    local drawableWater = math.max(0, water.qualified - water.protected_reserve)
                    local drawableConsumable = math.max(0,
                        consumable.qualified - consumable.protected_reserve)
                    local nutrientHeadroom = math.max(0, proc.nutrient.capacity
                        - (proc.nutrient.qualified + proc.nutrient.unqualified))
                    local recoveredWaterHeadroom = math.max(0, proc.recoveredWater.capacity
                        - (proc.recoveredWater.qualified + proc.recoveredWater.unqualified))
                    local co2Headroom = math.max(0, proc.co2.capacity
                        - (proc.co2.qualified + proc.co2.unqualified))
                    local residueHeadroom = math.max(0, proc.residue.capacity
                        - (proc.residue.qualified + proc.residue.unqualified))
                    local scale = 1.0
                    local limiter = ''
                    local function constrain(available, required, reason)
                        if required <= 0 then return end
                        local fraction = available / required
                        if fraction < scale then
                            scale = fraction
                            limiter = reason
                        end
                    end
                    constrain(drawableFeed, fromKg(feed.unit, fullKg),
                        "organic waste feed store '" .. feed.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(drawableOxygen,
                        fromKg(oxygen.unit, fullKg * proc.oxygenPerKgFeedKg),
                        "oxygen store '" .. oxygen.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(drawableWater,
                        fromKg(water.unit, fullKg * proc.processWaterPerKgFeedKg),
                        "process water store '" .. water.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(drawableConsumable,
                        fullKg * proc.consumablePerKgFeed,
                        "consumable store '" .. consumable.id
                            .. "' has nothing drawable above its protected reserve")
                    constrain(nutrientHeadroom,
                        fromKg(proc.nutrient.unit, fullKg * nutrientPerKgFeed),
                        "nutrient store '" .. proc.nutrient.id
                            .. "' is full: the recovered nutrients have nowhere to go")
                    constrain(recoveredWaterHeadroom,
                        fromKg(proc.recoveredWater.unit, fullKg * recoveredWaterPerKgFeed),
                        "recovered water store '" .. proc.recoveredWater.id
                            .. "' is full: the recovered water has nowhere to go")
                    constrain(co2Headroom,
                        fromKg(proc.co2.unit, fullKg * proc.co2PerKgFeedKg),
                        "CO2 store '" .. proc.co2.id
                            .. "' is full: the carbon dioxide has nowhere to go")
                    constrain(residueHeadroom,
                        fromKg(proc.residue.unit, fullKg * residuePerKgFeed),
                        "residue store '" .. proc.residue.id
                            .. "' is full: the unrecoverable residue has nowhere to go")
                    if scale < 0 then scale = 0 end
                    if scale > 1 then scale = 1 end
                    proc.lastScale = scale
                    proc.lastLimiter = limiter
                    proc.lastRequested = fullKg
                    if scale > 0 then
                        local powerDemand = scale * proc.rateKgPerS
                            * proc.specificEnergyKWhPerKgFeed * 3600.0
                        proc.lastPowerDemand = powerDemand
                        local fromGeneration = math.min(powerDemand, generation)
                        generation = generation - fromGeneration
                        local fromBattery = drawBatteries(self,
                            powerDemand - fromGeneration, fixed_hours)
                        local supplied = fromGeneration + fromBattery
                        batterySupplied = batterySupplied + fromBattery
                        proc.lastPowerSupplied = supplied
                        if powerDemand > 1e-12 and supplied <= 1e-12 then
                            -- Nothing was paid: the tick is refused
                            -- whole. No feed digests, no oxygen is drawn
                            -- and no product appears.
                            proc.lastBlockReason =
                                'no electrical supply: the microbial processor cannot run'
                        else
                            local energySuppliedKJ = supplied * fixed_seconds
                            -- Energy feasibility: the processor only
                            -- digests the feed the supplied energy can
                            -- pay for at the declared dose.
                            local doseKJ = proc.specificEnergyKWhPerKgFeed * 3600.0
                            local energyFeasibleKg = doseKJ > 0
                                and energySuppliedKJ / doseKJ or fullKg * scale
                            -- The extent is capped by the inputs that
                            -- are actually drawable above their
                            -- protected reserves.
                            local extentKg = math.min(energyFeasibleKg,
                                toKg(feed.unit, drawableFeed))
                            if proc.oxygenPerKgFeedKg > 0 then
                                extentKg = math.min(extentKg,
                                    toKg(oxygen.unit, drawableOxygen) / proc.oxygenPerKgFeedKg)
                            end
                            if proc.processWaterPerKgFeedKg > 0 then
                                extentKg = math.min(extentKg,
                                    toKg(water.unit, drawableWater) / proc.processWaterPerKgFeedKg)
                            end
                            if proc.consumablePerKgFeed > 0 then
                                extentKg = math.min(extentKg,
                                    drawableConsumable / proc.consumablePerKgFeed)
                            end
                            -- Move the flows, clamped per flow so
                            -- roundoff never overshoots (#556).
                            local feedUnits = math.min(
                                fromKg(feed.unit, extentKg), drawableFeed)
                            local feedActualKg = toKg(feed.unit, feedUnits)
                            local oxygenUnits = math.min(
                                fromKg(oxygen.unit, feedActualKg * proc.oxygenPerKgFeedKg),
                                drawableOxygen)
                            local oxygenActualKg = toKg(oxygen.unit, oxygenUnits)
                            local waterUnits = math.min(
                                fromKg(water.unit, feedActualKg * proc.processWaterPerKgFeedKg),
                                drawableWater)
                            local waterActualKg = toKg(water.unit, waterUnits)
                            local consumableUnits = math.min(
                                feedActualKg * proc.consumablePerKgFeed, drawableConsumable)
                            local nutrientUnits = math.min(
                                fromKg(proc.nutrient.unit, feedActualKg * nutrientPerKgFeed),
                                nutrientHeadroom)
                            local nutrientActualKg = toKg(proc.nutrient.unit, nutrientUnits)
                            local recoveredWaterUnits = math.min(
                                fromKg(proc.recoveredWater.unit,
                                    feedActualKg * recoveredWaterPerKgFeed),
                                recoveredWaterHeadroom)
                            local recoveredWaterActualKg = toKg(proc.recoveredWater.unit,
                                recoveredWaterUnits)
                            local co2Units = math.min(
                                fromKg(proc.co2.unit, feedActualKg * proc.co2PerKgFeedKg),
                                co2Headroom)
                            local co2ActualKg = toKg(proc.co2.unit, co2Units)
                            local residueUnits = math.min(
                                fromKg(proc.residue.unit, feedActualKg * residuePerKgFeed),
                                residueHeadroom)
                            local residueActualKg = toKg(proc.residue.unit, residueUnits)
                            feed.qualified = feed.qualified - feedUnits
                            oxygen.qualified = oxygen.qualified - oxygenUnits
                            water.qualified = water.qualified - waterUnits
                            consumable.qualified = consumable.qualified - consumableUnits
                            proc.nutrient.qualified = proc.nutrient.qualified + nutrientUnits
                            proc.recoveredWater.qualified =
                                proc.recoveredWater.qualified + recoveredWaterUnits
                            proc.co2.qualified = proc.co2.qualified + co2Units
                            proc.residue.qualified = proc.residue.qualified + residueUnits
                            self.unit_loss[feed.unit] =
                                (self.unit_loss[feed.unit] or 0) + feedUnits
                            self.unit_loss[oxygen.unit] =
                                (self.unit_loss[oxygen.unit] or 0) + oxygenUnits
                            self.unit_loss[water.unit] =
                                (self.unit_loss[water.unit] or 0) + waterUnits
                            self.unit_loss[consumable.unit] =
                                (self.unit_loss[consumable.unit] or 0) + consumableUnits
                            self.unit_loss[proc.nutrient.unit] =
                                (self.unit_loss[proc.nutrient.unit] or 0) - nutrientUnits
                            self.unit_loss[proc.recoveredWater.unit] =
                                (self.unit_loss[proc.recoveredWater.unit] or 0) - recoveredWaterUnits
                            self.unit_loss[proc.co2.unit] =
                                (self.unit_loss[proc.co2.unit] or 0) - co2Units
                            self.unit_loss[proc.residue.unit] =
                                (self.unit_loss[proc.residue.unit] or 0) - residueUnits
                            -- Constituent boundary: the compound feed's
                            -- constituents leave the compound-material
                            -- world with the feed, and the plain product
                            -- stores carry none back. The recovered
                            -- share is tracked in the processor's own
                            -- per-constituent ledger; at the station
                            -- boundary the whole feed constituent set
                            -- is booked as leaving the compound stores.
                            local net = {}
                            addConstituents(net, composition, feedActualKg, -1)
                            if consumable.composition then
                                addConstituents(net, consumable.composition,
                                    toKg(consumable.unit, consumableUnits), -1)
                            end
                            for constituent, change in pairs(net) do
                                self.constituent_loss[constituent] =
                                    (self.constituent_loss[constituent] or 0) - change
                            end
                            -- Energy: the dose plus the exothermic heat
                            -- the products do not retain leave through
                            -- the sink, once.
                            local reactionHeatKJ = feedActualKg * proc.reactionHeatKJPerKgFeed
                            local heatKJ = energySuppliedKJ + reactionHeatKJ
                            proc.sink.microbialHeat =
                                (proc.sink.microbialHeat or 0) + heatKJ / fixed_seconds
                            proc.lastReactionHeatKW = reactionHeatKJ / fixed_seconds
                            -- Per-constituent recovery: each recovered
                            -- nutrient is the feed's bound content times
                            -- its recovery fraction; the atmospheric
                            -- nitrogen share never appears here.
                            proc.nitrogenRecoveredTotal = proc.nitrogenRecoveredTotal
                                + feedActualKg * dryPerKgFeed * composition.nutrientNitrogen
                                * proc.nitrogenRecovery
                            proc.phosphorusRecoveredTotal = proc.phosphorusRecoveredTotal
                                + feedActualKg * dryPerKgFeed * composition.phosphorus
                                * proc.phosphorusRecovery
                            proc.potassiumRecoveredTotal = proc.potassiumRecoveredTotal
                                + feedActualKg * dryPerKgFeed * composition.potassium
                                * proc.potassiumRecovery
                            proc.mineralsRecoveredTotal = proc.mineralsRecoveredTotal
                                + feedActualKg * dryPerKgFeed * composition.otherMinerals
                                * proc.mineralsRecovery
                            proc.lastFeedConsumed = feedActualKg
                            proc.lastOxygenConsumed = oxygenActualKg
                            proc.lastWaterConsumed = waterActualKg
                            proc.lastConsumableRequested = scale * fullKg * proc.consumablePerKgFeed
                            proc.lastConsumableUsed = consumableUnits
                            proc.lastConsumableUnmet = proc.lastConsumableRequested - consumableUnits
                            proc.lastNutrientProduced = nutrientActualKg
                            proc.lastRecoveredWaterProduced = recoveredWaterActualKg
                            proc.lastCo2Produced = co2ActualKg
                            proc.lastResidueProduced = residueActualKg
                            proc.lastUnmetProcessing = math.max(0, fullKg * scale - feedActualKg)
                            proc.feedConsumedTotal = proc.feedConsumedTotal + feedActualKg
                            proc.oxygenConsumedTotal = proc.oxygenConsumedTotal + oxygenActualKg
                            proc.waterConsumedTotal = proc.waterConsumedTotal + waterActualKg
                            proc.consumablesUsedTotal = proc.consumablesUsedTotal + consumableUnits
                            proc.nutrientProducedTotal = proc.nutrientProducedTotal + nutrientActualKg
                            proc.recoveredWaterTotal = proc.recoveredWaterTotal + recoveredWaterActualKg
                            proc.co2ProducedTotal = proc.co2ProducedTotal + co2ActualKg
                            proc.residueProducedTotal = proc.residueProducedTotal + residueActualKg
                            proc.energySuppliedKJ = proc.energySuppliedKJ + energySuppliedKJ
                            proc.reactionHeatKJ = proc.reactionHeatKJ + reactionHeatKJ
                            proc.heatRoutedKJ = proc.heatRoutedKJ + heatKJ
                        end
                    end
                end
            end
            assert(isFinite(proc.lastScale) and isFinite(proc.lastRequested)
                and isFinite(proc.lastFeedConsumed) and isFinite(proc.lastOxygenConsumed)
                and isFinite(proc.lastWaterConsumed) and isFinite(proc.lastConsumableRequested)
                and isFinite(proc.lastConsumableUsed) and isFinite(proc.lastConsumableUnmet)
                and isFinite(proc.lastNutrientProduced) and isFinite(proc.lastRecoveredWaterProduced)
                and isFinite(proc.lastCo2Produced) and isFinite(proc.lastResidueProduced)
                and isFinite(proc.lastPowerDemand) and isFinite(proc.lastPowerSupplied)
                and isFinite(proc.lastUnmetProcessing) and isFinite(proc.lastReactionHeatKW)
                and isFinite(proc.feedConsumedTotal) and isFinite(proc.oxygenConsumedTotal)
                and isFinite(proc.waterConsumedTotal) and isFinite(proc.consumablesUsedTotal)
                and isFinite(proc.nutrientProducedTotal) and isFinite(proc.recoveredWaterTotal)
                and isFinite(proc.co2ProducedTotal) and isFinite(proc.residueProducedTotal)
                and isFinite(proc.nitrogenRecoveredTotal) and isFinite(proc.phosphorusRecoveredTotal)
                and isFinite(proc.potassiumRecoveredTotal) and isFinite(proc.mineralsRecoveredTotal)
                and isFinite(proc.energySuppliedKJ) and isFinite(proc.reactionHeatKJ)
                and isFinite(proc.heatRoutedKJ),
                "microbial processor '" .. proc.id .. "' result is not finite")
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
            local collectorHeat = loop.collectorHeat or 0
            loop.collectorHeat = 0
            if not disabled then
                local heatIntoLoop = loop.lastPumpSupplied + loop.lastHeaterSupplied
                local loopCapacityKJ = loop.coolant * loop.cp
                loop.temperature = loop.temperature
                    + (heatIntoLoop + loop.lastExchange + collectorHeat - extraction)
                        * fixed_seconds / loopCapacityKJ
                loop.pumpWorkKJ = loop.pumpWorkKJ + loop.lastPumpSupplied * fixed_seconds
                loop.heaterKJ = loop.heaterKJ + loop.lastHeaterSupplied * fixed_seconds
                loop.exchangedKJ = loop.exchangedKJ + loop.lastExchange * fixed_seconds
                loop.extractedKJ = (loop.extractedKJ or 0) + extraction * fixed_seconds
                loop.collectorKJ = (loop.collectorKJ or 0) + collectorHeat * fixed_seconds
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
        -- Water treatment work rides the same pool (#402): it is never a
        -- third budget beside the loads and the thermal plant.
        for _, treatment in ipairs(self.water_treatments) do
            totalSupplied = totalSupplied + treatment.lastPowerSupplied
        end
        -- Electrolysis and CO2 capture work ride the same pool (#404):
        -- oxygen production and carbon removal are services, not a
        -- second budget beside the loads and the thermal plant.
        for _, cell in ipairs(self.electrolyzers) do
            totalSupplied = totalSupplied + cell.lastPowerSupplied
        end
        for _, capture in ipairs(self.co2_captures) do
            totalSupplied = totalSupplied + capture.lastPowerSupplied
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
            -- Electrolysis process heat enters the sink reservoir exactly
            -- once (#404): the cell booked the part of its electrical dose
            -- the split did not retain as chemical energy here in
            -- applyPower, and the reservoir integrates it as its own
            -- ledger term.
            local electrolysisHeat = reservoir.electrolysisHeat or 0
            reservoir.electrolysisHeat = 0
            -- Fuel-cell waste heat enters the sink reservoir exactly once
            -- (#406): the cell booked the chemical energy it did not
            -- deliver as electricity here in applyPower, and the
            -- reservoir integrates it as its own ledger term.
            local fuelCellHeat = reservoir.fuelCellHeat or 0
            reservoir.fuelCellHeat = 0
            -- Sabatier heat enters the sink reservoir exactly once (#405):
            -- the reactor booked its electrical dose plus the exothermic
            -- reaction heat the products do not retain here in
            -- applyPower, and the reservoir integrates it as its own
            -- ledger term.
            local sabatierHeat = reservoir.sabatierHeat or 0
            reservoir.sabatierHeat = 0
            -- Methane-processing waste heat enters the sink reservoir
            -- exactly once (#405): the processor booked the part of its
            -- electrical dose the endothermic split did not absorb here
            -- in applyPower.
            local methaneHeat = reservoir.methaneHeat or 0
            reservoir.methaneHeat = 0
            -- Microbial digestion heat enters the sink reservoir exactly
            -- once (#407): the processor booked its electrical dose plus
            -- the exothermic reaction heat the products do not retain
            -- here in applyPower, and the reservoir integrates it as its
            -- own ledger term.
            local microbialHeat = reservoir.microbialHeat or 0
            reservoir.microbialHeat = 0
            local atmosphereHeat = reservoir.atmosphereHeat or 0
            reservoir.atmosphereHeat = 0
            local heatIn = reservoir.absorbedKW + routed + loopHeat + condenser
                + generatorHeat + atmosphereHeat + electrolysisHeat + fuelCellHeat
                + sabatierHeat + methaneHeat + microbialHeat
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
            -- Phase-change buffers ride through the deficit (#398): the
            -- running imbalance after rejection is what the buffers
            -- hosted on this reservoir see. A charge moves heat from the
            -- reservoir into the buffer's finite store, capped by the
            -- remaining deficit, the configured rate limit, the store's
            -- headroom and the thermodynamic direction (the reservoir
            -- must be hotter than the buffer). A discharge returns
            -- stored heat into the reservoir, capped by the remaining
            -- rejection surplus, the rate limit, the stored energy and
            -- the direction (the buffer must be hotter). Recovery thus
            -- consumes real rejection capacity: the store can only
            -- reset as fast as the rejection path can carry the heat
            -- away, never instantly. A circulation-access buffer is
            -- unreachable while its gating loop is not circulating, so
            -- a blackout cannot absorb through a powered-only store for
            -- free. Buffers run in declaration order and the running
            -- balance carries through them, so two buffers on one host
            -- never absorb the same deficit twice.
            local remaining = heatIn - rejected
            local bufferCharge = 0
            local bufferDischarge = 0
            for b = 1, #reservoir.buffers do
                local buffer = reservoir.buffers[b]
                local band = buffer.maxUsable - buffer.minUsable
                local bufferTemperature = buffer.minUsable
                    + buffer.stored * band / buffer.capacity
                local charge = 0
                local discharge = 0
                local limiter = ''
                local accessible = buffer.accessMode ~= 'circulation'
                    or (buffer.gateLoop ~= nil and buffer.gateLoop.circulating)
                if not accessible then
                    limiter = 'circulation lost: the powered buffer is unreachable'
                elseif remaining > 0 then
                    if reservoir.temperature > bufferTemperature then
                        local limit = buffer.maxCharge
                        if buffer.accessMode == 'passive' then
                            limit = math.min(limit, buffer.conductance
                                * (reservoir.temperature - bufferTemperature))
                        end
                        local headroom = (buffer.capacity - buffer.stored) / fixed_seconds
                        charge = math.min(limit, remaining, headroom)
                        if charge <= 0 then
                            limiter = 'buffer is full: no headroom left to absorb'
                        elseif charge < remaining then
                            if headroom <= charge + 1e-12 then
                                limiter = 'buffer headroom: the store is filling up'
                            else
                                limiter = 'buffer charge rate limit'
                            end
                        end
                    else
                        limiter = 'buffer is not colder than the reservoir: no charge direction'
                    end
                elseif remaining < 0 then
                    if bufferTemperature > reservoir.temperature then
                        local limit = buffer.maxDischarge
                        if buffer.accessMode == 'passive' then
                            limit = math.min(limit, buffer.conductance
                                * (bufferTemperature - reservoir.temperature))
                        end
                        local drawable = buffer.stored / fixed_seconds
                        discharge = math.min(limit, -remaining, drawable)
                        if discharge <= 0 then
                            limiter = 'buffer is depleted: nothing left to return'
                        elseif discharge < -remaining then
                            if drawable <= discharge + 1e-12 then
                                limiter = 'buffer store: nearly nothing left to return'
                            else
                                limiter = 'buffer discharge rate limit'
                            end
                        end
                    else
                        limiter = 'buffer is not hotter than the reservoir: no discharge direction'
                    end
                end
                buffer.stored = buffer.stored + (charge - discharge) * fixed_seconds
                -- Roundoff never leaves the finite bounds: the store is
                -- clamped to [0, capacity] so a one-ulp overshoot
                -- cannot accumulate (#556 discipline).
                if buffer.stored < 0 then buffer.stored = 0 end
                if buffer.stored > buffer.capacity then buffer.stored = buffer.capacity end
                buffer.chargedKJ = buffer.chargedKJ + charge * fixed_seconds
                buffer.dischargedKJ = buffer.dischargedKJ + discharge * fixed_seconds
                buffer.accessible = accessible
                buffer.lastCharge = charge
                buffer.lastDischarge = discharge
                buffer.lastLimiter = limiter
                buffer.full = buffer.stored >= buffer.capacity - 1e-9
                buffer.depleted = buffer.stored <= 1e-9
                remaining = remaining - charge + discharge
                bufferCharge = bufferCharge + charge
                bufferDischarge = bufferDischarge + discharge
            end
            local net = remaining
            local newTemperature = reservoir.temperature + net * fixed_seconds / capacity
            reservoir.lastRouted = routed
            reservoir.lastBufferCharge = bufferCharge
            reservoir.lastBufferDischarge = bufferDischarge
            reservoir.lastLoopHeat = loopHeat
            reservoir.lastHeatIn = heatIn
            reservoir.lastRejected = rejected
            reservoir.lastNet = net
            reservoir.absorbedKJ = reservoir.absorbedKJ + reservoir.absorbedKW * fixed_seconds
            reservoir.routedKJ = reservoir.routedKJ + routed * fixed_seconds
            reservoir.loopKJ = (reservoir.loopKJ or 0) + loopHeat * fixed_seconds
            reservoir.condenserKJ = (reservoir.condenserKJ or 0) + condenser * fixed_seconds
            reservoir.generatorKJ = (reservoir.generatorKJ or 0) + generatorHeat * fixed_seconds
            reservoir.electrolysisKJ = (reservoir.electrolysisKJ or 0) + electrolysisHeat * fixed_seconds
            reservoir.fuelCellKJ = (reservoir.fuelCellKJ or 0) + fuelCellHeat * fixed_seconds
            reservoir.sabatierKJ = (reservoir.sabatierKJ or 0) + sabatierHeat * fixed_seconds
            reservoir.methaneProcessingKJ = (reservoir.methaneProcessingKJ or 0)
                + methaneHeat * fixed_seconds
            reservoir.microbialKJ = (reservoir.microbialKJ or 0)
                + microbialHeat * fixed_seconds
            reservoir.atmosphereKJ = (reservoir.atmosphereKJ or 0) + atmosphereHeat * fixed_seconds
            reservoir.bufferChargeKJ = (reservoir.bufferChargeKJ or 0)
                + bufferCharge * fixed_seconds
            reservoir.bufferDischargeKJ = (reservoir.bufferDischargeKJ or 0)
                + bufferDischarge * fixed_seconds
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
                + (reservoir.electrolysisKJ or 0) + (reservoir.fuelCellKJ or 0)
                + (reservoir.sabatierKJ or 0) + (reservoir.methaneProcessingKJ or 0)
                + (reservoir.microbialKJ or 0)
                + (reservoir.atmosphereKJ or 0)
                + (reservoir.bufferDischargeKJ or 0) - (reservoir.bufferChargeKJ or 0)
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
        -- Thermal buffer storage ledger (#398): the store's starting
        -- energy plus what it charged minus what it discharged must
        -- equal the stored energy within the declared tolerance: energy
        -- enters or leaves the store only through the booked exchange,
        -- never a silent reset or an unexplained drain. The store never
        -- leaves its finite bounds, and a charge or discharge beyond
        -- the configured limits is a conservation violation, never a
        -- silent rounding.
        for _, buffer in ipairs(self.thermal_buffers) do
            local bufferBound = BALANCE_TOLERANCE * math.max(1,
                math.max(math.abs(buffer.startingEnergy),
                    math.max(math.abs(buffer.chargedKJ), math.abs(buffer.dischargedKJ))))
            local residual = buffer.startingEnergy + buffer.chargedKJ
                - buffer.dischargedKJ - buffer.stored
            assert(residual == residual and math.abs(residual) <= bufferBound,
                "thermal buffer '" .. buffer.id .. "' storage ledger residual "
                .. 'exceeds tolerance')
            assert(buffer.stored >= -BALANCE_TOLERANCE
                and buffer.stored <= buffer.capacity + BALANCE_TOLERANCE,
                "thermal buffer '" .. buffer.id .. "' stored energy left its finite bounds")
            assert(buffer.chargedKJ >= -BALANCE_TOLERANCE
                and buffer.dischargedKJ >= -BALANCE_TOLERANCE,
                "thermal buffer '" .. buffer.id .. "' exchange ledger went negative")
            local values = { buffer.stored, buffer.lastCharge, buffer.lastDischarge,
                buffer.chargedKJ, buffer.dischargedKJ }
            for _, value in ipairs(values) do
                assert(isFinite(value),
                    "thermal buffer '" .. buffer.id .. "' value is not finite")
            end
        end
        for _, loop in ipairs(self.coolant_loops) do
            assert(loop.coolant >= 0 and loop.coolant == loop.coolant
                and loop.coolant <= loop.capacity + BALANCE_TOLERANCE
                    * math.max(1, loop.capacity),
                "coolant loop '" .. loop.id .. "' inventory left its finite bounds")
            local startingEnergy = loop.startingCoolant * loop.cp * loop.startingTemperature
            local bound = BALANCE_TOLERANCE * math.max(1, math.abs(startingEnergy))
            local residual = startingEnergy + loop.pumpWorkKJ + loop.heaterKJ
                + loop.exchangedKJ + (loop.collectorKJ or 0) - (loop.extractedKJ or 0)
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
        -- #565: every published thermal/class rollup is a sum of
        -- individually finite terms; the aggregate must itself be
        -- finite before the snapshot that publishes it can commit.
        -- Two finite 1e308 flows or ratings sum to infinity and pass
        -- every inequality audit, so the sums are checked explicitly.
        local absorbedTotal, routedTotal, rejectedTotal = 0, 0, 0
        for _, reservoir in ipairs(self.reservoirs) do
            absorbedTotal = absorbedTotal + reservoir.absorbedKW
            routedTotal = routedTotal + reservoir.lastRouted
            rejectedTotal = rejectedTotal + reservoir.lastRejected
        end
        local circulationTotal, freezeHeatingTotal, pumpPowerTotal = 0, 0, 0
        for _, loop in ipairs(self.coolant_loops) do
            circulationTotal = circulationTotal + loop.lastFlow
            freezeHeatingTotal = freezeHeatingTotal + loop.lastHeaterSupplied
            pumpPowerTotal = pumpPowerTotal + loop.lastPumpSupplied
        end
        local coolingProvidedTotal, coolingUnmetTotal = 0, 0
        local condenserHeatTotal, chillerPowerTotal = 0, 0
        local classRollups = {}
        for _, chiller in ipairs(self.chillers) do
            coolingProvidedTotal = coolingProvidedTotal + chiller.lastCooling
            coolingUnmetTotal = coolingUnmetTotal + (chiller.lastDemand - chiller.lastCooling)
            condenserHeatTotal = condenserHeatTotal + chiller.lastCondenserKW
            chillerPowerTotal = chillerPowerTotal + chiller.lastPowerSupplied
            local rollup = classRollups[chiller.thermalClass]
            if rollup == nil then
                rollup = { capacity = 0, provided = 0, unmet = 0, work = 0, condenser = 0 }
                classRollups[chiller.thermalClass] = rollup
            end
            rollup.capacity = rollup.capacity + chiller.ratedKW
            rollup.provided = rollup.provided + chiller.lastCooling
            rollup.unmet = rollup.unmet + (chiller.lastDemand - chiller.lastCooling)
            rollup.work = rollup.work + chiller.lastPowerSupplied
            rollup.condenser = rollup.condenser + chiller.lastCondenserKW
        end
        local generatorOutputTotal, generatorStartupTotal = 0, 0
        local generatorWasteTotal, generatorDecayTotal = 0, 0
        local bufferStoredTotal, bufferAvailableTotal = 0, 0
        local bufferChargeTotal, bufferDischargeTotal = 0, 0
        for _, buffer in ipairs(self.thermal_buffers) do
            bufferStoredTotal = bufferStoredTotal + buffer.stored
            bufferAvailableTotal = bufferAvailableTotal + (buffer.capacity - buffer.stored)
            bufferChargeTotal = bufferChargeTotal + buffer.lastCharge
            bufferDischargeTotal = bufferDischargeTotal + buffer.lastDischarge
        end
        for _, gen in ipairs(self.generators) do
            generatorOutputTotal = generatorOutputTotal + gen.outputKW
            generatorStartupTotal = generatorStartupTotal + gen.lastStartupDrawKW
            generatorWasteTotal = generatorWasteTotal + gen.lastWasteHeatKW
            generatorDecayTotal = generatorDecayTotal + gen.lastDecayHeatKW
        end
        local aggregates = { absorbedTotal, routedTotal, rejectedTotal,
            circulationTotal, freezeHeatingTotal, pumpPowerTotal,
            coolingProvidedTotal, coolingUnmetTotal, condenserHeatTotal,
            chillerPowerTotal, generatorOutputTotal, generatorStartupTotal,
            generatorWasteTotal, generatorDecayTotal,
            bufferStoredTotal, bufferAvailableTotal,
            bufferChargeTotal, bufferDischargeTotal }
        for _, value in ipairs(aggregates) do
            assert(isFinite(value), 'station thermal rollup aggregate is not finite')
        end
        for thermalClass, rollup in pairs(classRollups) do
            assert(isFinite(rollup.capacity) and isFinite(rollup.provided)
                and isFinite(rollup.unmet) and isFinite(rollup.work)
                and isFinite(rollup.condenser),
                "cold-service class '" .. thermalClass .. "' rollup is not finite")
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
                { 'oxygen', compartment.startingOxygen,
                    compartment.electrolysisOxygen or 0,
                    compartment.oxygenConsumed + compartment.leakOxygen
                        + compartment.cycleOxygen,
                    compartment.oxygen },
                { 'nitrogen', compartment.startingNitrogen, 0,
                    compartment.leakNitrogen + compartment.cycleNitrogen,
                    compartment.nitrogen },
                { 'carbonDioxide', compartment.startingCarbonDioxide, compartment.co2Produced,
                    compartment.leakCarbonDioxide + compartment.cycleCarbonDioxide
                        + (compartment.capturedCarbonDioxide or 0),
                    compartment.carbonDioxide },
                { 'waterVapour', compartment.startingWaterVapour, compartment.vapourAdded,
                    compartment.condensed + compartment.leakWaterVapour
                        + compartment.cycleWaterVapour + (compartment.collected or 0),
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
            -- Thermal closure: starting + occupant heat + crew heat
            -- (#401) + latent heat + exchanged from the reservoir must
            -- equal the compartment's current capacity x temperature
            -- within the tolerance.
            local startingEnergy = compartment.capacity * compartment.startingTemperature
            local thermalBound = BALANCE_TOLERANCE * math.max(1, math.abs(startingEnergy))
            local thermalResidual = startingEnergy + compartment.occupantHeatKJ
                + compartment.crewHeatKJ
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
                compartment.crewHeatKJ,
                compartment.latentHeatKJ, compartment.exchangedKJ }
            for _, value in ipairs(values) do
                assert(isFinite(value),
                    "atmosphere '" .. compartment.id .. "' value is not finite")
            end
            -- #565: the published derived magnitudes - the partial
            -- pressures, the absolute pressure and the saturation
            -- vapour capacity - are computed from this state by the
            -- snapshot; a finite inventory in a tiny volume can
            -- overflow the ideal-gas division, so every derived
            -- published magnitude is audited finite here too.
            local oxygenKPa = partialPressureKPa(compartment.oxygen, MOLAR_MASS_OXYGEN,
                compartment.volumeM3, compartment.temperature)
            local nitrogenKPa = partialPressureKPa(compartment.nitrogen, MOLAR_MASS_NITROGEN,
                compartment.volumeM3, compartment.temperature)
            local carbonDioxideKPa = partialPressureKPa(compartment.carbonDioxide,
                MOLAR_MASS_CARBON_DIOXIDE, compartment.volumeM3, compartment.temperature)
            local vapourKPa = partialPressureKPa(compartment.waterVapour, MOLAR_MASS_WATER,
                compartment.volumeM3, compartment.temperature)
            assert(isFinite(oxygenKPa) and isFinite(nitrogenKPa)
                and isFinite(carbonDioxideKPa) and isFinite(vapourKPa)
                and isFinite(oxygenKPa + nitrogenKPa + carbonDioxideKPa + vapourKPa)
                and isFinite(saturationVapourPressureKPa(compartment.temperature)),
                "atmosphere '" .. compartment.id .. "' derived pressure is not finite")
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
    -- Crew ledger audit (#401): the crew's own account closes over its
    -- pathways - consumed (food + water + oxygen) equals returned (CO2
    -- + moisture + urine + organic waste) plus retained body mass
    -- within the declared tolerance - the retained mass is never
    -- negative (the crew cannot emit more than it consumed), and the
    -- metabolic scale stays within [0, 1]. Store- and constituent-level
    -- closure of the crew's draws and returns is enforced by the shared
    -- unit and constituent boundary audits; this closes the crew's own
    -- consumption/return/retained account.
    local function checkCrews(self)
        for _, crew in ipairs(self.crews) do
            assert(isFinite(crew.foodConsumed) and isFinite(crew.waterConsumed)
                and isFinite(crew.oxygenConsumed) and isFinite(crew.co2Produced)
                and isFinite(crew.moistureProduced) and isFinite(crew.urineProduced)
                and isFinite(crew.organicWasteProduced) and isFinite(crew.heatKJ)
                and isFinite(crew.retainedKg) and isFinite(crew.lastScale)
                and isFinite(crew.lastActivity),
                "crew '" .. crew.id .. "' ledger is not finite")
            assert(crew.lastScale >= 0 and crew.lastScale <= 1 + 1e-12,
                "crew '" .. crew.id .. "' metabolic scale left the declared range")
            local consumed = toKg(crew.food.unit, crew.foodConsumed)
                + toKg(crew.water.unit, crew.waterConsumed) + crew.oxygenConsumed
            local returned = toKg(crew.urine.unit, crew.urineProduced)
                + toKg(crew.organicWaste.unit, crew.organicWasteProduced)
                + crew.co2Produced + crew.moistureProduced
            local bound = BALANCE_TOLERANCE * math.max(1, consumed)
            assert(crew.retainedKg >= -bound,
                "crew '" .. crew.id .. "' retained body mass is negative")
            assert(math.abs(crew.retainedKg - (consumed - returned)) <= bound,
                "crew '" .. crew.id .. "' consumed/returned/retained ledger does not close")
        end
    end
    -- Water recovery ledger audit (#402): the collector may never take
    -- more condensate than the compartment held above its humidity target,
    -- and the treatment's own account closes twice over - processed mass
    -- leaves the machine as treated water plus concentrate and nothing
    -- else, and every treated kilogram was either qualified or held, never
    -- both and never neither. The treatment is never supplied more energy
    -- than the process owed, and its scale stays inside the declared range.
    local function checkWater(self)
        for _, collector in ipairs(self.water_collectors) do
            assert(isFinite(collector.lastRequested) and isFinite(collector.lastCondensable)
                and isFinite(collector.lastCollected) and isFinite(collector.lastLatentHeatKW)
                and isFinite(collector.collectedTotal) and isFinite(collector.latentHeatKJ),
                "water collector '" .. collector.id .. "' ledger is not finite")
            local bound = BALANCE_TOLERANCE * math.max(1, collector.lastCondensable)
            assert(collector.lastCollected >= -bound,
                "water collector '" .. collector.id .. "' collected a negative mass")
            assert(collector.lastCollected <= collector.lastCondensable + bound,
                "water collector '" .. collector.id
                    .. "' collected more than the compartment held above its humidity target")
        end
        for _, treatment in ipairs(self.water_treatments) do
            assert(isFinite(treatment.lastScale) and isFinite(treatment.processedTotal)
                and isFinite(treatment.treatedTotal) and isFinite(treatment.acceptedTotal)
                and isFinite(treatment.heldTotal) and isFinite(treatment.concentrateTotal)
                and isFinite(treatment.energySuppliedKJ) and isFinite(treatment.energyOwedKJ)
                and isFinite(treatment.consumablesUsedTotal),
                "water treatment '" .. treatment.id .. "' ledger is not finite")
            assert(treatment.lastScale >= 0 and treatment.lastScale <= 1 + 1e-12,
                "water treatment '" .. treatment.id
                    .. "' processing scale left the declared [0, 1] range")
            local bound = BALANCE_TOLERANCE * math.max(1, treatment.processedTotal)
            local split = treatment.processedTotal - treatment.treatedTotal
                - treatment.concentrateTotal
            assert(isFinite(split) and math.abs(split) <= bound,
                "water treatment '" .. treatment.id
                    .. "' processed/treated/concentrate ledger does not close")
            local quality = treatment.treatedTotal - treatment.acceptedTotal
                - treatment.heldTotal
            assert(isFinite(quality) and math.abs(quality) <= bound,
                "water treatment '" .. treatment.id
                    .. "' accepted/held quality ledger does not close")
            assert(treatment.energySuppliedKJ <= treatment.energyOwedKJ + bound,
                "water treatment '" .. treatment.id
                    .. "' was supplied more energy than the process owed")
        end
    end
    -- Fuel-cell backup ledger audit (#406): the reaction closes on mass -
    -- hydrogen + oxygen = water - and on energy - chemical = electrical +
    -- waste. A cell never delivers electrical energy it has not burned
    -- reactants for, never delivers above its installed rating, and the
    -- endurance clock never counts a negative remaining supply.
    local function checkFuelCells(self)
        for _, cell in ipairs(self.fuel_cells) do
            assert(isFinite(cell.lastOutputKW) and isFinite(cell.lastHydrogenKg)
                and isFinite(cell.lastOxygenKg) and isFinite(cell.lastWaterKg)
                and isFinite(cell.lastWasteHeatKW) and isFinite(cell.lastUnmetOutputKW)
                and isFinite(cell.hydrogenConsumedKg) and isFinite(cell.oxygenConsumedKg)
                and isFinite(cell.waterProducedKg) and isFinite(cell.chemicalEnergyKJ)
                and isFinite(cell.electricalKJ) and isFinite(cell.wasteHeatKJ)
                and isFinite(cell.enduranceSeconds),
                "fuel cell '" .. cell.id .. "' ledger is not finite")
            local bound = BALANCE_TOLERANCE * math.max(1, cell.waterProducedKg)
            local massResidual = cell.hydrogenConsumedKg + cell.oxygenConsumedKg
                - cell.waterProducedKg
            assert(math.abs(massResidual) <= bound,
                "fuel cell '" .. cell.id
                    .. "' hydrogen/oxygen/water reaction ledger does not close")
            local energyResidual = cell.chemicalEnergyKJ - cell.electricalKJ
                - cell.wasteHeatKJ
            assert(math.abs(energyResidual) <= bound,
                "fuel cell '" .. cell.id
                    .. "' chemical/electrical/waste energy ledger does not close")
            assert(cell.electricalKJ <= cell.chemicalEnergyKJ + bound,
                "fuel cell '" .. cell.id
                    .. "' delivered more electrical energy than the reactants' chemical energy")
            assert(cell.lastOutputKW <= cell.ratedOutputKW + BALANCE_TOLERANCE,
                "fuel cell '" .. cell.id .. "' delivered above the installed rating")
            assert(not (cell.lastOutputKW > 1e-9 and cell.lastHydrogenKg <= 1e-12),
                "fuel cell '" .. cell.id .. "' delivered power without consuming any hydrogen")
            assert(cell.enduranceSeconds >= 0,
                "fuel cell '" .. cell.id .. "' endurance clock is negative")
        end
    end
    -- Nutrient recovery ledger audit (#407): the digestion closes on
    -- mass - feed + oxygen + process water = nutrients + recovered water
    -- + CO2 + residue - with the residue as the balance term, and its
    -- energy ledger closes over supplied + released = routed. The
    -- released reaction heat never exceeds the consumed feedstock's
    -- declared chemical energy, and the recovered per-constituent
    -- totals never exceed the feed's bound content - the atmospheric
    -- nitrogen share is never recovered.
    local function checkMicrobial(self)
        for _, proc in ipairs(self.microbial_processors) do
            assert(isFinite(proc.lastScale) and isFinite(proc.lastRequested)
                and isFinite(proc.lastFeedConsumed) and isFinite(proc.lastOxygenConsumed)
                and isFinite(proc.lastWaterConsumed) and isFinite(proc.lastConsumableUsed)
                and isFinite(proc.lastNutrientProduced) and isFinite(proc.lastRecoveredWaterProduced)
                and isFinite(proc.lastCo2Produced) and isFinite(proc.lastResidueProduced)
                and isFinite(proc.lastPowerDemand) and isFinite(proc.lastPowerSupplied)
                and isFinite(proc.lastUnmetProcessing) and isFinite(proc.lastReactionHeatKW)
                and isFinite(proc.feedConsumedTotal) and isFinite(proc.oxygenConsumedTotal)
                and isFinite(proc.waterConsumedTotal) and isFinite(proc.consumablesUsedTotal)
                and isFinite(proc.nutrientProducedTotal) and isFinite(proc.recoveredWaterTotal)
                and isFinite(proc.co2ProducedTotal) and isFinite(proc.residueProducedTotal)
                and isFinite(proc.nitrogenRecoveredTotal) and isFinite(proc.phosphorusRecoveredTotal)
                and isFinite(proc.potassiumRecoveredTotal) and isFinite(proc.mineralsRecoveredTotal)
                and isFinite(proc.energySuppliedKJ) and isFinite(proc.reactionHeatKJ)
                and isFinite(proc.heatRoutedKJ),
                "microbial processor '" .. proc.id .. "' ledger is not finite")
            assert(proc.lastScale >= -BALANCE_TOLERANCE
                and proc.lastScale <= 1 + BALANCE_TOLERANCE,
                "microbial processor '" .. proc.id
                    .. "' processing scale left the declared [0, 1] range")
            local bound = BALANCE_TOLERANCE * math.max(1,
                proc.nutrientProducedTotal + proc.recoveredWaterTotal
                    + proc.co2ProducedTotal + proc.residueProducedTotal)
            local massResidual = proc.feedConsumedTotal + proc.oxygenConsumedTotal
                + proc.waterConsumedTotal - proc.nutrientProducedTotal
                - proc.recoveredWaterTotal - proc.co2ProducedTotal
                - proc.residueProducedTotal
            assert(math.abs(massResidual) <= bound,
                "microbial processor '" .. proc.id
                    .. "' feed/oxygen/water/nutrient/water/CO2/residue digestion ledger does not close")
            local energyResidual = proc.energySuppliedKJ + proc.reactionHeatKJ
                - proc.heatRoutedKJ
            assert(math.abs(energyResidual) <= bound,
                "microbial processor '" .. proc.id
                    .. "' supplied/released/routed energy ledger does not close")
            local composition = proc.feed.composition
            local feedEnergyKJ = proc.feedConsumedTotal * (1 - composition.moisture)
                * composition.energyKJPerKgDry
            local energyBound = BALANCE_TOLERANCE * math.max(1, math.abs(feedEnergyKJ))
            assert(proc.reactionHeatKJ <= feedEnergyKJ + energyBound,
                "microbial processor '" .. proc.id
                    .. "' released more reaction heat than the consumed feedstock's declared chemical energy")
            local boundNitrogenKg = proc.feedConsumedTotal * (1 - composition.moisture)
                * composition.nutrientNitrogen
            local nitrogenBound = BALANCE_TOLERANCE * math.max(1, math.abs(boundNitrogenKg))
            assert(proc.nitrogenRecoveredTotal <= boundNitrogenKg + nitrogenBound,
                "microbial processor '" .. proc.id
                    .. "' recovered more nitrogen than the consumed feed's bound nutrient nitrogen carries")
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
        checkCrews(self)
        checkWater(self)
        checkFuelCells(self)
        checkMicrobial(self)
        self.ticks = self.ticks + 1
        return self.ticks
    end
    checkFiniteLimits(object)
    checkConservation(object)
    checkConstituentConservation(object)
    checkThermal(object)
    checkAtmospheres(object)
    checkCrews(object)
    checkWater(object)
    checkFuelCells(object)
    checkMicrobial(object)
    return object
end
