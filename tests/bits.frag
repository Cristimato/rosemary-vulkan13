#version 450
layout(location=0) flat in uvec4 v;
layout(location=0) out vec4 o;
layout(constant_id=0) const bool SB = true;
void main(){ uvec4 s = (v.yxwz << uvec4(3u)) & uvec4(v.x, v.x, 7u, 9u); bvec4 b = bvec4(SB, true, SB, false);
  float c = clamp(float(s.x), 0.0, 1.0); o = vec4(s) * c + vec4(b); }
