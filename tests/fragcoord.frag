#version 450
// lê gl_FragCoord (componente z e vetor inteiro) sem escrever profundidade
layout(location = 0) out vec4 o;
void main() { float z = gl_FragCoord.z; o = gl_FragCoord * 0.001 + vec4(z); }
