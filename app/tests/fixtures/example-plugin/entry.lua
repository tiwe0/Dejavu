local active = false
return {
    start = function(ctx)
        local helper = assert(load(ctx.read("helper.lua"), "@example/helper.lua", "t"))()
        assert(helper.name == "example resource")
        assert(ctx.read("hook.c"):find("example_resource", 1, true))
        active = true
    end,
    stop = function(ctx)
        active = false
    end,
    status = function(ctx)
        return active and "running" or "stopped"
    end,
}
