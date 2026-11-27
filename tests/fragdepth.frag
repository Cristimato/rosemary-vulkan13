#version 450
// escreve gl_FragDepth a partir do gl_FragCoord (como os 2 FS do BT3)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    o = vec4(uv, gl_FragCoord.z, 1.0);
    gl_FragDepth = gl_FragCoord.z * 0.5 + uv.x * 0.001;
}
