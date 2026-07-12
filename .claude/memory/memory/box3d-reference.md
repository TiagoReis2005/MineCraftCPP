---
name: box3d-reference
description: "Erin Catto's Box3D physics engine — user asked about it; assessed as NOT for core MC physics, maybe for later juice (debris/ragdolls)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 2731a1e1-53ff-4b67-b728-38c4d8ac85ed
---

https://github.com/erincatto/box3d — Erin Catto's (Box2D author) 3D physics engine. v0.1.0 released 2026-06-30, MIT, C17 core (CMake, FetchContent-friendly), rigid bodies + joints + CCD, multithreaded/SIMD, cross-platform deterministic with record/replay. PRs disabled (issues/Discord only).

Assessment shared with user (2026-07-10): do NOT use for core [[minecraftcpp-project]] physics — the game intentionally mirrors Minecraft's 20 TPS AABB voxel sweeps with exact vanilla constants, and a rigid-body solver fights that. Candidate for later optional features: TNT debris, tumbling falling blocks, ragdolls, physics minecarts. Determinism could matter if multiplayer/replays ever happen.
