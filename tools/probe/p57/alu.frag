#version 450
// carga de ALU típica de iluminação: muitas operações float por pixel
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 v0;
layout(location = 2) in vec4 v1;
layout(location = 3) in vec4 v2;
layout(location = 4) in vec4 v3;
layout(location = 0) out vec4 o;
void main() {
    vec3 n = normalize(vec3(v0.xy, 1.0) + v1.xyz * 0.1);
    vec3 c = vec3(0.0);
    for (int i = 0; i < 24; i++) {
        vec3 l = normalize(vec3(sin(float(i) * 1.3 + v2.x), cos(float(i) * 0.7 + v2.y), 0.8));
        vec3 h = normalize(l + vec3(0.0, 0.0, 1.0));
        float nl = max(dot(n, l), 0.0), nh = max(dot(n, h), 0.0);
        c += vec3(0.8, 0.7, 0.6) * nl + pow(nh, 32.0) * 0.5 + v3.xyz * 0.01;
    }
    o = vec4(c / 96.0, 0.25);
}
