#version 450
layout(set = 0, binding = 0, std140) uniform constants {
    vec4 parameters[12];
    mat4 mvp;
    vec3 checkerColorAndSize;
    int srgbTarget;
#ifdef LARGE_UBO
    mat4 matrices[14];
    vec4 tail[3];
    int signedTag;
    bool enabled;
    vec2 endMarker;
#endif
} PushConstants;
layout(location = 0) out vec4 color;

void main() {
    /* Encode selected UBO fields into the pixel so a readback proves the
     * shader saw the bound range, not just that UpdateDescriptorSets ran. */
#ifdef LARGE_UBO
    bool valid = PushConstants.parameters[11] == vec4(41, 42, 43, 44);
    for (int m = 0; m < 14; ++m)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                valid = valid && PushConstants.matrices[m][c][r] ==
                                  float(1 + m * 16 + c * 4 + r);
    for (int t = 0; t < 3; ++t)
        valid = valid && PushConstants.tail[t] == vec4(51 + t, 61 + t, 71 + t, 81 + t);
    valid = valid && PushConstants.endMarker == vec2(91, 92);
    if (!valid) {
        color = vec4(1, 0, 1, 1);
        return;
    }
    color = vec4(PushConstants.enabled ? 1.0 : 0.0,
                 PushConstants.mvp[3][3],
                 PushConstants.signedTag == -37 ? 0.0 : 1.0,
                 float(PushConstants.srgbTarget));
#else
    color = vec4(PushConstants.parameters[0].x,
                 PushConstants.mvp[3][3],
                 PushConstants.checkerColorAndSize.x,
                 float(PushConstants.srgbTarget));
#endif
}
