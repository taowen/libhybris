#version 450
layout(location=0) in vec4 value;
layout(location=0) flat out vec4 color;
layout(push_constant) uniform Expected { vec4 value; } expected;
#ifdef HYBRIS_SCALED_MULTI
vec4 inspect_value(vec4 fetched) { return vec4(equal(fetched, expected.value)); }
#endif
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
#ifdef HYBRIS_SCALED_MULTI
    color = inspect_value(value);
#else
    color = vec4(equal(value, expected.value));
#endif
#ifdef HYBRIS_SCALED_ALTERNATE
    color = vec4(1.0) - color;
#endif
}
