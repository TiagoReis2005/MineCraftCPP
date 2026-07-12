#version 450

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec3 vWorld;

layout(location = 0) out vec4 outColor;

void main() {
    // No fog: the far terrain is shown at full color so the whole world stays visible.
    outColor = vec4(vColor, 1.0);
}
