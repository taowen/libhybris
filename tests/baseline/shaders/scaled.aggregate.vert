#version 450
#if HYBRIS_AGGREGATE == 3
layout(location=0) in mat4 values;
vec4 column(mat4 copy, int i) { return copy[i]; }
#elif HYBRIS_AGGREGATE == 4
layout(location=0) in vec4 values[4];
vec4 column(vec4 copy[4], int i) { return copy[i]; }
#elif HYBRIS_AGGREGATE == 5
layout(location=0) in vec4 values[2][2];
vec4 column(vec4 copy[2][2], int i) { return copy[i / 2][i % 2]; }
#else
layout(location=0) in mat2x4 values[2];
vec4 column(mat2x4 copy[2], int i) { return copy[i / 2][i % 2]; }
#endif
layout(location=0) flat out vec4 color;
layout(push_constant) uniform Expected { vec4 values[4]; } expected;
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    color = vec4(1.0);
    for (int i = 0; i < 4; ++i)
        color *= vec4(equal(column(values, i), expected.values[i]));
}
