-- Station systems slice 5: lifecycle, authored finite Resource stores,
-- bounded authored transfers, compound-resource conversions, solar
-- generation, External equipment load supply and rechargeable battery
-- banks. Every system executes this factory in its own budgeted private
-- sandbox.
-- Stores, transfer rules, conversion rules, solar arrays, equipment
-- load contracts and battery descriptors arrive as plain immutable
-- descriptor values; each object initialises its own copy and re-checks
-- the finite-capacity, per-unit conservation, per-constituent compound
-- conservation, power-allocation and battery energy-conservation
-- invariants every tick. The host applies the authoritative ledger;
-- this mirror audits the same rules inside the sandbox so a corrupt
-- crossing or model drift fails contained through the integrated error
-- path.
local BALANCE_TOLERANCE = 1e-9

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

return function(name, stores, transfers, conversions, solar, loads, batteries)
    assert(type(name) == "string")
    assert(type(stores) == "table", "resource store descriptors missing")
    assert(type(transfers) == "table", "resource transfer descriptors missing")
    assert(type(conversions) == "table", "resource conversion descriptors missing")
    assert(type(solar) == "table", "solar array descriptors missing")
    assert(type(loads) == "table", "equipment load descriptors missing")
    assert(type(batteries) == "table", "battery descriptors missing")
    local object = {
        ticks = 0, stores = {}, transfers = {}, order = {},
        conversions = {}, conversion_order = {},
        starting_totals = {}, unit_loss = {},
        starting_constituents = {}, constituent_loss = {},
        solar = {}, loads = {}, load_order = {}, batteries = {},
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
        for index = 1, #self.stores do
            local store = self.stores[index]
            assert(store.qualified >= 0 and store.unqualified >= 0,
                "resource store inventory went negative")
            assert(store.qualified + store.unqualified <= store.capacity,
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
            local residual = (totals[unit] or 0)
                + (self.unit_loss[unit] or 0) - starting
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
                + (self.constituent_loss[constituent] or 0) - starting
            local bound = BALANCE_TOLERANCE * math.max(1.0, math.abs(starting))
            assert(residual == residual and math.abs(residual) <= bound,
                "constituent conservation residual for '" .. constituent .. "' exceeds tolerance")
        end
    end
    -- Mirror of the host ledger: same deterministic order, same limits.
    -- Unmet material never moves and is never deleted; shortages are valid
    -- outcomes, while a conservation violation is a software failure.
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
                    local completed = flow.rate * fixed_seconds * scale
                    flow.store.qualified = flow.store.qualified - completed
                    addConstituents(net, flow.store.composition,
                        toKg(flow.store.unit, completed), -1)
                end
                for _, flow in ipairs(conversion.outputs) do
                    local completed = flow.rate * fixed_seconds * scale
                    flow.store.qualified = flow.store.qualified + completed
                    addConstituents(net, flow.store.composition,
                        toKg(flow.store.unit, completed), 1)
                end
                for constituent, change in pairs(net) do
                    self.constituent_loss[constituent] =
                        (self.constituent_loss[constituent] or 0) - change
                end
                for _, flow in ipairs(conversion.inputs) do
                    self.unit_loss[flow.store.unit] =
                        (self.unit_loss[flow.store.unit] or 0) + flow.rate * fixed_seconds * scale
                end
                for _, flow in ipairs(conversion.outputs) do
                    self.unit_loss[flow.store.unit] =
                        (self.unit_loss[flow.store.unit] or 0) - flow.rate * fixed_seconds * scale
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
        end
        local availableGeneration = generation
        for _, battery in ipairs(self.batteries) do
            battery.last_charge = 0
            battery.last_discharge = 0
        end
        local batterySupplied = 0
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
            load.heatExported = load.admitted and load.heatKW * load.lastLevel
                * load.deliveredFraction or 0
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
        -- requested. Shortages are valid; an over-allocation is a
        -- software failure and fails contained.
        local totalSupplied = 0
        for index = 1, #self.loads do totalSupplied = totalSupplied + (supplied[index] or 0) end
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
    end
    function object:advance(world_tick, fixed_seconds)
        assert(world_tick > 0 and fixed_seconds > 0)
        checkFiniteLimits(self)
        applyPower(self, world_tick * fixed_seconds, fixed_seconds)
        applyTransfers(self, fixed_seconds)
        applyConversions(self, fixed_seconds)
        checkFiniteLimits(self)
        checkConservation(self)
        checkConstituentConservation(self)
        self.ticks = self.ticks + 1
        return self.ticks
    end
    checkFiniteLimits(object)
    checkConservation(object)
    checkConstituentConservation(object)
    return object
end
