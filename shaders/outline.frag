#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.02, 0.02, 0.02, 1.0); // clean near-black selection lines
}
