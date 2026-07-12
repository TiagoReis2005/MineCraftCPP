#version 450

layout(location = 0) in vec3 inPos; // outline geometry in unit-box 0..1 space

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} cam;

layout(push_constant) uniform Push {
    vec4 boxMin;  // world-space min corner of the selection box
    vec4 boxSize; // box extents
} pc;

void main() {
    // Scale slightly outward from the box center so the bars sit just off the faces.
    const float inflate = 0.002;
    vec3 local = inPos * pc.boxSize.xyz;
    vec3 center = pc.boxSize.xyz * 0.5;
    vec3 world = pc.boxMin.xyz + center + (local - center) * (1.0 + 2.0 * inflate);
    gl_Position = cam.proj * cam.view * vec4(world, 1.0);
}
