-- Station systems slice 2c: lifecycle, authored finite Resource stores,
-- bounded authored transfers and compound-resource conversions. Every
-- system executes this factory in its own budgeted private sandbox.
-- Stores, transfer rules and conversion rules arrive as plain immutable
-- descriptor values; each object initialises its own copy and re-checks
-- the finite-capacity, per-unit conservation and per-constituent
-- compound conservation invariants every tick. The host applies the
-- authoritative ledger; this mirror audits the same rules inside the
-- sandbox so a corrupt crossing or model drift fails contained through
-- the integrated error path.
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

return function(name, stores, transfers, conversions)
    assert(type(name) == "string")
    assert(type(stores) == "table", "resource store descriptors missing")
    assert(type(transfers) == "table", "resource transfer descriptors missing")
    assert(type(conversions) == "table", "resource conversion descriptors missing")
    local object = {
        ticks = 0, stores = {}, transfers = {}, order = {},
        conversions = {}, conversion_order = {},
        starting_totals = {}, unit_loss = {},
        starting_constituents = {}, constituent_loss = {},
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
    function object:advance(world_tick, fixed_seconds)
        assert(world_tick > 0 and fixed_seconds > 0)
        checkFiniteLimits(self)
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
