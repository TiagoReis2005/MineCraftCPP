#version 450

layout(location = 0) in vec3 inPos; // inward-facing unit cube around the camera
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform Push {
    mat4 viewProj; // rotation-only view * projection (the cube never translates)
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vUV = inUV;
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
}
