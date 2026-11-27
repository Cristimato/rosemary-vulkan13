#version 450
#extension GL_ARB_shader_draw_parameters : require
// um ponto por draw, na coluna gl_VertexIndex; o FS pinta com gl_DrawID
layout(location = 0) flat out int did;
void main() {
    gl_Position = vec4((float(gl_VertexIndex) + 0.5) / 8.0 * 2.0 - 1.0, (0.5) / 8.0 * 2.0 - 1.0, 0.5, 1.0);
    gl_PointSize = 1.0;
    did = gl_DrawIDARB;
}
