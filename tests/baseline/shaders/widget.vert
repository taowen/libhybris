#version 450
layout(set = 0, binding = 0, std140) uniform constants {
    vec4 parameters[12];
    mat4 mvp;
    vec3 checkerColorAndSize;
    int srgbTarget;
} PushConstants;

void main() {
    /* 12-vertex / 18-index widget-shaped box covering NDC. */
    const vec2 corners[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0),
                                   vec2(1.0, 1.0), vec2(-1.0, 1.0));
    uint i = uint(gl_VertexIndex) % 4u;
    vec4 pos = vec4(corners[i], 0.0, 1.0);
    /* Touch MVP so a zero matrix is distinguishable from identity. */
    pos = PushConstants.mvp * pos;
    gl_Position = pos;
}
