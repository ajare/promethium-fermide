-- Station systems slice 2b: lifecycle, authored finite Resource stores and
-- bounded authored transfers. Every system executes this factory in its own
-- budgeted private sandbox. Stores and transfer rules arrive as plain
-- immutable descriptor values; each object initialises its own copy and
-- re-checks the finite-capacity and conservation invariants every tick.
-- The host applies the authoritative ledger; this mirror audits the same
-- rules inside the sandbox so a corrupt crossing or model drift fails
-- contained through the integrated error path.
local BALANCE_TOLERANCE = 1e-9

return function(name, stores, transfers)
    assert(type(name) == "string")
    assert(type(stores) == "table", "resource store descriptors missing")
    assert(type(transfers) == "table", "resource transfer descriptors missing")
    local object = { ticks = 0, stores = {}, transfers = {}, order = {}, starting_totals = {} }
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
        object.stores[index] = {
            id = store.id,
            unit = store.unit,
            capacity = store.capacity,
            qualified = store.qualified,
            unqualified = store.unqualified,
            protected_reserve = protected,
        }
        by_id[store.id] = object.stores[index]
        local total = object.starting_totals[store.unit] or 0
        object.starting_totals[store.unit] = total + store.qualified + store.unqualified
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
        object.transfers[index] = {
            id = transfer.id,
            source = source,
            destination = destination,
            unit = transfer.unit,
            rate = transfer.rate,
            priority = transfer.priority,
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
            local residual = (totals[unit] or 0) - starting
            local bound = BALANCE_TOLERANCE * math.max(1.0, math.abs(starting))
            assert(residual == residual and math.abs(residual) <= bound,
                "conservation residual for unit '" .. unit .. "' exceeds tolerance")
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
    function object:advance(world_tick, fixed_seconds)
        assert(world_tick > 0 and fixed_seconds > 0)
        checkFiniteLimits(self)
        applyTransfers(self, fixed_seconds)
        checkFiniteLimits(self)
        checkConservation(self)
        self.ticks = self.ticks + 1
        return self.ticks
    end
    checkFiniteLimits(object)
    checkConservation(object)
    return object
end
