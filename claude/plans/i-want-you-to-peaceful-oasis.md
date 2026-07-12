# MineCraftCPP — Vulkan Voxel Engine (Minecraft clone from scratch)

## Context

The user wants a Minecraft clone written in C++ from scratch in an empty project
directory (`c:\Users\Jaime Reis\Desktop\MineCraftCPP`). Decisions locked in via Q&A:

- **Renderer:** Vulkan 1.3 (chosen deliberately over OpenGL, accepting the boilerplate
  cost for explicit control + multithreaded command submission). Priorities the user
  stated: *rendering techniques* and *threaded meshing* — "clean and beautiful."
- **Toolchain:** MSVC Build Tools 2022 + CMake.
- **Scope:** "Go bigger" — full creative experience **plus** multi-chunk streaming around
  the player **plus** world save/load.
- **Game modes:** Creative, Survival, and Spectator (a `GameMode` system).
- **Textures (explicit request):** the user manually drops PNGs into folders like
  `assets/textures/blocks/torch.png` and `assets/textures/items/...`. The engine scans
  these folders by filename and loads them — no asset packing step on their side.
- **Inventory rendering (explicit request):** blocks in the hotbar/inventory are drawn as
  real 3D isometric block icons (the cube with its own face textures), exactly like
  Minecraft; flat items render as 2D sprites.

**Environment reality check (already verified):** the machine currently has **no C++
compiler, no CMake, no Vulkan SDK** — only Git, VS Code, and winget. So milestone 0 is
installing the toolchain. Windows 11, x64.

**Honest expectation-setting:** Vulkan + streaming + 3 game modes + threaded meshing +
a beauty pass is a multi-week project, not a single commit. This plan is therefore staged
into milestones, each independently runnable, so progress is visible early and often. The
first *visible* result is a cleared Vulkan window; the first *fun* result is M3.

---

## Tech stack & dependencies

| Concern            | Choice                                   | How it's pulled in |
|--------------------|------------------------------------------|--------------------|
| Window + input + surface | GLFW                               | CMake `FetchContent` |
| Math               | GLM                                      | CMake `FetchContent` |
| GPU memory         | VMA (Vulkan Memory Allocator)            | `FetchContent` / vendored header |
| Vulkan fn loading  | volk (meta-loader, avoids loader trampoline) | `FetchContent` / vendored |
| PNG decode         | stb_image (single header)                | vendored in `external/` |
| PNG encode (icon/placeholder dump, optional) | stb_image_write          | vendored in `external/` |
| Vulkan headers/loader/validation/`glslc` | LunarG Vulkan SDK            | winget (system install) |
| Build              | CMake ≥ 3.24 (FetchContent + Vulkan find module) | winget |
| Debug overlay (optional, later) | Dear ImGui (Vulkan+GLFW backend)| `FetchContent` |

**Vulkan feature level:** 1.3 core, using **dynamic rendering** (`VK_KHR_dynamic_rendering`)
and **synchronization2** — this removes render-pass/framebuffer object boilerplate and
modernizes barriers. Shaders authored in GLSL, compiled to SPIR-V at build time via `glslc`
(CMake custom commands).

---

## Project structure

```
MineCraftCPP/
├── CMakeLists.txt
├── README.md                      # build + run + "where to drop textures"
├── .gitignore                     # build/, external downloads
├── external/                      # vendored single-header libs
│   ├── stb_image.h
│   └── stb_image_write.h
├── assets/
│   └── textures/
│       ├── blocks/                # <-- USER DROPS grass_top.png, dirt.png, torch.png ...
│       └── items/                 # <-- USER DROPS stick.png, pickaxe.png ...
├── shaders/                       # GLSL source; compiled to SPIR-V into build/shaders/
│   ├── chunk.vert / chunk.frag    # terrain (opaque)
│   ├── water.vert / water.frag    # transparent pass
│   ├── icon.vert  / icon.frag     # 3D block-icon offscreen render
│   └── ui.vert    / ui.frag       # 2D sprites / hotbar / crosshair / font
└── src/
    ├── main.cpp                   # entry, game loop, fixed-timestep
    ├── core/
    │   ├── Window.{h,cpp}         # GLFW window + Vulkan surface, resize events
    │   ├── Input.{h,cpp}          # keyboard/mouse state, captured cursor
    │   ├── Camera.{h,cpp}         # view/proj, frustum (for culling)
    │   └── ThreadPool.{h,cpp}     # worker pool for meshing + worldgen
    ├── gfx/                       # Vulkan backend (the boilerplate, contained here)
    │   ├── VkContext.{h,cpp}      # instance, debug messenger, device, queues
    │   ├── Swapchain.{h,cpp}      # swapchain + depth, recreation on resize
    │   ├── Allocator.{h,cpp}      # VMA wrapper
    │   ├── Buffer.{h,cpp}         # buffer/staging upload helpers
    │   ├── Image.{h,cpp}          # image create, transitions, mipmaps
    │   ├── Pipeline.{h,cpp}       # graphics pipeline + layout builder
    │   ├── Descriptors.{h,cpp}    # descriptor pool/set helpers
    │   ├── FrameContext.{h,cpp}   # frames-in-flight, per-frame cmd/sync/UBO
    │   ├── TextureArray.{h,cpp}   # folder scan -> 2D texture array + name->layer map
    │   └── Renderer.{h,cpp}       # ties passes together; records the frame
    ├── world/
    │   ├── Block.{h,cpp}          # BlockId, BlockData (faces, flags, render type)
    │   ├── BlockRegistry.{h,cpp}  # block defs; resolves face names -> layer indices
    │   ├── Section.{h,cpp}        # 16x16x16 voxel storage (uint16/palette)
    │   ├── Chunk.{h,cpp}          # column of sections; meshing state
    │   ├── World.{h,cpp}          # chunk map, streaming, get/set block
    │   ├── WorldGen.{h,cpp}       # noise heightmap + surface/strata (+ caves later)
    │   ├── Noise.{h,cpp}          # Perlin/simplex + FBM
    │   ├── Lighting.{h,cpp}       # skylight + block-light BFS (beauty pass)
    │   ├── Mesher.{h,cpp}         # face-cull mesh + baked AO (runs on workers)
    │   └── Region.{h,cpp}         # save/load chunk data to disk
    ├── game/
    │   ├── GameMode.h             # enum: Creative, Survival, Spectator
    │   ├── Player.{h,cpp}         # mode-aware movement + AABB collision
    │   ├── Raycast.{h,cpp}        # DDA voxel traversal for targeted block
    │   └── Inventory.{h,cpp}      # hotbar + inventory slots, item stacks
    └── ui/
        ├── ItemIconRenderer.{h,cpp} # 3D isometric block icons -> cached textures
        ├── UIRenderer.{h,cpp}        # 2D quads/sprites
        ├── Font.{h,cpp}              # bitmap font (counts, F3 debug)
        └── Hud.{h,cpp}              # crosshair, hotbar, inventory screen
```

---

## Milestones (each is runnable)

**M0 — Toolchain & skeleton.** Install MSVC + CMake + Vulkan SDK. Create `CMakeLists.txt`
with FetchContent deps, shader-compile custom commands, and asset copy. App opens a GLFW
window. *Verify: window opens, clean exit.*

**M1 — Vulkan triangle → cleared frame.** `VkContext`, `Swapchain`, `FrameContext`
(2 frames in flight, dynamic rendering), one pipeline drawing a hardcoded triangle; then
a full-screen clear. Validation layers on in debug. Window resize recreates swapchain.
*Verify: colored triangle, no validation errors, resize works.*

**M2 — Textured chunk.** `TextureArray` scans `assets/textures/blocks/` into a layer array
+ name→layer map (magenta fallback for missing files). `Block`/`BlockRegistry`, one
`Section`, `Mesher` (face culling), `Camera` + fly controls. Render one textured 16³ chunk.
*Verify: a textured chunk; missing PNGs show magenta, not a crash.*

**M3 — World + streaming + worldgen (first "fun" build).** `World` chunk map keyed by
(cx,cz); `WorldGen` noise terrain (grass/dirt/stone strata); `ThreadPool` runs worldgen +
meshing on workers, main/transfer queue uploads results; load/unload columns by render
distance; frustum culling. *Verify: fly over an endless generated landscape, smooth
streaming, no main-thread hitching.*

**M4 — Interaction + game modes.** `Raycast` block targeting; break/place; `GameMode`
(Creative fly + instant break + infinite blocks; Survival gravity + AABB collision + break
time by hardness; Spectator noclip). Mode switch key. *Verify: break/place works; each mode
behaves correctly.*

**M5 — HUD + 3D inventory icons (explicit request).** `ItemIconRenderer` renders each block
as an isometric cube to a cached offscreen texture; `Hud` draws hotbar, crosshair, selected
highlight, item counts; inventory screen (E). Flat `items/*.png` draw as 2D sprites.
*Verify: hotbar shows real 3D block icons like Minecraft; selecting + placing matches.*

**M6 — Save/load + beauty pass.** `Region` serializes chunks (RLE/palette) + player/mode to
disk, loads on demand. Beauty: `Lighting` (skylight + block-light flood fill, smooth/AO
shading), distance fog, sky/day color, transparent water pass, mipmaps + anisotropic
sampling. Optional: shadow map, bloom, Dear ImGui debug overlay. *Verify: quit/relaunch
restores the world; lighting/fog/water look right.*

---

## Key subsystems (detail on the explicitly-requested parts)

### Folder-based texture pipeline (`gfx/TextureArray`)
- On startup, enumerate `assets/textures/blocks/*.png`. Each file → one layer of a
  `VK_IMAGE_TYPE_2D` **array** image; record `std::unordered_map<string,uint32_t>`
  (filename-stem → layer). Validate uniform dimensions (e.g. 16×16; configurable, supports
  HD packs if all consistent). Generate mipmaps; **nearest** min/mag for the crisp MC look
  (anisotropy + linear optional via setting).
- **Missing-texture safety:** a procedurally generated magenta/black checker occupies layer
  0; any unresolved name maps to it. The engine never crashes because a PNG isn't dropped
  yet — directly honoring the "I drop files in manually" workflow.
- Block definitions (`BlockRegistry`) reference textures **by name per face**, e.g.
  `grass = { top:"grass_top", bottom:"dirt", side:"grass_side" }`. Names are resolved to
  layer indices once at load. Defs live in C++ first; can migrate to a JSON data file later.
- `items/*.png` load into a separate sprite atlas/array for flat-item rendering.

### 3D inventory block icons (`ui/ItemIconRenderer`) — the headline feature
- For each `BlockData` whose `iconType == CUBE`, render the unit cube **once** into a small
  offscreen color+depth image (e.g. 96×96) using:
  - **orthographic** projection,
  - the classic Minecraft GUI transform (≈ rotate 30° about X, 45° about Y) so the top face
    + two side faces are visible,
  - the same block texture array + that block's per-face layer indices, with simple
    per-face shading (top brightest, sides dimmer) for depth readability.
- Cache the result as a sampled texture (`icon.{vert,frag}` pipeline). Re-render only when
  textures change (effectively once) — far cheaper than redrawing 3D per slot per frame.
- `UIRenderer`/`Hud` then blit the cached icon into hotbar/inventory slots as a 2D sprite,
  with item-count text via the bitmap `Font`.
- `iconType` per block/item: `CUBE` (3D icon), `FLAT` (2D `items/` sprite, e.g. stick/tool),
  `CROSS` (plants/torch — flat-ish). This is the data switch the user described: "render for
  the blocks with the type of item like the block itself."

### Threaded meshing (stated priority)
- `ThreadPool` runs worldgen and meshing jobs. `Mesher` produces CPU vertex/index buffers
  (face culling + baked per-vertex AO) with **no Vulkan calls**, so it's safely parallel.
- Completed meshes go to a results queue; the render thread (or a dedicated **transfer
  queue** with double-buffered staging) uploads to device-local buffers and swaps the
  chunk's draw buffers atomically. Dirty chunks (after block edits) are re-queued.
- Frustum-cull chunks against the camera each frame before recording draws.

### Game modes (`game/GameMode`, `game/Player`)
- **Creative:** toggle-fly, no fall damage, instant break, infinite block supply.
- **Survival:** gravity + jump + AABB-vs-voxel collision (swept box), break time scaled by
  block hardness, finite inventory; health/hunger stubbed first, fleshed out later.
- **Spectator:** noclip free-fly, passes through blocks, no world interaction.
- `Player` branches movement/interaction on the active mode; a key cycles modes (creative
  default for early testing).

### Save/load (`world/Region`)
- Per-region (e.g. 16×16 columns) or per-column files under a `saves/<world>/` dir. Block
  data stored with a per-section palette + RLE/bit-packing. Player position, look, selected
  hotbar slot, and game mode persisted. Columns load lazily as streaming requests them.

---

## Build & run

**M0 install (one time):**
```powershell
winget install Kitware.CMake
winget install Microsoft.VisualStudio.2022.BuildTools   # add "Desktop development with C++"
winget install KhronosGroup.VulkanSDK                   # provides headers, loader, validation, glslc
```
(Confirm exact IDs with `winget search` if any differ; reopen the shell afterward so PATH /
`VULKAN_SDK` are set. Recommended VS Code extensions: **C/C++** and **CMake Tools**.)

**Configure / build / run:**
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
build\Debug\MineCraftCPP.exe
```
CMake compiles `shaders/*` → `build/.../shaders/*.spv` and copies `assets/` next to the exe,
so dropping a PNG into `assets/textures/blocks/` and rebuilding (or re-running) picks it up.

## Verification (end-to-end, per milestone)
- Run the exe after each milestone and confirm the milestone's "Verify" line above.
- Validation layers enabled in Debug — treat any validation error/warning as a failure to
  fix before moving on.
- Texture workflow check: drop `dirt.png`, `grass_top.png`, `grass_side.png`, `stone.png`
  into `assets/textures/blocks/`, rebuild, confirm terrain + the 3D hotbar icons use them;
  delete one and confirm the magenta fallback (no crash).
- Game-mode check: cycle Creative/Survival/Spectator and confirm fly/gravity/noclip and
  break/place behavior per mode.
- Persistence check (M6): build something, quit, relaunch, confirm it's restored.

> Note: I can't compile/run this in the planning environment (no GPU/display, toolchain not
> yet installed). Verification happens on your machine after M0; I'll guide each run and fix
> validation/build errors as they surface.

## Open defaults (chosen now, easily changed later)
- Section size 16³, columns 16 sections tall (256 build height) — adjustable.
- Texture resolution 16×16 default; HD packs work if all files share one size.
- Noise: Perlin FBM heightmap first; caves/biomes deferred to a later pass.
- 2 frames in flight; render distance default ~8–12 columns (tunable).
