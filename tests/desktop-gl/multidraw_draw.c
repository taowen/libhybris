#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int multidraw_draw(PFNEGLGETPROCADDRESSPROC lookup) {
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
  G(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData);
  G(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
  G(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
  G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
  G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
  G(PFNGLVIEWPORTPROC, glViewport);
  G(PFNGLCLEARCOLORPROC, glClearColor);
  G(PFNGLCLEARPROC, glClear);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLGETERRORPROC, glGetError);
  G(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
  G(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDELETESHADERPROC, glDeleteShader);
  G(PFNGLMULTIDRAWARRAYSPROC, glMultiDrawArrays);
  G(PFNGLMULTIDRAWELEMENTSBASEVERTEXPROC, glMultiDrawElementsBaseVertex);
  G(PFNGLDRAWARRAYSPROC, glDrawArrays);
  const char *source[2] = {
      "#version 430 core\n#extension GL_ARB_shader_draw_parameters : require\n"
      "layout(location=0) in vec2 position; uniform int indexed;"
      "flat out int id;flat out int valid;void main(){id=gl_DrawIDARB;"
      "int start=id==0?3:id==2?6:9;"
      "int base=indexed==0?0:id==0?3:id==2?-2:8;"
      "valid=int(gl_VertexID>=start&&gl_VertexID<start+3&&"
      "gl_BaseVertexARB==base&&gl_InstanceID==0&&gl_BaseInstanceARB==0);"
      "gl_Position=vec4(position,0,1);}",
      "#version 430 core\nflat in int id;flat in int valid;out vec4 color;"
      "void main(){int band=gl_FragCoord.x<5?0:gl_FragCoord.x<10?2:3;"
      "if(id!=band)discard;color=valid==0?vec4(1,0,1,1):"
      "id==0?vec4(1,0,0,1):id==2?vec4(0,1,0,1):vec4(0,0,1,1);}"};
  GLuint program = glCreateProgram(), shaders[2];
  for (int i = 0; i < 2; i++) {
    shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(shaders[i], 1, &source[i], NULL);
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
  GLuint vao, buffers[3];
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(3, buffers);
  float positions[12][2];
  for (int i = 0; i < 12; i++) {
    positions[i][0] = i % 3 == 1 ? 3 : -1;
    positions[i][1] = i % 3 == 2 ? 3 : -1;
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, NULL);
  glEnableVertexAttribArray(0);
  int failures = 0;
  for (int phase = 0; phase < 5; phase++) {
    glUniform1i(glGetUniformLocation(program, "indexed"), phase > 0 && phase < 4);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* Empty draw 1 must consume a DrawID without running a vertex shader. */
    GLsizei counts[4] = {3, 0, 3, 3};
    if (phase == 0) {
      GLint starts[4] = {3, 0, 6, 9};
      glMultiDrawArrays(GL_TRIANGLES, starts, counts, 4);
    } else if (phase == 4) {
      glDrawArrays(GL_TRIANGLES, 3, 3);
    } else {
      unsigned width = 1u << (phase - 1);
      unsigned char indices[40] = {0};
      const unsigned values[10] = {99, 0, 1, 2, 8, 9, 10, 1, 2, 3};
      for (int i = 0; i < 10; i++)
        memcpy(indices + i * width, &values[i], width);
      glBindBuffer(GL_COPY_READ_BUFFER, buffers[1]);
      glBufferData(GL_COPY_READ_BUFFER, width * 10, indices, GL_STREAM_DRAW);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, width * 10, NULL, GL_STREAM_DRAW);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ELEMENT_ARRAY_BUFFER, 0, 0, width * 10);
      const void *offsets[4] = {(void *)(uintptr_t)width, NULL,
                               (void *)(uintptr_t)(4 * width), (void *)(uintptr_t)(7 * width)};
      GLint bases[4] = {3, 0, -2, 8};
      glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts,
          width == 1 ? GL_UNSIGNED_BYTE : width == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
          offsets, 4, bases);
    }
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int bad = 0;
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) {
        unsigned char expected[4] = {0, 0, 0, 255};
        if (phase != 4 || x < 5)
          expected[x < 5 ? 0 : x < 10 ? 1 : 2] = 255;
        bad += memcmp(pixels + (y * 16 + x) * 4, expected, 4) != 0;
      }
    char name[64];
    snprintf(name, sizeof(name), "multidraw-%d.rgba", phase);
    FILE *f = fopen(name, "wb");
    if (!f) return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), f);
    int closed = fclose(f);
    GLenum error = glGetError();
    int failed = bad || error || written != sizeof(pixels) || closed;
    printf("MULTIDRAW_VERTEX phase=%d %s bad_pixels=%d error=0x%x\n", phase,
           failed ? "FAIL" : "PASS", bad, error);
    failures += failed;
  }
  glDeleteBuffers(3, buffers);
  glBindVertexArray(0);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++) glDeleteShader(shaders[i]);
  return failures ? 2 : 0;
}
