# Plan: Author the `interact` API examples in `shared/examples.lua`

## Context

`shared/examples.lua` is the spec-by-example for the `inter` resource's public API.
Right now the real `add*` functions aren't implemented yet ([client/exports.lua](client/exports.lua)
and [client/main.lua](client/main.lua) are empty), and the only existing example is a single
`interact.addCoords` call with an empty `interactable = {}`. The examples file therefore
**defines the intended API surface** that the implementation will later be built against.

Goal: write enough, realistic examples for each of the eight callers —
`addCoords`, `addLocalEntity`, `addEntity`, `addModel`, `addGlobalObject`,
`addGlobalPlayer`, `addGlobalPed`, `addGlobalVehicle` — that demonstrate the two
distance behaviours (fixed show/hide vs. preview-then-interact) and the option/state schema.

These signatures mirror ox_target's `add*` family, so per-function arguments are well-grounded.

## Decisions (from the user)

- **Distance = "short caller":** a bare number is fixed; a small table turns on a preview band.
- **`interactable` = array of menu options**, each with its own `onSelect`.
- **Three-arg shape** (per the user's `addCoords` description): `target`, `options`, `states`,
  where `states` is a table of lifecycle callbacks (`onFocus`, `tooClose`, universal `onSelect`, …).
- Features to showcase: per-option `onSelect`, `canInteract` gating, and item requirements.

## API shape these examples lock in

```lua
interact.addCoords(coords, options, states)   -- target = vec3
interact.addLocalEntity(entity, options, states)  -- target = local handle
interact.addEntity(netId, options, states)        -- target = network id
interact.addModel(model|{models}, options, states)
interact.addGlobalObject(options, states)         -- no target arg
interact.addGlobalPlayer(options, states)
interact.addGlobalPed(options, states)
interact.addGlobalVehicle(options, states)
```

**`options` table**
```lua
{
  icon  = 'fa-solid fa-...',   -- preview prompt icon (shown in world)
  label = '...',               -- preview prompt label
  distance = 2.0,              -- FIXED: shows + interactable within 2.0m
  -- distance = { preview = 8.0, interact = 2.0 }, -- PREVIEW band, then interact up close
  interactable = {             -- menu options revealed when in interact range
    {
      icon  = 'fa-...',
      label = 'Lockpick',
      items = { 'lockpick' },                 -- item requirement (string or list)
      canInteract = function(data) return ... end, -- gating (job/gang/state/distance)
      onSelect    = function(data) ... end,   -- per-option action
    },
    -- ...more options
  },
}
```

**`states` table (3rd arg) — lifecycle callbacks**
```lua
{
  onPreview = function(data) end, -- entered the preview band (preview distance only)
  onFocus   = function(data) end, -- within interact range / focused
  tooClose  = function(data) end, -- player is too close to use it
  onSelect  = function(data) end, -- UNIVERSAL: any interactable option chosen
  onExit    = function(data) end, -- left range entirely
}
```

**`data` convention** (passed to `onSelect`/`canInteract`/state callbacks), ox_target-style:
`data.entity`, `data.netId`, `data.coords`, `data.distance`, `data.name`. Globals pass the
resolved entity; `addCoords` passes `coords`.

## What goes in `shared/examples.lua`

Keep everything inside the existing `if Config.Debugging then ... end` guard (the file is
test-only per the [fxmanifest.lua](fxmanifest.lua) comment). Preserve the current Debugging
`addCoords` as example #1, then add a clearly-commented block per function. Spread the
feature coverage so the set as a whole shows fixed + preview distance, per-option `onSelect`,
`canInteract`, items, and the full `states` table — while each example stays readable.

| Caller | Scenario | Distance mode | Highlights |
|---|---|---|---|
| `addCoords` (debug) | existing bug marker | fixed | minimal, kept as-is |
| `addCoords` | distant control panel | **preview** `{preview=10, interact=2}` | full `states` (onPreview/onFocus/tooClose/onSelect/onExit) |
| `addLocalEntity` | client-spawned prop | fixed | 2 options, `canInteract` |
| `addEntity` | a specific net entity | preview | per-option `onSelect` w/ `data.entity` |
| `addModel` | ATM models (list) | fixed | `items` requirement, multi-model target |
| `addGlobalObject` | inspect/pick up world objects | fixed | `canInteract` filters by model |
| `addGlobalPlayer` | give item to a player | fixed | `data.entity`, distance gating |
| `addGlobalPed` | rob a ped | preview | `items` (weapon) + `canInteract` |
| `addGlobalVehicle` | lockpick / inspect vehicle | preview | `items={'lockpick'}`, 2+ options, `states` |

Each `onSelect` will use plausible bodies (notifications, `TriggerServerEvent`, prop logic) so
the examples read like real usage rather than empty stubs.

## File touched

- `shared/examples.lua` — sole edit (replace the contents with the full example set, keeping
  the `Config.Debugging` guard and the original debug marker).

## Verification

- **Lua loads:** `luacheck shared/examples.lua` if available, otherwise `luac -p` / a syntax
  pass — confirm no parse errors and balanced tables.
- **In-game (manual):** start the resource with `Config.Debugging = true`; each example should
  register without runtime errors once the `add*` exports exist. Until then, the examples serve
  as the reference the implementation targets — confirm the call shapes match the intended
  `(target, options, states)` signature.
- **Self-consistency:** every example uses the same `data`/`states`/`distance` conventions so
  the future implementation has one schema to satisfy.
