# Animation System v2 — "AnimGraph" Design

**Status:** design locked 2026-07-11 · implementation starting (milestone A0)
**Replaces:** `src/anim/PlayerAnimator` (retired at A7, kept until parity)
**Owner note:** this doc is the contract. Code seams reference it via `FUTURE(tag)` comments — `grep -r "FUTURE(" src tools` lists every extension point.

---

## 0. TL;DR — the pipeline

```
gameplay code                     animation runtime                        outputs
─────────────                     ─────────────────                        ───────
world probing      ──writes──▶  Blackboard (typed params)
input, physics                        │
                                      ▼
                                Graph instance (per entity)
                                  Layers (masked, override/additive)
                                    └─ State machines
                                        └─ Blend nodes (clip/1D/2D…)
                                            └─ Clips (keyframes, quat)
                                      │
                                      ▼
                                Pose ops (blend, compose)   ──▶  bone matrices → renderer
                                      │                     ──▶  socket world transforms
                                      ▼                     ──▶  events fired this tick
                                Debug snapshot (ring buffer)──▶  curve values
                                                            ──▶  tools exe timeline
```

One runtime, **entity-agnostic**: player, horse, boat are all "a rig + a graph + a blackboard". Nothing player-specific in the core.

## 1. Principles (referenced as P1…P6)

- **P1 — Gameplay decides WHAT, animation decides HOW IT LOOKS.** World probing (gap distance, ledge height, combat case, mount state) is gameplay code; it writes parameters. The runtime never queries the world.
- **P2 — Physics is authoritative for movement.** No root motion drives the capsule. The `root` bone stays in poses so a warping layer *can* exist later (`FUTURE(root-motion)`).
- **P3 — Events/curves are authoritative gameplay signals** where animation timing *is* the mechanic (oar thrust windows, hit windows, footsteps). Deterministic, no drift.
- **P4 — Data-driven everything.** Rigs from geometry JSON, clips from clip JSON, graphs from graph JSON. Adding a horse costs data, not engine code.
- **P5 — Blend in quaternions.** Euler angles come in from authoring, are converted to quats at load, blended as quats (shortest-arc nlerp), composed to matrices. Never blend Euler.
- **P6 — Loud stubs, no dead code.** Schema fields for future features are parsed + validated day one; unimplemented paths fail with `"not implemented yet: see FUTURE(tag) in docs/animation.md"`. We do not pre-write rotting implementations.

## 2. Modules

| File (src/anim/) | Responsibility |
|---|---|
| `Rig.h/.cpp` | Bone hierarchy loaded from Bedrock geometry JSON: names, parent indices, pivots, locators. Bone-name→index lookup. |
| `Pose.h/.cpp` | `Pose` = per-bone local TRS (quat rot, vec3 pos, vec3 scale). Ops: `blend`, `compose→matrices`, socket world transforms. IK pass slot: `FUTURE(ik)`. |
| `AnimClip.h/.cpp` | Clip asset: channels (quat-converted at load), duration, loop, events, curves. Native JSON + Bedrock keyframe import (Blockbench). |
| `Blackboard.h` | Typed parameter store (bool/int/float/trigger). Written by gameplay, read by expressions. |
| `BlendNode.h/.cpp` | Polymorphic node tree: `Clip`, `Blend1D`, `Blend2D` (stub, `FUTURE(parkour)`), `Additive` (stub, `FUTURE(combat)`). |
| `AnimStateMachine.h/.cpp` | States, Molang-guarded transitions, cross-fades, one-shots, interrupt rules, priorities. |
| `AnimGraph.h/.cpp` | Graph asset (layers + machines + parameter declarations) and its JSON loader. |
| `Animator.h/.cpp` | Per-entity instance: update(dt) → pose, matrices, sockets, events, curves, debug snapshot. The only header gameplay includes. |
| `Molang.h/.cpp` | (exists) Reused as THE expression language for conditions/expressions, bound to blackboard params as `v.<name>`. |
| `Json.h/.cpp` | (exists) JSON parsing. |
| `PlayerAnimator.h/.cpp` | (exists) Legacy vanilla-stack evaluator. Retired at A7. |

Asset layout: `assets/anim/clips/*.clip.json`, `assets/anim/graphs/*.graph.json`. Bedrock exports stay in `assets/animations/` and are referenced by clips.

## 3. Rig

Parsed from `assets/models/entity/*.json` (`minecraft:geometry`): every bone becomes `{name, parentIndex, pivot, locators{}}`. Mesh building (today `PlayerModel.cpp`, 6-part `PP_` enum) generalizes to **one mesh per bone**, vector aligned to rig bone indices; the existing bone-name→skin-layer mapping survives unchanged. This kills `PP_Count` and is what makes the new `leftForearm`/`rightForearm`/`leftShin`/`rightShin` bones (and any future entity) animatable.

**Sockets** = bones or locators whose world transform is queryable after evaluation: `rightItem` (held item — replaces the hardcoded hand anchor in main.cpp), `lead_hold`, future `saddle` (`FUTURE(mounts)`), and **foot locators** to add on each shin (`FUTURE(decals)`, `FUTURE(snow)`).

## 4. Pose math

- Local space per bone, Bedrock conventions (pixels, pivots, model faces -Z after the existing X-mirror in model building).
- `blend(a, b, t)`: pos/scale lerp; rot **nlerp with shortest-arc** (negate quat when dot<0).
- Multi-input blends fold pairwise with normalized weights.
- `compose(rig, pose) → mat4[]` walks parent chain: `world[i] = world[parent] * T(pivot) * R * S * T(-pivot) * T(pos)`.
- Additive apply (`FUTURE(combat)`): `rot = base.rot * delta.rot`, `pos = base.pos + delta.pos`, delta extracted vs the clip's frame 0.

## 5. Clips

Native format; channels either inline or imported from a Bedrock animation (Blockbench authoring path):

```jsonc
// assets/anim/clips/walk.clip.json
{
  "duration": 0.8,            // seconds; omit to take imported length
  "loop": true,
  "import_bedrock": {          // OPTIONAL: pull keyframe channels from a Blockbench export
    "file": "animations/player.animation.json",
    "animation": "animation.player.new_walk"
  },
  // channels may ALSO be authored inline (same shape as Bedrock: per-bone rot/pos/scale keyframes)
  "events": [
    { "time": 0.10, "name": "foot_down", "bone": "leftShin" },   // payload = source bone + optional string
    { "time": 0.50, "name": "foot_down", "bone": "rightShin" },
    { "time": 0.35, "name": "foot_up",   "bone": "leftShin" }
  ],
  "curves": {
    // named scalar tracks, keyframed & lerped; blended by clip weight at runtime
    "bob_amount": [ { "time": 0.0, "value": 0.0 }, { "time": 0.4, "value": 1.0 }, { "time": 0.8, "value": 0.0 } ]
  }
}
```

- Interpolation: linear + step day one; catmull-rom parsed → loud stub (`FUTURE(smooth-keys)`).
- Molang-expression channels (the old style) remain legal in imported clips — a 1-key channel evaluates its expression each tick (keeps vanilla imports working during migration).
- Boat oar clips will carry `oar_in_water` curves + `oar_enter_water`/`oar_exit_water` events — thrust ∝ curve × stroke rate, drag always on (`FUTURE(boat)`, P3).

## 6. Blackboard

Declared in the graph, written by gameplay each tick, read by Molang as `v.<name>`:

- `float`, `int`, `bool` — persistent values (`speed`, `gap_distance`, `weapon_type`…).
- `trigger` — bool that auto-clears at end of tick (`jump_pressed`, `attack_pressed`). Fire-and-forget from input code.
- Mount bridging is a *convention*, not a feature: gameplay copies `mount_speed`, `mount_is_jumping`… into the rider's blackboard (`FUTURE(mounts)`).

## 7. Graph: layers → state machines → nodes

```jsonc
// assets/anim/graphs/player.graph.json
{
  "parameters": {
    "speed":        { "type": "float",   "default": 0.0 },  // horizontal m/s
    "is_on_ground": { "type": "bool",    "default": true },
    "jump_pressed": { "type": "trigger" }
  },
  "layers": [
    {
      "name": "locomotion",
      "mask": { "bones": ["root"], "recursive": true },     // whole body
      "blend": "override",
      "state_machine": {
        "initial": "ground",
        "states": {
          "ground": {
            "node": { "type": "blend1d", "parameter": "v.speed", "points": [
              { "value": 0.0, "node": { "type": "clip", "clip": "idle" } },
              { "value": 2.0, "node": { "type": "clip", "clip": "walk", "sync_group": "stride" } },
              { "value": 5.6, "node": { "type": "clip", "clip": "run",  "sync_group": "stride" } }
            ]},
            "transitions": [
              { "to": "jump_stand", "condition": "v.jump_pressed && v.speed < 2.0",  "duration": 0.08 },
              { "to": "jump_run",   "condition": "v.jump_pressed && v.speed >= 2.0", "duration": 0.08 },
              { "to": "fall",       "condition": "!v.is_on_ground",                  "duration": 0.15 }
            ]
          },
          "jump_stand": { "node": { "type": "clip", "clip": "jump_stand" }, "one_shot": true,
            "interruptible_after": 0.15,
            "on_finished": "fall",
            "transitions": [ { "to": "land", "condition": "v.is_on_ground", "duration": 0.05, "priority": 1 } ] },
          "jump_run":   { "node": { "type": "clip", "clip": "jump_run" }, "one_shot": true,
            "interruptible_after": 0.10,
            "on_finished": "fall",
            "transitions": [ { "to": "land", "condition": "v.is_on_ground", "duration": 0.05, "priority": 1 } ] },
          "fall": { "node": { "type": "clip", "clip": "fall" },
            "transitions": [ { "to": "land", "condition": "v.is_on_ground", "duration": 0.05 } ] },
          "land": { "node": { "type": "clip", "clip": "land" }, "one_shot": true, "on_finished": "ground" }
        }
      }
    }
    // FUTURE(combat): "upper_body" layer, mask body+arms+head, override, weapon-selected attack machines.
    // FUTURE(parkour): vault/climb states using blend2d(v.ledge_height, v.gap_distance) + playback_rate from arc time.
  ]
}
```

Semantics:

- **Transitions**: evaluated in `priority` order (then file order); first true condition wins; `duration` = cross-fade seconds (smoothstep weight). Conditions are Molang over the blackboard.
- **One-shots**: `one_shot: true` plays once; `on_finished` fires when playback ends (after cross-fade-out starts). Not interruptible before `interruptible_after` seconds except by transitions with `"force": true` (parsed day one — hurt/death will need it, `FUTURE(combat)`).
- **Interruption mid-cross-fade**: source side is frozen as a snapshot pose; new transition blends from the frozen mix. Cheap, standard, stable.
- **`playback_rate`**: optional Molang expr per state (default 1.0). This is the parkour arc-matching hook — physics computes jump time, sets a param, clip stretches to land on touchdown (`FUTURE(parkour)`, P2).
- **Layers**: evaluated in order; `override` replaces masked bones by layer weight, `additive` is a loud stub (`FUTURE(combat)`). Mask = named bones, `recursive` includes descendants, optional `"exclude"`.

## 8. Sync groups & event firing

- States/clips tagged `sync_group: "name"` share **normalized phase**: the highest-weight member is leader, followers sample at the leader's phase. Walk↔run blending never foot-slides; player row clip ↔ boat oar clip never drift (`FUTURE(boat)` — sync groups already span entities via a shared group registry when mounted).
- Foot-plant *markers* for phase alignment beyond normalized time: `FUTURE(sync-markers)`.
- **Events fire from the dominant (highest-weight) clip in a group only** — exactly one `foot_down` per step during blends. Firing = local-time crossing detection, loop-wrap aware.
- Event delivery: `Animator::update` fills a per-tick queue of `{name, bone, boneWorldTransform, payload}`; gameplay drains it and fans out to consumers (sound by block material; `FUTURE(decals)` footprints; `FUTURE(snow)` layer deformation; `FUTURE(boat)` thrust windows). The runtime never knows what a footprint is.

## 9. Runtime API (surface sketch)

```cpp
mc::Animator anim;
anim.init(&rig, &graph);                    // per entity instance
anim.blackboard().set("speed", horizSpeed); // gameplay writes, every tick
anim.blackboard().fire("jump_pressed");     // triggers
anim.update(dt);                            // evaluate graph → pose
anim.boneMatrices();                        // span<mat4>, rig-aligned → renderer
anim.socketWorld("rightItem");              // attachments (held item, saddle…)
anim.curve("oar_in_water");                 // blended scalar tracks
anim.drainEvents(events);                   // this tick's events
anim.debugSnapshot();                       // see §10
```

## 10. Debug & tooling (the separate exe — user requirement)

**The game exe never grows an editor.** Developer tools are standalone executables in `tools/`, linking the same `mc_engine` static library (build split done at A0).

- **Debug introspection is a day-one core feature**: every `update()` records a `DebugSnapshot` — per layer: active state, transition in flight (from→to, t), per active clip: name/phase/weight; blackboard values; events fired. Kept in a ring buffer (~10 s). Costs nothing meaningful; enables everything below. `FUTURE(anim-debugger)`.
- **T1 `anim_studio` (tools/)**: loads a rig + clips/graph, orbit-camera preview, scrub bar; and the **CS2/GTA-style timeline debugger** — horizontal tracks per layer showing states as colored bars over time, transition wedges with their durations, event ticks, parameter lanes; live-attached to a running graph or replaying a snapshot ring. This is the "lines of the player animation showing what happens when and how long" the user saw in the CS2 animgraph debugger / GTA VI leak tooling.
- **Trajectory visualization** (GTA-style predicted-path lines: jump arcs, parkour landing markers) is gameplay prediction rendering — needs a world-space debug line renderer in-game. `FUTURE(debug-draw)`; pairs with `FUTURE(parkour)`.
- Until T1 exists, the F3 overlay gets one cheap line per layer: current state name + phase (wired at A4 — invaluable while building the state machine).

**Authoring workflow:** keyframe clips in **Blockbench** (export Bedrock JSON → `import_bedrock`), events/curves/sync added in the thin `.clip.json` wrapper by hand (they're sparse); graphs hand-written JSON against this doc's schema. A node-graph editor is `FUTURE(tools)` — after the runtime proves the format.

## 11. Integration & migration

1. `PlayerModel` → generic per-bone meshes from the rig (kills `PP_` enum; elbows/knees become live). `ModelRenderer` consumes N matrices.
2. Held item: hardcoded hand anchor in main.cpp → `socketWorld("rightItem")`.
3. `PlayerAnimator` keeps driving the player until the new runtime reaches parity (A7), then dies. Vanilla clips either re-authored or imported via `import_bedrock`.
4. First person: becomes its own small graph on the same rig (`FUTURE(first-person)`); legacy path kept until then.

## 12. Milestones

| # | Deliverable | Acceptance |
|---|---|---|
| **A0** | ✅ Build split: `mc_engine` static lib + thin game exe + `tools/` home | game builds & runs unchanged |
| **A1** | ✅ `Rig` + `Pose` + compose; PlayerModel generalized to per-bone meshes; foot locators in geometry | identity pose renders identical to today; debug-bend a forearm/shin (F6) |
| **A2** | ✅ Clips: native JSON + Bedrock import; single-clip playback | authored walk loop plays on the player (F7) |
| **A3** | ✅ Blackboard + Molang binding; `Blend1D`; sync groups; quat pose blend | idle→walk→run smoothly by real speed (F7), no foot slide |
| **A4** | ✅ State machines: transitions, cross-fades, one-shots, triggers, interrupts; graph JSON loader; state readout | stand-jump vs run-jump demo (F7; console state readout) |
| **A5** | ✅ Layers + masks (override); AnimGraph composites | right-arm wave (H) over running legs |
| **A6** | ✅ Events (dominant-clip firing) + debug snapshot ring buffer | `foot_down` logs per step, one per blended step |
| **A7** | Migration: retire PlayerAnimator; first-person parity | old system deleted; no visual regressions |
| **T1** | ✅ `anim_studio` tools exe: clip preview (orbit + scrub) + live graph timeline debugger | scrub a clip ✅; drive the graph, watch state bars/events ✅ |

Order within a session may interleave, but nothing above its dependencies.

## 13. FUTURE tag registry

| Tag | Meaning |
|---|---|
| `FUTURE(combat)` | weapon/enemy-selected attack machines, additive layer blending, hit-window events, `force` transitions (hurt/death), combo buffering |
| `FUTURE(parkour)` | `Blend2D` (height×distance), `playback_rate` arc matching, vault/climb states, world-probe params |
| `FUTURE(mounts)` | saddle sockets, rider attachment, `mount_*` param bridging, rider reaction layer |
| `FUTURE(boat)` | oar sync across entities, `oar_in_water` curve → thrust, enter/exit water events |
| `FUTURE(decals)` | footprint decals consuming `foot_down` + foot socket transform |
| `FUTURE(snow)` | snow-layer deformation consumer of the same events |
| `FUTURE(ik)` | ✅ foot-lock IK done (anim/FootIK, pinned feet via foot_down/up); hands-to-ledge still future |
| `FUTURE(root-motion)` | root displacement extraction + target matching/motion warping |
| `FUTURE(anim-debugger)` | timeline debugger in tools exe over debug snapshots |
| `FUTURE(debug-draw)` | in-game world-space debug lines (trajectories, probes, gizmos) |
| `FUTURE(sync-markers)` | foot-plant phase markers for sync beyond normalized time |
| `FUTURE(smooth-keys)` | catmull-rom keyframe interpolation |
| `FUTURE(first-person)` | first-person arm/item graph replacing the legacy delta path |
| `FUTURE(tools)` | further tools-exe apps (graph node editor, etc.) |

## 14. Decision log

- **Fully custom runtime** over Bedrock animation controllers: controllers can't express real blend trees/2D spaces cleanly; we keep Bedrock *clip* compatibility for Blockbench authoring instead. (User choice, 2026-07-11.)
- **Molang reused as the expression language** — already implemented in-tree, fit for purpose; the *system* is custom, the *expression syntax* is borrowed.
- **Physics-authoritative, no root motion** (P2) — right for a voxel game; warping kept possible, not built.
- **Events/curves authoritative for animation-timed mechanics** (P3) — determinism over duplicated simulation.
- **Editor lives in separate tools exe(s), never the game exe** (user requirement, 2026-07-11). Enabled by the A0 lib split.
- **Blockbench remains the keyframe tool**; we build the graph/debug tooling it can't do. Building a keyframe editor from scratch was evaluated and rejected (months of UI work for a worse Blockbench).
