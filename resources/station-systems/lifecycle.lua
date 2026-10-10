-- Station systems slice 2a: lifecycle plus authored finite Resource stores.
-- Every system executes this factory in its own budgeted private sandbox.
-- Stores arrive as plain immutable descriptor values; each object initialises
-- its own copy and re-checks the finite-capacity invariants every tick.
return function(name, stores)
    assert(type(name) == "string")
    assert(type(stores) == "table", "resource store descriptors missing")
    local object = { ticks = 0, stores = {} }
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
        object.stores[index] = {
            id = store.id,
            unit = store.unit,
            capacity = store.capacity,
            qualified = store.qualified,
            unqualified = store.unqualified,
        }
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
    function object:advance(world_tick, fixed_seconds)
        assert(world_tick > 0 and fixed_seconds > 0)
        checkFiniteLimits(self)
        self.ticks = self.ticks + 1
        return self.ticks
    end
    checkFiniteLimits(object)
    return object
end
