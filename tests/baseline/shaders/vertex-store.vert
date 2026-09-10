#version 450
layout(push_constant) uniform Phase { uint value; } phase;
layout(set=0, binding=0, std430) buffer Result {
    uint count; uint mask; uint padding0; uint padding1;
    uvec4 values[32];
} result;
layout(location=0) flat out vec4 color;
void main() {
    uint actual = uint(gl_VertexIndex) - 7u;
    uint vertex = (phase.value & 256u) == 0u ? actual : actual == 0u ? 0u : actual == 4u ? 1u : 2u;
    uint instance = uint(gl_InstanceIndex) - 5u;
    atomicAdd(result.count, 1u);
    atomicOr(result.mask, 1u << (instance * 3u + vertex));
    uint slot = instance * 16u + actual;
    atomicExchange(result.values[slot].x, uint(gl_VertexIndex));
    atomicExchange(result.values[slot].y, uint(gl_InstanceIndex));
    atomicExchange(result.values[slot].z, phase.value);
    atomicExchange(result.values[slot].w, 0xabcdefu);
    vec2 p = vec2((vertex << 1u) & 2u, vertex & 2u);
    gl_Position = vec4(p * 2.0 - 1.0, 0, 1);
    if ((phase.value & 255u) == 1u) gl_Position.x += 8.0;
    color = vec4(1.0);
}
