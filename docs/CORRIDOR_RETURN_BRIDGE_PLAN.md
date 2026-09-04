# Corridor-return content bridge plan

## Feasibility

The requested trinket behavior is implementable with a narrow native bridge.
The base game already models return-corridor content in
`shared/rules.json` under `corridor_return_content`.  It evaluates separate
entries for `ac_battle`, `ac_hunger`, and `ac_trap`, so the framework does not
need to replace map generation or synthesize tile structures itself.

The Lua policy should treat the percentages as additive modifiers to a chance
multiplier, not as percentage-point additions:

- Creeping Hide: battle/trap multiplier delta `+0.50`.
- Creeping Tentacle: hunger multiplier delta `-0.50`.
- Chaos set: hunger multiplier delta `-0.50`; battle/trap extra rolls `+1`.
- Final chance: `native_chance * max(0, 1 + sum(multiplier_deltas))`, clamped
  to `[0, 1]` before each roll.

Consequently, Tentacle plus the set bonus gives hunger a multiplier of zero.
The set bonus calls the same native placement routine once more for battle and
trap, preserving the engine RNG, empty-tile selection, save representation,
and battle/encounter filling behavior.

## Statically identified path (Steam build 25110930)

Executable SHA-256:
`6ACA206C13C678001C3ACCA59E7A2F939CB25310D65A594BC32FFD2C282BD7B3`

The names below are descriptive reverse-engineering labels, not engine symbols.

1. `raid_move_to_area`, RVA `0x00762F20`
   - Logs `move to area`.
   - For a corridor, iterates the loaded `corridor_return_content` table.
   - Maps the table keys (`ac_battle`, `ac_hunger`, `ac_trap`, and others) to
     the engine content enum.
   - Evaluates `base_chance + base_dark_mod_table(torchlight)` and calls the
     per-content placement routine.
2. `corridor_return_try_place`, RVA `0x005D7CE0`
   - Candidate primary hook.
   - Observed ABI: `RCX=return-content owner`, `RDX=Area*`, `XMM2=chance`,
     `R9D=area-content enum`; returns `AL=true` when content was placed.
   - Rejects corridors without an eligible empty interior tile.
   - Draws from the game's Rules RNG, chooses one eligible interior tile, calls
     `Area::SetContent`, fills content-specific data, and returns success.
3. `area_set_content`, RVA `0x00595900`
   - Observed ABI: `RCX=Area*`, `EDX=tile index`, `R8D=area-content enum`.
   - Writes the tile content and clears stale content-specific fields.
4. `fill_area_tile_content`, RVA `0x005DA7D0`
   - Observed ABI: `RCX=return-content owner`, `RDX=Area*`, `R8D=tile index`.
   - Fills battle/ambush, trap, obstacle, and curio-specific fields after the
     enum has been assigned.
5. `range_table_value_at`, RVA `0x0076DDE0`
   - Evaluates the darkness modifier table for the current torchlight value.
   - The caller supplies the raid torchlight float from `raid+0x5F14`.
     Live trace PID 20200 confirmed values 50, 44, and exact 0 matching play.

Content enum mapping recovered from the executable's `ac_*` lookup table:

| Value | Symbol |
| ---: | --- |
| 0 | `ac_nothing` |
| 1 | `ac_battle` |
| 2 | `ac_ambush` |
| 3 | `ac_trap` |
| 4 | `ac_obstacle` |
| 5 | `ac_happening` |
| 6 | `ac_guarded_curio` |
| 7 | `ac_curio` |
| 8 | `ac_hunger` |
| 9 | `ac_treasure` |
| 10 | `ac_guarded_treasure` |
| 11 | `ac_ambush_curio` |
| 12 | `ac_ambush_treasure` |
| 13 | `ac_hidden_door` |
| 14 | `ac_prisoner` |

## Implemented bridge contract

The bridge exposes this policy-free event:

```lua
dd.on("corridor_return_roll", function(context)
    -- context.content is "battle", "hunger", "trap", or another symbolic ID.
    -- context.native_chance is read-only.
    -- The callback returns only bounded policy values.
    return {
        chance_multiplier_delta = 0.0,
        extra_rolls = 0,
    }
end)
```

The C++ bridge should:

- hook only `corridor_return_try_place`, so ordinary initial map generation is
  not affected;
- translate the native enum to a symbolic content name before Lua sees it;
- expose no `Area*`, raid pointer, raw address, or unrestricted native call;
- aggregate all mods' multiplier deltas additively, clamp the resulting
  multiplier to at least zero, and cap extra rolls (recommended maximum: 4);
- call the original trampoline once per roll and stop neither after success nor
  after failure unless no eligible tile remains; each requested roll is an
  independent native roll;
- apply changes only when the verified torchlight value is exactly zero;
- fail closed and call the native function exactly once if any profile field,
  enum mapping, Lua callback, or state validation fails.

For the trinkets, prefer three unique equipped-buff marker IDs (Hide, Tentacle,
and Chaos set).  The Lua policy can query those IDs across the hero formation.
First verify that a harmless zero-amount marker remains in the Actor buff vector.
If the engine drops zero-amount buffs, reverse-engineer the equipped-trinket ID
layout instead of adding a gameplay-visible marker stat.

## Targeted live trace before implementation

The static path is strong enough to plan against but not yet sufficient to ship
a hook. Add an opt-in fixed-size recorder for the following points only:

1. Entry/exit of `raid_move_to_area`:
   - capture target area ID/kind, current and destination tile, movement
     direction, and the fields that distinguish first entry from return entry;
   - verify that `corridor_return_try_place` is reached only for the engine's
     return-corridor path.
2. Entry/exit of `corridor_return_try_place`:
   - capture `RDX`, `XMM2`, `R9D`, return `AL`, selected tile index, and the
     before/after tile content values;
   - run one return at torch 100, 50, 25, 1, and 0 to validate the chance values
     against `shared/rules.json`.
3. Reads around the apparent torchlight field `raid+0x5F14`:
   - confirm exact values while the UI shows 100, 75, 50, 25, 1, and 0;
   - determine whether zero is represented exactly or needs an integer/epsilon
     comparison.
4. Equipped marker validation:
   - equip each trinket separately and then the complete set;
   - confirm unique marker IDs in the hero Actor buff vectors outside combat;
   - confirm the set marker appears only when both pieces are on the same hero.

Use a short disposable expedition/save backup. The recorder must be opt-in,
must not ship in the normal runtime, and must be removed after the profile and
ABI are verified.

The temporary implementation is enabled by creating
`_windows/win64/ddlua/config/corridor_trace.enabled`. It writes
`_windows/win64/ddlua/logs/corridor_trace_<pid>.bin`. Decode it after the game
has exited with:

```powershell
python tools/decode_corridor_trace.py <corridor_trace_PID.bin>
```

## Live trace result: PID 20200

The 2026-09-05 run produced 21 movement calls and 33 return-content rolls.
Eleven corridor entries each invoked exactly three rolls in the stable order
`battle`, `hunger`, `trap`; room entries invoked none. This confirms that
`raid_move_to_area` plus `corridor_return_try_place` is the narrow return-
corridor path needed by the bridge.

- Torchlight at `raid+0x5F14` matched UI/gameplay values: 50, 44, and exact
  IEEE-754 zero. The alternate `raid+0x298` candidate was an integer-like flag
  and is rejected.
- Native chances were 0.10 battle, 0.10 hunger, 0.08 trap at torch 50/44 and
  0.125 battle, 0.125 hunger, 0.105 trap at torch zero.
- Successful placements returned true and changed exactly one empty tile:
  trap at sequences 5 and 15, hunger at sequences 7 and 13, and battle at
  sequence 21. Forward and reversed corridors both succeeded.
- `area_kind == 1` identifies every corridor observed. `area_reversed` tracked
  traversal orientation and does not alter the call contract.
- All observed tile knowledge values were 3. Return-content may therefore be
  inserted into already visited/known corridor tiles by this native function.
- Actor buff vectors contained stable trinket buff IDs such as
  `TRINKET_ancestors_map_SCOUTING_BUFF`; unique non-zero marker buffs are a
  viable equipment-presence channel. Zero-amount marker retention is still
  untested and should not be assumed.

The final zero-torch return placed `ac_battle` at tile 2. Four seconds later,
the same PID's `app.log` recorded `FillOutBattle with mash entry index:2`, the
`shambler_spawn_01` narration, and actor `shambler_A`. Therefore the corridor
return layer places an ordinary battle enum first, and the conditional Shambler
replacement occurs later while the battle mash is selected. The trinket bridge
should preserve that downstream native path: extra battle rolls must call the
original placement function and must not synthesize a monster mash directly.

## Acceptance test matrix

At torchlight above zero, all four equipment states must match native rolls.
At exactly zero:

| Equipment | Battle/trap chance | Hunger chance | Battle/trap rolls |
| --- | ---: | ---: | ---: |
| none | `native` | `native` | 1 |
| Hide | `native * 1.5` | `native` | 1 |
| Tentacle | `native` | `native * 0.5` | 1 |
| both / Chaos | `native * 1.5` | `0` | 2 |

Also verify no duplicate placement on occupied/door tiles, save/reload after
new content is placed, reverse traversal in both corridor orientations, and
interaction with other mods that alter `shared/rules.json` probabilities.
