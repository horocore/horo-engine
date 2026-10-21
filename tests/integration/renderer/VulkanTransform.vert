#version 460
layout(location = 0) in vec3 position;
layout(set = 0, binding = 0, std140) uniform Material {
    vec4 tint;
    uint viewIndex;
    mat4 worldToClip[32];
    mat4 localToWorld;
} material;
void main() {
    gl_Position = material.worldToClip[material.viewIndex] * material.localToWorld * vec4(position, 1.0);
}
