#version 450
layout(location = 0) in vec3 pos;
void main() {
  gl_Position = vec4(pos, 1.0);
  gl_ClipDistance[0] = pos.x;
  gl_ClipDistance[1] = pos.y;
  gl_ClipDistance[2] = 1.0;
  gl_ClipDistance[3] = 1.0;
  gl_ClipDistance[4] = 1.0;
  gl_ClipDistance[5] = 1.0;
}
