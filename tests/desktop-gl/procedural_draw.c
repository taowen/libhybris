#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ordinary application VS: no compute source and no vertex SSBO declarations.
 * The optional Zink execution mode must convert this shader itself. */
int procedural_draw(PFNEGLGETPROCADDRESSPROC lookup) {
#define G(type, name)                                                          \
  type name = (type)lookup(#name);                                             \
  if (!name)                                                                   \
  return 2
  G(PFNGLCREATESHADERPROC, glCreateShader);
  G(PFNGLSHADERSOURCEPROC, glShaderSource);
  G(PFNGLCOMPILESHADERPROC, glCompileShader);
  G(PFNGLGETSHADERIVPROC, glGetShaderiv);
  G(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
  G(PFNGLCREATEPROGRAMPROC, glCreateProgram);
  G(PFNGLATTACHSHADERPROC, glAttachShader);
  G(PFNGLLINKPROGRAMPROC, glLinkProgram);
  G(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
  G(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
  G(PFNGLUSEPROGRAMPROC, glUseProgram);
  G(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
  G(PFNGLUNIFORM1IPROC, glUniform1i);
  G(PFNGLGENBUFFERSPROC, glGenBuffers);
  G(PFNGLBINDBUFFERPROC, glBindBuffer);
  G(PFNGLBUFFERDATAPROC, glBufferData);
  G(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
  G(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange);
  G(PFNGLGETINTEGERVPROC, glGetIntegerv);
  G(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
  G(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
  G(PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC,
    glDrawArraysInstancedBaseInstance);
  G(PFNGLVIEWPORTPROC, glViewport);
  G(PFNGLCLEARCOLORPROC, glClearColor);
  G(PFNGLCLEARPROC, glClear);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLGETERRORPROC, glGetError);
  G(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
  G(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDELETESHADERPROC, glDeleteShader);
  const char *sources[] = {
      "#version 430 core\nlayout(std140,binding=0) uniform Widget {vec4 "
      "parameters[12];mat4 MVP;vec3 checker;int srgb;};"
      "uniform int first_vertex;flat out int instance;flat out vec4 color;"
      "void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
      "instance=gl_InstanceID;vec4 "
      "pos=MVP*vec4(p[gl_VertexID-first_vertex],0,1);"
      "pos.x=pos.x*.5+(instance==0?-.5:.5);gl_Position=pos;"
      "bool "
      "valid=parameters[0].x==7&&parameters[11].w==11&&checker.z==9&&srgb==1;"
      "color=valid?(instance==0?vec4(1,0,0,1):vec4(0,1,0,1)):vec4(1,0,1,1);}",
      "#version 430 core\nflat in int instance;flat in vec4 color;out vec4 "
      "result;"
      "void "
      "main(){if((instance==0&&gl_FragCoord.x>=8)||(instance==1&&gl_FragCoord."
      "x<8))discard;result=color;}"};
  GLuint program = glCreateProgram(), shaders[2];
  for (int i = 0; i < 2; i++) {
    shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(shaders[i], 1, &sources[i], NULL);
    glCompileShader(shaders[i]);
    GLint ok;
    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[4096];
      glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
      puts(log);
      return 2;
    }
    glAttachShader(program, shaders[i]);
  }
  glLinkProgram(program);
  GLint ok;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    puts(log);
    return 2;
  }
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "first_vertex"), 7);
  GLuint vao, ubo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &ubo);
  glBindBuffer(GL_UNIFORM_BUFFER, ubo);
  GLint alignment;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
  if (alignment <= 0 || alignment > 65536)
    return 2;
  unsigned stride = ((272u + alignment - 1) / alignment) * alignment;
  unsigned char *data = malloc(stride * 3);
  if (!data)
    return 2;
  memset(data, 0, stride * 3);
  float fields[68] = {0};
  fields[0] = 7;
  fields[47] = 11;
  fields[66] = 9;
  for (int i = 0; i < 4; i++)
    fields[48 + i * 5] = 1;
  for (int record = 0; record < 2; record++) {
    unsigned char *dst = data + stride * (record + 1);
    memcpy(dst, fields, sizeof(fields));
    int32_t srgb = record + 1;
    memcpy(dst + 268, &srgb, sizeof(srgb));
  }
  glBufferData(GL_UNIFORM_BUFFER, stride * 3, data, GL_DYNAMIC_DRAW);
  free(data);
  int failures = 0;
  for (int phase = 0; phase < 3; phase++) {
    unsigned offset = stride * (phase == 0 ? 1 : 2);
    if (phase == 2) {
      int32_t srgb = 1;
      glBufferSubData(GL_UNIFORM_BUFFER, offset + 268, sizeof(srgb), &srgb);
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, offset, 272);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, 16, 16);
    glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 7, 3, 2, 5);
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int bad = 0;
    for (int i = 0; i < 256; i++) {
      unsigned char expected[4] = {255, 0, 255, 255};
      if (phase != 1) {
        expected[0] = i % 16 < 8 ? 255 : 0;
        expected[1] = i % 16 < 8 ? 0 : 255;
        expected[2] = 0;
      }
      if (memcmp(pixels + i * 4, expected, 4))
        bad++;
    }
    char name[64];
    snprintf(name, sizeof(name), "procedural-%d.rgba", phase);
    FILE *f = fopen(name, "wb");
    if (!f)
      return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), f);
    int closed = fclose(f);
    GLenum error = glGetError();
    int failed = bad || error || written != sizeof(pixels) || closed;
    printf("PROCEDURAL_VERTEX phase=%d %s bad_pixels=%d error=0x%x\n", phase,
           failed ? "FAIL" : "PASS", bad, error);
    failures += failed;
  }
  glDeleteBuffers(1, &ubo);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++)
    glDeleteShader(shaders[i]);
  return failures ? 2 : 0;
}
