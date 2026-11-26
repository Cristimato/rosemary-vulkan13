#version 450
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
void main() {
    vec2 v = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v * 2.0 - 1.0, p.z, 1.0) * p.w;
}
