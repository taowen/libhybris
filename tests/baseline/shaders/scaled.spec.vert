#version 450
#ifdef HYBRIS_SPEC_DIRECT
layout(constant_id=7) const int base = 3;
#else
layout(constant_id=7) const int base = 2;
#endif
layout(constant_id=19) const float tint = 1.0;
layout(constant_id=23) const bool invert = false;
#ifdef HYBRIS_SPEC_DIRECT
const int columns = base;
#else
const int columns = ((base << 1) / 2) + 1 + (invert ? 1 : 0);
#endif
layout(location=0) in vec4 values[columns];
vec4 column(vec4 copy[columns], int i) { return copy[i]; }
layout(location=0) flat out vec4 color;
layout(push_constant) uniform Expected { vec4 values[4]; } expected;
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    color = vec4(1.0);
    for (int i = 0; i < columns; ++i)
        color *= vec4(equal(column(values, i), expected.values[i]));
    if (invert) color = vec4(1.0) - color;
    color *= tint;
}
