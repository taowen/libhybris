#version 450
layout(push_constant) uniform Mode { uint value; } mode;
layout(location=0) flat out vec4 color;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = mode.value == 0 ? vec4(p * 2.0 - 1.0, 0, 1) : vec4(0.0625, 0.0625, 0, 1);
    gl_PointSize = 1.0;
    color = vec4(1.0);
#ifdef POINT_READ
    color = vec4(gl_PointSize);
#endif
#ifdef POINT_MIXED
    if (mode.value == 2) gl_PointSize = 5.0;
#endif
}
