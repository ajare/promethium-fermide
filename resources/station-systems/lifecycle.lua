-- Station systems slice 1a: lifecycle only, no physical process coefficients.
-- Every system executes this factory in its own budgeted private sandbox.
return function(name)
    assert(type(name) == "string")
    local object = { ticks = 0 }
    function object:advance(world_tick, fixed_seconds)
        assert(world_tick > 0 and fixed_seconds > 0)
        self.ticks = self.ticks + 1
        return self.ticks
    end
    return object
end
