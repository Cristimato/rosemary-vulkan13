#version 460
layout(location = 0) flat out int id;
void main() {
    int q = gl_DrawID;
    vec2 v = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 0.5;   /* triângulo 0..1 */
    vec2 o = vec2(float(q & 1), float(q >> 1));
    gl_Position = vec4((o + v * 0.98) - 1.0, 0.5, 1.0);
    id = q;
}
