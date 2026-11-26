#version 450
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
layout(location = 0) out vec2 uv;
void main() {
    /* triângulo dentro da tela; p.w < 0 inverte a ordem dos vértices */
    vec2 v[3] = vec2[3](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.0, 0.5));
    int i = p.w < 0.0 ? 2 - gl_VertexIndex : gl_VertexIndex;
    gl_Position = vec4(v[i], 0.5, 1.0);
    uv = v[i];
}
