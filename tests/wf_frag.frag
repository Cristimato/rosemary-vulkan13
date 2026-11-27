#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(location = 0) in vec2 uv;
layout(location = 1) flat in int mat;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D t[];
layout(set = 0, binding = 1) uniform texture2D ti[16];
layout(set = 0, binding = 2) uniform sampler s;
layout(set = 0, binding = 3, rgba8) uniform image2D si[4];
void main() {
    vec4 a = texture(t[nonuniformEXT(mat)], uv);
    vec4 b = texture(t[nonuniformEXT(mat + 1)], uv, 1.5);
    vec4 c = texture(sampler2D(ti[nonuniformEXT(mat & 15)], s), uv * 2.0);
    vec4 d = texelFetch(t[nonuniformEXT(mat)], ivec2(uv), 0);
    imageStore(si[nonuniformEXT(mat & 3)], ivec2(uv), a);
    o = a + b + c + d;
}
