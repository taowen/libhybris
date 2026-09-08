#version 450
#ifndef WIDTH
#define WIDTH 4
#endif
#if WIDTH == 1
layout(location=0) in float value;
#elif WIDTH == 2
layout(location=0) in vec2 value;
#elif WIDTH == 3
layout(location=0) in vec3 value;
#else
layout(location=0) in vec4 value;
#endif
layout(location=0) flat out vec4 color;
layout(push_constant) uniform Expected { vec4 value; } expected;
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    color = vec4(1.0);
#if WIDTH == 1
    color.x = abs(value - expected.value.x) < 0.00001 ? 1.0 : 0.0;
#else
    // Runtime indexing exercises Input access chains, including short vectors.
    for (int i = 0; i < WIDTH; ++i) {
        int c = (i + gl_VertexIndex) % WIDTH;
        color[c] = abs(value[c] - expected.value[c]) < 0.00001 ? 1.0 : 0.0;
    }
#endif
}
