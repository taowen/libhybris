#version 450
layout(location = 0) in vec4 value;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Expected {
    vec4 values[4];
    uvec4 config;
} expected;
void main() {
    const vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(-1,1),
                                   vec2(-1,1), vec2(1,-1), vec2(1,1));
    uint instance = gl_InstanceIndex - expected.config.x;
    vec2 position = corners[gl_VertexIndex];
    position.x = (position.x + 1.0) * 0.25 - 1.0 + float(instance) * 0.5;
    gl_Position = vec4(position, 0, 1);
    color = vec4(equal(value, expected.values[instance]));
}
