#version 450

// Screen-space decal: reconstruct the visible surface from scene depth and paint the
// destroy_stage texture onto whatever lies inside the decal volume. Faces hidden by
// neighbors are never painted (they are not in the depth buffer) and there is nothing
// to z-fight with — the paint IS at the surface.
//
// Precision strategy: reconstruct in VIEW space (small numbers -> clean per-pixel
// derivatives; a world-space inverse mixes world-scale translations into every pixel
// and its noise drowns the derivatives). Rotate to world, snap the face plane to the
// 1/16 block-pixel grid (all block geometry lies on it), and re-intersect the view
// ray: positions and crack UVs come out exact.

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 invProj;
} cam;
layout(set = 0, binding = 1) uniform sampler2DArray blockTextures;
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;

layout(push_constant) uniform Push {
    vec4 boxMin;  // world-space min corner of the decal volume
    vec4 boxSize; // volume extents; .w = destroy_stage texture layer
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d >= 1.0) discard; // sky
    vec2 uvScreen = gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0));

    // View-space surface position, then world = R^T * viewPos + camPos.
    vec4 vp = cam.invProj * vec4(uvScreen * 2.0 - 1.0, d, 1.0);
    vec3 viewPos = vp.xyz / vp.w;
    mat3 R = mat3(cam.view);
    vec3 camPos = -(vec3(cam.view[3]) * R);
    vec3 ray = viewPos * R; // R^T * viewPos: camera-to-surface, world axes
    vec3 world = camPos + ray;

    // The sampled surface itself must lie inside the decal volume: a nearer occluder
    // whose plane cuts through the volume must never be painted — the snap below only
    // REFINES a point known to be on the cracked block.
    const float pre = 0.02;
    vec3 bMin = pc.boxMin.xyz, bMax = pc.boxMin.xyz + pc.boxSize.xyz;
    if (any(lessThan(world, bMin - pre)) || any(greaterThan(world, bMax + pre))) discard;

    // Face axis from derivatives (stable: view-space reconstruction), snapped plane,
    // exact re-intersection.
    vec3 an = abs(cross(dFdx(world), dFdy(world)));
    int axis = (an.y >= an.x && an.y >= an.z) ? 1 : (an.x >= an.z ? 0 : 2);
    if (abs(ray[axis]) < 1e-6) discard; // grazing: no stable intersection
    float plane = round(world[axis] * 16.0) / 16.0;
    world = camPos + ray * ((plane - camPos[axis]) / ray[axis]);

    // Axis-aligned camera-facing normal of the snapped face.
    vec3 n = vec3(0.0);
    n[axis] = -sign(ray[axis]);

    // Tight volume test on the exact (snapped) position. Kept small so the thin
    // outline bars paint lines of their true width.
    const float slack = 1.5e-3;
    if (any(lessThan(world, bMin - slack)) || any(greaterThan(world, bMax + slack))) discard;

    // Reject surfaces that hug a volume boundary but face INWARD: that is the ground
    // or wall AROUND the block, not the block itself (fence base on grass).
    const float be = 2e-3;
    if (world.x < bMin.x + be && n.x > 0.5) discard;
    if (world.x > bMax.x - be && n.x < -0.5) discard;
    if (world.y < bMin.y + be && n.y > 0.5) discard;
    if (world.y > bMax.y - be && n.y < -0.5) discard;
    if (world.z < bMin.z + be && n.z > 0.5) discard;
    if (world.z > bMax.z - be && n.z < -0.5) discard;

    // Plain color mode (boxSize.w < 0): selection outline bars. Under the modulate-2x
    // blend a near-black source carves a crisp dark line into the surface.
    if (pc.boxSize.w < 0.0) {
        outColor = vec4(0.03, 0.03, 0.03, 1.0);
        return;
    }

    // Tri-planar, world-anchored at natural block scale: the two components tangent
    // to the face are the crack UVs (16 crack texels per block).
    vec2 uv;
    if (axis == 1)      uv = world.xz;
    else if (axis == 0) uv = world.zy;
    else                uv = world.xy;

    vec4 c = texture(blockTextures, vec3(fract(uv), pc.boxSize.w));
    if (c.a < 0.05) discard;
    // Pipeline blends "modulate 2x" (out = 2 * src * dst): output the crack texel
    // straight — the blender does the lighten/darken against the block pixels.
    outColor = vec4(c.rgb, 1.0);
}
