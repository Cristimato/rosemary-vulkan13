#version 460
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
layout(location = 0) flat out int id;
layout(location = 1) out vec4 c;
void main() {
    vec2 v = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v - 1.0, p.z, 1.0);
    id = gl_DrawID; c = p.col;
}
