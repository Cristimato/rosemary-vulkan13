#version 450
// plano inclinado em perspectiva cobrindo a tela (texturas vistas de lado = anisotrópico de verdade)
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 v0;
layout(location = 2) out vec4 v1;
layout(location = 3) out vec4 v2;
layout(location = 4) out vec4 v3;
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 q = p[gl_VertexIndex];
    float d = mix(1.0, 12.0, (q.y + 1.0) * 0.5);     // profundidade cresce para cima
    gl_Position = vec4(q, 0.5, 1.0);
    uv = vec2(q.x * d, d * 4.0) * 0.25;
    v0 = vec4(q, d, 1.0); v1 = v0.yzwx * 0.5; v2 = v0.zwxy * 0.25; v3 = v0.wxyz * 0.125;
}
