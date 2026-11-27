#version 450
// carga de textura: 4 amostras de uma textura 2048x2048 com mipmaps
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D t;
void main() {
    vec4 a = texture(t, uv) + texture(t, uv * 1.37 + 0.1) + texture(t, uv * 0.71 + 0.3) + texture(t, uv * 2.1 + 0.7);
    o = a * 0.0625;    // média das 4 amostras dividida pelos 4 desenhos (sem saturar)
}
