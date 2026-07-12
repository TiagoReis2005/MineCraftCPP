#version 450

// Far low-detail terrain. Vertices are already in world space and carry a baked color;
// no per-chunk offset, no texture sampling.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} cam;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec3 vWorld;

void main() {
    vWorld = inPos;
    vColor = inColor;
    gl_Position = cam.proj * cam.view * vec4(inPos, 1.0);
}
