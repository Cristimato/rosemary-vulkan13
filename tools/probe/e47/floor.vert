#version 450
// chão (y = -0.2 no espaço da câmera) com um vértice ATRÁS da câmera e projeção perspectiva
// near = 1, far = 100: a parte entre o olho e o near só aparece com depthClamp de verdade.
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
void main() {
    vec3 v[3] = vec3[3](vec3(-3.0, -0.2, 10.0), vec3(3.0, -0.2, 10.0), vec3(0.0, -0.2, -2.0));
    vec3 q = v[gl_VertexIndex];
    float n = 1.0, f = 100.0;
    gl_Position = vec4(q.x, q.y, f / (f - n) * q.z - f * n / (f - n), q.z);
}
