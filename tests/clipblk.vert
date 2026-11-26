#version 450
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[2]; };
void main() {
    vec2 v = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(v, 0.5, 1.0);
    gl_ClipDistance[0] = v.x;          /* corta a metade esquerda */
    gl_ClipDistance[1] = 1.0;
}
