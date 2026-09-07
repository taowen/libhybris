#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ordinary application VS: no compute source and no vertex SSBO declarations.
 * The optional Zink execution mode must convert this shader itself. */
int attribute_draw(PFNEGLGETPROCADDRESSPROC lookup) {
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
  G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
  G(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer);
  G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
  G(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor);
  const char *sources[] = {
      "#version 430 core\nlayout(location=0) in vec2 position;"
      "layout(location=1) in vec4 tint;layout(location=2) in ivec2 numbers;"
      "layout(location=3) in vec4 halves;uniform int divisor;"
      "flat out int instance;flat out vec4 color;void main(){"
      "instance=gl_InstanceID;int n=5+instance/divisor;"
      "bool valid=all(equal(numbers,ivec2(-123,321)))&&"
      "all(equal(halves,vec4(.5,-2,0,1)))&&"
      "all(lessThan(abs(tint-vec4(float(n)/255.,.2,.4,1)),vec4(.00001)));"
      "vec2 pos=position;pos.x=pos.x*.25+float(instance)*.5-.75;"
      "gl_Position=vec4(pos,0,1);color=valid?"
      "vec4(float(instance&1),float((instance>>1)&1),0,1):vec4(1,0,1,1);}",
      "#version 430 core\nflat in int instance;flat in vec4 color;out vec4 result;"
      "void main(){if(int(gl_FragCoord.x)/4!=instance)discard;result=color;}"};
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
  GLuint vao, buffers[4];
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(4, buffers);
  /* Every attribute uses distinct data with padding. Vertex records 0..6 and
   * instance records 0..4 deliberately contain the wrong values. */
  float positions[10][4] = {{0}};
  const float triangle[3][2] = {{-1,-1},{3,-1},{-1,3}};
  for (int i = 0; i < 3; i++)
    memcpy(&positions[7+i][1], triangle[i], sizeof(triangle[i]));
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)4);
  unsigned char tints[9][8] = {{0}};
  for (int i = 5; i < 9; i++) {
    tints[i][4] = i; tints[i][5] = 51; tints[i][6] = 102; tints[i][7] = 255;
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(tints), tints, GL_STATIC_DRAW);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8, (void *)4);
  int16_t integers[10][4] = {{0}};
  uint16_t halves[10][4] = {{0}};
  for (int i = 7; i < 10; i++) {
    integers[i][2] = -123; integers[i][3] = 321;
    halves[i][2] = 0x3800; halves[i][3] = 0xc000;
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(integers), integers, GL_STATIC_DRAW);
  glVertexAttribIPointer(2, 2, GL_SHORT, 8, (void *)4);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(halves), halves, GL_STATIC_DRAW);
  glVertexAttribPointer(3, 2, GL_HALF_FLOAT, GL_FALSE, 8, (void *)4);
  for (int i = 0; i < 4; i++) glEnableVertexAttribArray(i);
  int failures = 0;
  for (int divisor = 1; divisor <= 2; divisor++) {
    glUniform1i(glGetUniformLocation(program, "divisor"), divisor);
    glVertexAttribDivisor(1, divisor);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, 16, 16);
    glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 7, 3, 4, 5);
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int bad = 0;
    for (int i = 0; i < 256; i++) {
      int instance = i % 16 / 4;
      unsigned char expected[4] = {255 * (instance & 1), 255 * ((instance >> 1) & 1), 0, 255};
      if (memcmp(pixels + i * 4, expected, 4)) bad++;
    }
    char name[64];
    snprintf(name, sizeof(name), "attributes-%d.rgba", divisor);
    FILE *f = fopen(name, "wb");
    if (!f) return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), f);
    int closed = fclose(f);
    GLenum error = glGetError();
    int failed = bad || error || written != sizeof(pixels) || closed;
    printf("ATTRIBUTE_VERTEX divisor=%d %s bad_pixels=%d error=0x%x\n", divisor,
           failed ? "FAIL" : "PASS", bad, error);
    failures += failed;
  }
  glDeleteBuffers(4, buffers);
  glDeleteVertexArrays(1, &vao);
  glBindVertexArray(0);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++) glDeleteShader(shaders[i]);
  return failures ? 2 : 0;
}
