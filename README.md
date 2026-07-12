# MineCraftCPP

A Minecraft-style voxel engine written from scratch in C++ with **Vulkan 1.3**.

> Status: **M1** — runnable Vulkan window (clears the screen + draws a triangle using
> dynamic rendering + synchronization2). See `..\..\.claude\plans` for the full milestone plan.

## Prerequisites (one-time install)

This project needs a C++ toolchain, CMake, and the Vulkan SDK. On Windows with `winget`:

```powershell
winget install Kitware.CMake
winget install Microsoft.VisualStudio.2022.BuildTools   # include "Desktop development with C++"
winget install KhronosGroup.VulkanSDK                   # Vulkan headers, loader, validation, glslc
```

After installing, **close and reopen your terminal** so `PATH` and `VULKAN_SDK` are set.
Verify:

```powershell
cmake --version
glslc --version          # ships with the Vulkan SDK
echo $env:VULKAN_SDK
```

Recommended VS Code extensions: **C/C++** (`ms-vscode.cpptools`) and **CMake Tools**
(`ms-vscode.cmake-tools`).

## Build & run

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
build\bin\Debug\MineCraftCPP.exe
```

The first configure downloads GLFW and GLM via CMake `FetchContent` (needs internet + git).
Shaders are compiled to SPIR-V and, together with `assets/`, copied next to the executable
on every build.

## Textures — drop your own PNGs

The engine loads textures **by filename** from these folders (no packing step):

```
assets/textures/blocks/    <-- e.g. grass_top.png, grass_side.png, dirt.png, stone.png, torch.png
assets/textures/items/     <-- e.g. stick.png, pickaxe.png
```

Block textures should share one resolution (16x16 by default; HD packs work if all files
match). A missing texture falls back to a magenta/black checker — the engine never crashes
because a file isn't there yet. (Texture loading lands in M2.)

## Project layout

See the milestone plan for the full architecture. Source lives under `src/` split into
`core/` (window, input, camera, threads), `gfx/` (Vulkan backend), `world/`, `game/`, and
`ui/`.
