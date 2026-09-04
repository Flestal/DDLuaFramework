# Local validation record

Date: 2026-09-04 (Asia/Seoul)

## Game executable (read-only inspection)

- Path: `E:/SteamLibrary/steamapps/common/DarkestDungeon/_windows/win64/Darkest.exe`
- Size: 19,351,552 bytes
- SHA-256: `6ACA206C13C678001C3ACCA59E7A2F939CB25310D65A594BC32FFD2C282BD7B3`
- PE machine: x64
- WinMM imports confirmed: `timeBeginPeriod`, `timeEndPeriod`, `timeGetTime`
- Bundled SDL2 transitively imports 18 additional `waveIn*`/`waveOut*` functions; all are forwarded by the proxy.
- The installed Steam client and NVIDIA display driver additionally require mixer/message APIs and `timeGetDevCaps`; the locally observed import union is forwarded too.
- Local Steam client and NVIDIA driver imports were also inspected; their required mixer, wave-message, and timer-capability exports are forwarded.

## Prototype

- Generator: Visual Studio 17 2022
- Compiler: MSVC 19.44.35228.0, x64
- Configuration: Release
- CTest: 5/5 passed
  - proxy forwarding and successful Lua execution
  - protected Lua runtime-error handling
  - protected Lua syntax-error handling
  - protected render callback error handling and automatic callback disable
  - application-directory `winmm.dll` selection through a normal import (no explicit DLL path)
- Proxy exports: `timeBeginPeriod`, `timeEndPeriod`, `timeGetTime`, diagnostic `DDLua_GetInitializationState`
- PE flags: x64, Dynamic Base, NX compatible
- Packaged DDLua 0.2.1 `winmm.dll`: 520,192 bytes; SHA-256 `98AB4BADF4EEDC2C667229400DDE5FE6739F376D26D3362DD67D8436A7194E08`
- Runtime dependencies: `KERNEL32.dll`, `bcrypt.dll` (no external Lua or Visual C++ runtime DLL)

The test-host executable intentionally does not match a game build profile. Its log confirmed that the game bridge remains disabled while the build-independent Lua bootstrap still runs.

## Live game test

The user installed the package, after which the proxy was updated in place to include SDL2, Steam client, and NVIDIA driver's observed transitive WinMM imports. The actual bundled `SDL2.dll` loaded successfully against the proxy. `Darkest.exe` then remained alive and responsive, loaded both the local proxy and absolute System32 implementation, matched profile `steam-25110930-win64`, and logged successful execution of `init.lua`. Earlier missing-entry-point errors for `waveInClose`, `mixerSetControlDetails`, and `timeGetDevCaps` no longer occurred in the final run.

Observed log sequence:

- `DDLua Framework 0.1.0 bootstrap started`
- matching process SHA-256
- `Supported Darkest Dungeon build profile: steam-25110930-win64`
- `Lua framework loaded`
- `Lua initialization completed`

## Live hot-reload test

With the game left running, the example mod was changed from v1 to v2, replaced with a syntax-error candidate, and restored to v1. The log confirmed:

- v2 loaded and `Hot reload committed` without restarting the game.
- The syntax-error candidate logged `Hot reload rejected; previous working state retained`.
- Restoring v1 committed another reload and delivered a fresh `render_tick` callback.
- `Darkest.exe` remained alive and responsive throughout the sequence.

## Base mods directory migration

Version 0.2.1 changed discovery from `ddlua/mods/<id>/init.lua` to the normal game-mod layout `DarkestDungeon/mods/<mod folder>/lua/init.lua`. Automated discovery tests passed, and a live game run logged the exact discovery root `E:/SteamLibrary/steamapps/common/DarkestDungeon/mods`. The example mod loaded from that tree, hot-reloaded v1 to v2, and hot-reloaded back to v1 while the game remained responsive. The old framework-local example directory was removed.

## Version 0.3.0 offline validation

- Release build completed with MSVC 19.38 for x64.
- All five proxy, Lua isolation, syntax-error, callback-error, and application
  search-order tests passed.
- `tools/verify_bridge_profile.py` matched the SHA-256 build and all recorded
  internal bridge bytes against the installed `Darkest.exe`.
- Installed DLL SHA-256:
  `161C0F9CED9770763DBAFFF6C6354C653F9053AC2E06D1CECCF32FE8C1700B43`.
- The previous 0.2.1 DLL was retained as `winmm.dll.ddlua-0.2.1.bak`.
- Actual marker target capture and forced Burn damage still require an in-game
  test; offline tests deliberately do not execute Darkest Dungeon internals.
- The maximum-one-stack guard uses the verified Actor buff-vector layout
  (`+0x460/+0x468`, entry size `0x1D8`) and BuffStat type `0x5C`; this path also
  remains pending an in-game duplicate-application test.

## Version 0.4.0 policy separation

- Removed all Flestal effect names, amounts, and duplicate-stack rules from the
  native bridge.
- Added the generic `dd.effect(name, handlers)` API with protected, short-lived
  per-target contexts.
- Kept build RVAs, ABI contracts, object layout, enum mappings, and native DoT
  entry points in the fail-closed native profile.
- Moved the Rekindled Fire Sprayer names, `0.80` amount, maximum-one-stack rule,
  Burn primitive selection, and execution order into `FlestalTrinkets/lua/init.lua`.

## Version 0.4.7 runtime cleanup

- Removed the temporary all-call binary recorder from the native bridge,
  including caller/stack capture, memory snapshots, and per-record file flushes.
- Removed Flestal's per-trigger success messages. Framework bootstrap, load,
  hot-reload, and error messages remain in `ddlua/logs/ddlua.log`.
- The final 0.4.6 gameplay capture had already verified same-action-boundary
  group Burn execution: all enemies after wearer hit, and up to three other
  enemies after wearer was hit or dodged. Version 0.4.7 changes recording only,
  not those bridge semantics.
- The Release build passed all six offline regression tests before deployment.

## Version 0.4.8 stat-presence guard

- `ActorRef:has_buff` accepts an omitted amount and then matches any active
  entry of the requested symbolic stat. Exact amount/tolerance matching remains
  available for scripts that need it.
- Flestal's Burn-decay guard now uses stat presence instead of a brittle exact
  `0.80` comparison, and both debuff effects use a `1000%` application chance.
- The Release build passed all six offline regression tests before deployment.
