#version 450

// Screen-space decal volume: the unit cube is scaled to the decal box (slightly padded
// so surfaces lying exactly on the box boundary survive depth-reconstruction error).
// All the painting logic lives in the fragment shader.

layout(location = 0) in vec3 inPos; // unit cube 0..1 (shared with the outline)

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 invProj;
} cam;

layout(push_constant) uniform Push {
    vec4 boxMin;  // world-space min corner of the decal volume
    vec4 boxSize; // volume extents; .w = destroy_stage texture layer
} pc;

void main() {
    const float pad = 0.01;
    vec3 world = pc.boxMin.xyz - pad + inPos * (pc.boxSize.xyz + 2.0 * pad);
    gl_Position = cam.proj * cam.view * vec4(world, 1.0);
}
