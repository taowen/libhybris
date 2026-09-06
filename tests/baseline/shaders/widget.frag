#version 450
layout(set = 0, binding = 0, std140) uniform constants {
    vec4 parameters[12];
    mat4 mvp;
    vec3 checkerColorAndSize;
    int srgbTarget;
} PushConstants;
layout(location = 0) out vec4 color;

void main() {
    /* Encode selected UBO fields into the pixel so a readback proves the
     * shader saw the bound range, not just that UpdateDescriptorSets ran. */
    color = vec4(PushConstants.parameters[0].x,
                 PushConstants.mvp[3][3],
                 PushConstants.checkerColorAndSize.x,
                 float(PushConstants.srgbTarget));
}
