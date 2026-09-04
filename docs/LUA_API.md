# DDLua 0.6.0 Lua API

Place each mod in its own directory:

```text
DarkestDungeon/mods/my_mod/
  project.xml    # the mod's normal DD1 metadata, when applicable
  lua/
    mod.json     # optional DDLua metadata; reserved for dependency handling
    init.lua     # required Lua entry point
```

## Values and functions

- `dd.framework_version`: framework version string.
- `dd.game_build`: matched build-profile identifier, or an empty string.
- `dd.is_supported_build()`: whether game hooks are permitted for this executable.
- `dd.log(...)`: append tab-separated values to `ddlua/logs/ddlua.log`.
- `print(...)`: alias routed to the same per-mod log.
- `dd.on("render_tick", callback)`: run after `SDL_GL_SwapWindow` presents a frame.
- `dd.on("corridor_return_roll", callback)`: adjust a native revisited-corridor
  content roll through bounded policy values.
- `dd.effect(name, handlers)`: observe a named game effect and run optional per-target handlers.

`render_tick` receives presentation-to-presentation `dt` in seconds, clamped to 0.25. It is not a deterministic game-simulation update and must not be used to infer turns or combat state.

```lua
dd.log("loaded", dd.framework_version, dd.game_build)

local elapsed = 0
dd.on("render_tick", function(dt)
    elapsed = elapsed + dt
    if elapsed >= 5 then
        dd.log("five rendered seconds elapsed")
        elapsed = 0
    end
end)
```

## Named effects

`dd.effect` accepts a game effect name and a table containing at least one of
`before_apply` or `after_apply`:

```lua
dd.effect("My Effect", {
    before_apply = function(context)
        -- Returning false cancels this effect for this target only.
        return not context.target:has_buff("hp_dot_burn_decay_percent")
    end,
    after_apply = function(context)
        assert(context.target:defer_dot("burn"))
    end,
})
```

Handlers run once for every effective target selected by the game. The engine
finishes the named effect before any `after_apply` handler runs. If multiple Lua
mods register the same `before_apply`, any `false` return cancels that target.
Errors are logged and default to allowing the native effect.

The context exposes ephemeral actor references and only build-profiled primitive
operations:

- `context.target`: the current effect target as an `ActorRef`.
- `context.performer`: the effect performer as an `ActorRef`, or `nil`.
- `context:actors(group)`: returns an array of `ActorRef` values for the symbolic
  `heroes` or `enemies` formation.
- `actor:has_buff(stat, amount?, tolerance?)`: tests an active buff by symbolic
  stat. When `amount` is omitted, any amount matches; otherwise the default
  tolerance is `0.0001`.
- `actor:has_buff_id(buff_id)`: tests one exact active buff ID.
- `actor:trigger_dot(dot)`: immediately invokes the supported native DoT tick.
- `actor:defer_dot(dot)`: queues the supported DoT tick for the boundary after
  `bs_after_turn` has materialized additional effects. Repeated requests for the
  same battle, actor, and DoT in one action are coalesced.
- Actor references compare by actor identity with `==` and `~=`.

Group searches, target exclusion, count limits, ordering, and multi-actor
operations are ordinary Lua loops over `context:actors(group)`. They are not
native bridge policy. An effect context and every `ActorRef` derived from it are
valid only during that callback; saving either and using it later raises an
error. Raw pointers, arbitrary addresses, and unrestricted native calls are never
exposed. The current checked profile provides `hp_dot_burn_decay_percent` and
`burn`; additional symbols require separately verified profile entries.

## Corridor-return rolls

`corridor_return_roll` runs immediately before the native return-content roll.
Its context has read-only `content`, `native_chance`, and `torchlight` fields,
plus `context:actors("heroes")`. Corridor actor references support buff queries,
but not combat-only DoT operations. Known content names include `battle`,
`hunger`, and `trap`.

Return `nil` for native behavior, or a table with either bounded field:

```lua
dd.on("corridor_return_roll", function(context)
    local marked = false
    for _, actor in ipairs(context:actors("heroes")) do
        if actor:has_buff_id("my_marker_buff") then
            marked = true
            break
        end
    end
    if context.torchlight == 0 and context.content == "trap" and marked then
        return {
            chance_multiplier_delta = 0.5,
            extra_rolls = 1,
        }
    end
end)
```

`chance_multiplier_delta` values from all mods add together before the final
multiplier is clamped to `0..8`; the resulting chance is clamped to `0..1`.
Thus two `-0.5` returns produce a zero chance. `extra_rolls` values add together
and are capped at four. Every roll calls the original native placement function,
so downstream battle mash selection—including the zero-light Shambler
replacement—remains intact. The context expires when its callback returns.

## Hot reload

Save `init.lua` while the game is rendering. Existing files are checked about four times per second; new and removed mod directories are discovered within about two seconds.

- A successful load replaces only that mod's Lua state.
- A syntax or top-level runtime error preserves the previous working state.
- A callback error is logged and that callback is disabled until the next successful reload.
- Global variables are reset on reload; persistent storage is not exposed yet.

The safe runtime includes base, coroutine, table, string, math, and UTF-8 libraries. `io`, `os`, `package`, and `debug` are unavailable. Game objects and memory are not directly exposed.
