#version 450
/* ClipDistance como variável solta + leitura de volta (formato tipo Zink) */
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
layout(location = 0) out vec2 uv;
void main() {
    vec2 v = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(v, 0.5, 1.0);
    gl_ClipDistance[0] = v.y;          /* corta a metade de cima (y<0) */
    uv = v;
}
