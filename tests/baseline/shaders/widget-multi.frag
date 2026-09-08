#version 450
#define FIELDS vec4 parameters[12]; mat4 mvp; vec3 checkerColorAndSize; int srgbTarget;
layout(set = 0, binding = 0, std140) uniform First { FIELDS } first;
layout(set = 0, binding = 3, std140) uniform Pair { FIELDS } pair[2];
layout(set = 1, binding = 1, std140) uniform Last { FIELDS } last;
bool tags_valid() {
    return first.parameters[1].x == 101.0 && pair[0].parameters[1].x == 102.0 &&
           pair[1].parameters[1].x == 103.0 && last.parameters[1].x == 104.0;
}
layout(location = 0) flat in uint vertex_valid;
layout(location = 0) out vec4 color;
void main() {
    bool others = first.parameters[0].x == 1 && pair[0].parameters[0].x == 1 &&
                  last.parameters[0].x == 1 && first.srgbTarget == 1 &&
                  pair[0].srgbTarget == 1 && last.srgbTarget == 1;
    if (!tags_valid() || vertex_valid != 1u || !others) {
        color = vec4(1, 0, 1, 1);
        return;
    }
    color = vec4(pair[1].parameters[0].x, pair[1].mvp[3][3],
                 pair[1].checkerColorAndSize.x, float(pair[1].srgbTarget));
}
