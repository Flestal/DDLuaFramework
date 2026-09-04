# DDLua Framework

Experimental 64-bit Windows Lua 5.4 framework for Darkest Dungeon 1. Version 0.6.0 provides isolated per-mod Lua states, transactional hot reload, a render-boundary callback, build-verified generic effect/action-end/corridor-return bridges, and an opt-in corridor-return diagnostic tracer without exposing raw game memory to scripts.

## Architecture and safety boundary

`winmm.dll` forwards the game's direct WinMM imports plus the timer, mixer, and wave-input/output functions required transitively by its bundled SDL2, Steam client, and the locally detected graphics driver to the absolute System32 copy. `DllMain` only stores its module handle and disables thread notifications. The first forwarded WinMM call starts initialization on a worker thread, after loader-lock initialization has finished.

The core hashes the host executable, looks it up in `ddlua/config/supported_builds.ini`, and fails closed for unknown executables. On the supported build it replaces the main executable's imported `SDL_GL_SwapWindow` pointer and emits `render_tick` after presentation. Version 0.6.0 verifies every native address and layout in `ddlua/config/game_bridge_profiles.ini` before installing policy-free effect and common battle-state-transition bridges. C++ reads the resolved effect name, performer, and target from the engine's per-target apply call and exposes only symbolic, build-profiled actor primitives; effect names, values, ordering, cancellation rules, group filtering, exclusions, limits, and multi-actor operations live in each mod's Lua. Dot primitives verify that the requested status is present before reporting success. The temporary native all-call recorder used during bridge discovery is not part of the runtime. Lua gets only safe standard libraries; `io`, `os`, `package`, and `debug` are not opened. Every script and callback uses protected calls.

Each normal game mod containing `lua/init.lua` receives a separate Lua state. The discovery path is `DarkestDungeon/mods/<mod folder>/lua/init.lua`; DDLua no longer uses `ddlua/mods`. Existing scripts are checked every 250 ms and new/removed mod directories every 2 seconds. A successful edit atomically replaces that mod state. A syntax or runtime error rejects the new state and preserves the previous working version.

## Build and test

From a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix build/package
```

Lua 5.4.9 is vendored under `third_party/lua-5.4.9` from the official Lua release archive (SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`) under the Lua license.

If CMake selects a partially installed MSVC toolset, add the locally installed complete toolset to the configure command (for example `-T v143,version=14.44.35207`). This is a machine-specific workaround, not a project requirement.

## Installation (not performed automatically)

Build the `install` target into a staging directory, or copy these items from `build/stage/good` into `DarkestDungeon/_windows/win64`:

- `winmm.dll`
- `ddlua/init.lua`
- `ddlua/config/supported_builds.ini`
- `ddlua/config/game_bridge_profiles.ini`

The framework creates `ddlua/logs/ddlua.log`. It does not replace any original game file. Before installing, make sure no unrelated `winmm.dll` is already present; if one exists, stop and resolve the conflict instead of overwriting it.

## Removal and recovery

Exit the game, then remove only the added `winmm.dll` and `ddlua` directory. Since no original file is replaced, this restores the pre-install state. Steam file verification is not required.

## Current API

```lua
dd.log("Lua framework loaded")
print("also routed to ddlua.log")
dd.framework_version
dd.game_build
dd.is_supported_build()
dd.on("render_tick", function(dt)
    -- dt is presentation-to-presentation time, clamped to 0.25 seconds.
end)

dd.effect("My Named Effect", {
    before_apply = function(context)
        return not context.target:has_buff("my_symbolic_stat", 0.8)
    end,
    after_apply = function(context)
        assert(context.target:trigger_dot("burn"))
    end,
})
```

## Deliberately deferred

- additional game event bridges beyond named effects
- general actor/state mutation primitives
- Butcher's Circus/online support

Those features require separate, build-profiled signatures and lifecycle validation. A mismatched executable must never fall back to guessed addresses.
