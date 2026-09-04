dd.log("Lua framework loaded")
print("init.lua completed safely")
assert(io == nil and os == nil and package == nil and debug == nil,
       "unsafe standard libraries must remain unavailable")

local first_render = true
dd.on("render_tick", function(dt)
    if first_render then
        dd.log("render_tick callback active", dt)
        first_render = false
    end
end)
