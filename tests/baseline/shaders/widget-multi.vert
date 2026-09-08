#version 450
#define FIELDS vec4 parameters[12]; mat4 mvp; vec3 checkerColorAndSize; int srgbTarget;
layout(set = 0, binding = 0, std140) uniform First { FIELDS } first;
layout(set = 0, binding = 3, std140) uniform Pair { FIELDS } pair[2];
layout(set = 1, binding = 1, std140) uniform Last { FIELDS } last;
bool tags_valid() {
    return first.parameters[1].x == 101.0 && pair[0].parameters[1].x == 102.0 &&
           pair[1].parameters[1].x == 103.0 && last.parameters[1].x == 104.0;
}
layout(location = 0) flat out uint vertex_valid;
void main() {
    const vec2 corners[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
    vec4 pos = vec4(corners[uint(gl_VertexIndex) % 4u], 0, 1);
    gl_Position = (first.mvp * pos + pair[0].mvp * pos + pair[1].mvp * pos + last.mvp * pos) * 0.25;
    vertex_valid = tags_valid() ? 1u : 0u;
}
