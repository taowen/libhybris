#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Feasibility workload for a future VS-to-CS lowering. This does not expose
 * vertex SSBO support: the shader is explicitly written as compute today. */
int vertex_prepass(PFNEGLGETPROCADDRESSPROC lookup) {
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
  G(PFNGLGENBUFFERSPROC, glGenBuffers);
  G(PFNGLBINDBUFFERPROC, glBindBuffer);
  G(PFNGLBUFFERDATAPROC, glBufferData);
  G(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
  G(PFNGLBINDBUFFERBASEPROC, glBindBufferBase);
  G(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData);
  G(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute);
  G(PFNGLMEMORYBARRIERPROC, glMemoryBarrier);
  G(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
  G(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
  G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
  G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
  G(PFNGLDRAWARRAYSPROC, glDrawArrays);
  G(PFNGLVIEWPORTPROC, glViewport);
  G(PFNGLCLEARCOLORPROC, glClearColor);
  G(PFNGLCLEARPROC, glClear);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLGETERRORPROC, glGetError);
  G(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
  G(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDELETESHADERPROC, glDeleteShader);

  char cs[8192];
  size_t used = snprintf(cs, sizeof(cs),
                         "#version 430 core\nlayout(local_size_x=8) in;\n");
  for (int i = 0; i < 12; i++)
    used +=
        snprintf(cs + used, sizeof(cs) - used,
                 "layout(std430,binding=%d) readonly buffer B%d {uint v%d;};\n",
                 i, i, i);
  used += snprintf(
      cs + used, sizeof(cs) - used,
      "struct Vertex {vec4 position;vec4 color;};\n"
      "layout(std430,binding=12) buffer Output {Vertex vertices[];};\n"
      "layout(std430,binding=13) buffer Counter {uint count;uint ids;};\n"
      "void main(){uint id=gl_GlobalInvocationID.x;if(id>=6u)return;uint "
      "sum=0u;\n");
  for (int i = 0; i < 12; i++)
    used += snprintf(cs + used, sizeof(cs) - used, "sum+=v%d*%du;\n", i, i + 1);
  snprintf(cs + used, sizeof(cs) - used,
           "uint instance=id/3u;uint vertex=id%%3u;"
           "vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
           "vec2 pos=p[vertex];pos.x=pos.x*.5+(instance==0u?-.5:.5);"
           "vertices[id+1u].position=vec4(pos,0,1);"
           "vertices[id+1u].color=sum==650u?(instance==0u?vec4(1,0,0,1):vec4(0,"
           "1,0,1)):vec4(1,0,1,1);"
           "atomicAdd(count,1u);atomicOr(ids,1u<<id);}");
  const char *sources[] = {
      cs,
      "#version 430 core\nlayout(location=0) in vec4 "
      "position;layout(location=1) in vec4 input_color;"
      "flat out vec4 color;void "
      "main(){gl_Position=position;color=input_color;}",
      "#version 430 core\nflat in vec4 color;out vec4 result;void main(){"
      "if(color.b==0&&((color.r==1&&color.g==0&&gl_FragCoord.x>=8)||(color.g=="
      "1&&color.r==0&&gl_FragCoord.x<8)))discard;result=color;}"};
  GLuint programs[2] = {glCreateProgram(), glCreateProgram()}, shaders[3];
  GLenum types[] = {GL_COMPUTE_SHADER, GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  for (int i = 0; i < 3; i++) {
    shaders[i] = glCreateShader(types[i]);
    glShaderSource(shaders[i], 1, &sources[i], NULL);
    glCompileShader(shaders[i]);
    GLint ok;
    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[4096];
      glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
      printf("VERTEX_PREPASS_COMPILE_FAIL %d %s\n", i, log);
      return 2;
    }
    glAttachShader(programs[i != 0], shaders[i]);
  }
  for (int i = 0; i < 2; i++) {
    glLinkProgram(programs[i]);
    GLint ok;
    glGetProgramiv(programs[i], GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[4096];
      glGetProgramInfoLog(programs[i], sizeof(log), NULL, log);
      printf("VERTEX_PREPASS_LINK_FAIL %d %s\n", i, log);
      return 2;
    }
  }
  GLuint buffers[14], vao;
  glGenBuffers(14, buffers);
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  for (unsigned i = 0; i < 12; i++) {
    uint32_t value = i + 1;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[i]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(value), &value,
                 GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffers[i]);
  }
  uint32_t guarded[8 * 8];
  for (unsigned i = 0; i < 64; i++)
    guarded[i] = 0xdeadbeef;
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[12]);
  glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(guarded), guarded,
               GL_DYNAMIC_COPY);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, buffers[12]);
  uint32_t counters[2] = {0, 0};
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[13]);
  glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counters), counters,
               GL_DYNAMIC_COPY);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, buffers[13]);
  int failures = 0;
  for (int phase = 0; phase < 3; phase++) {
    /* Change one input, then restore it using the same live allocations. */
    uint32_t value = phase == 1 ? 13 : 12;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[11]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(value), &value);
    glUseProgram(programs[0]);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[12]);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, (void *)(uintptr_t)32);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void *)(uintptr_t)48);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glUseProgram(programs[1]);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[13]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[12]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(guarded), guarded);
    char image_name[64];
    snprintf(image_name, sizeof(image_name), "vertex-prepass-%d.rgba", phase);
    FILE *image = fopen(image_name, "wb");
    if (!image)
      return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), image);
    int close_result = fclose(image);
    if (written != sizeof(pixels) || close_result)
      return 2;
    int bad = 0, guards = 0;
    for (int i = 0; i < 256; i++) {
      unsigned char *p = pixels + i * 4;
      unsigned char expected[4] = {255, 0, 255, 255};
      if (phase != 1) {
        expected[0] = i % 16 < 8 ? 255 : 0;
        expected[1] = i % 16 < 8 ? 0 : 255;
        expected[2] = 0;
      }
      if (memcmp(p, expected, sizeof(expected)))
        bad++;
    }
    for (int i = 0; i < 8; i++)
      guards += guarded[i] != 0xdeadbeef || guarded[56 + i] != 0xdeadbeef;
    GLenum error = glGetError();
    int failed = bad || guards || counters[0] != (unsigned)(6 * (phase + 1)) ||
                 counters[1] != 63 || error;
    printf("VERTEX_PREPASS phase=%d %s bad_pixels=%d guards=%d count=%u ids=%u "
           "error=0x%x\n",
           phase, failed ? "FAIL" : "PASS", bad, guards, counters[0],
           counters[1], error);
    failures += failed;
  }
  glDeleteVertexArrays(1, &vao);
  glDeleteBuffers(14, buffers);
  for (int i = 0; i < 3; i++)
    glDeleteShader(shaders[i]);
  for (int i = 0; i < 2; i++)
    glDeleteProgram(programs[i]);
  return failures ? 2 : 0;
}
