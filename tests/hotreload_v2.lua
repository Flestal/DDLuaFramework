local version = "v2"
local announced = false

dd.log("example mod loaded", version, dd.framework_version, dd.game_build)

dd.on("render_tick", function(dt)
    if not announced then
        dd.log("example render_tick", version, dt)
        announced = true
    end
end)
