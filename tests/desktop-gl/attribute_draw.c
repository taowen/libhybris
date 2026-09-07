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
  G(PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC,
    glDrawElementsInstancedBaseVertexBaseInstance);
  G(PFNGLDRAWARRAYSINDIRECTPROC, glDrawArraysIndirect);
  G(PFNGLMULTIDRAWARRAYSINDIRECTPROC, glMultiDrawArraysIndirect);
  G(PFNGLMULTIDRAWELEMENTSINDIRECTPROC, glMultiDrawElementsIndirect);
  G(PFNGLMULTIDRAWARRAYSINDIRECTCOUNTARBPROC,
    glMultiDrawArraysIndirectCountARB);
  G(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData);
  G(PFNGLCLEARBUFFERSUBDATAPROC, glClearBufferSubData);
  G(PFNGLGENTEXTURESPROC, glGenTextures);
  G(PFNGLACTIVETEXTUREPROC, glActiveTexture);
  G(PFNGLBINDTEXTUREPROC, glBindTexture);
  G(PFNGLTEXIMAGE2DPROC, glTexImage2D);
  G(PFNGLTEXPARAMETERIPROC, glTexParameteri);
  G(PFNGLTEXBUFFERPROC, glTexBuffer);
  G(PFNGLDELETETEXTURESPROC, glDeleteTextures);
  G(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute);
  G(PFNGLMEMORYBARRIERPROC, glMemoryBarrier);
  G(PFNGLBINDBUFFERBASEPROC, glBindBufferBase);
  G(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData);
  const char *sources[] = {
      "#version 430 core\n#extension GL_ARB_shader_draw_parameters : require\n"
      "layout(location=0) in vec2 position;"
      "layout(location=1) in vec4 tint;layout(location=2) in ivec2 numbers;"
      "layout(location=3) in vec4 halves;uniform int divisor;uniform int "
      "expected_base;uniform int expected_vertex_base;"
      "layout(binding=3) uniform sampler2D sampled;"
      "layout(binding=7) uniform isamplerBuffer table;"
      "flat out int instance;flat out vec4 color;void main(){"
      "instance=gl_InstanceID+min(int(gl_DrawIDARB),1)*2;int "
      "n=expected_base+gl_InstanceID/divisor;"
      "bool "
      "valid=gl_DrawIDARB<=1&&gl_BaseInstanceARB==expected_base&&gl_"
      "BaseVertexARB==expected_vertex_base&&all(equal(numbers,ivec2(-123,321)))"
      "&&"
      "all(equal(halves,vec4(.5,-2,0,1)))&&"
      "all(lessThan(abs(texture(sampled,vec2(.5))-vec4(.2,.4,.6,1)),vec4(."
      "00001)))&&"
      "all(lessThan(abs(textureLod(sampled,vec2(.5),1)-vec4(.8,.6,.4,1)),vec4(."
      "00001)))&&"
      "texelFetch(table,n).r==100+n&&"
      "all(lessThan(abs(tint-vec4(float(n)/255.,.2,.4,1)),vec4(.00001)));"
      "vec2 pos=position;pos.x=pos.x*.25+float(instance)*.5-.75;"
      "gl_Position=vec4(pos,0,1);color=valid?"
      "vec4(float(instance&1),float((instance>>1)&1),0,1):vec4(1,0,1,1);}",
      "#version 430 core\nflat in int instance;flat in vec4 color;out vec4 "
      "result;"
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
  const char *compute_source =
      "#version 430 core\nlayout(local_size_x=1) in;"
      "layout(binding=11) uniform sampler2D sample_after_draw;"
      "layout(std430,binding=0) buffer Output {uvec4 payload;};"
      "void "
      "main(){payload=uvec4(round(texelFetch(sample_after_draw,ivec2(0),0)*255."
      "));}";
  GLuint compute_shader = glCreateShader(GL_COMPUTE_SHADER);
  GLuint compute_program = glCreateProgram();
  glShaderSource(compute_shader, 1, &compute_source, NULL);
  glCompileShader(compute_shader);
  glGetShaderiv(compute_shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
    return 2;
  glAttachShader(compute_program, compute_shader);
  glLinkProgram(compute_program);
  glGetProgramiv(compute_program, GL_LINK_STATUS, &ok);
  if (!ok)
    return 2;
  GLuint textures[3], texture_buffer, compute_buffer;
  glGenTextures(3, textures);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, textures[2]);
  const unsigned char compute_pixel[4] = {17, 34, 51, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               compute_pixel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenBuffers(1, &compute_buffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, compute_buffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, compute_buffer);
  glUseProgram(compute_program);
  glDispatchCompute(1, 1, 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  glUseProgram(program);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, textures[0]);
  const unsigned char level0[16] = {51, 102, 153, 255, 51, 102, 153, 255,
                                    51, 102, 153, 255, 51, 102, 153, 255};
  const unsigned char level1[4] = {204, 153, 102, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               level0);
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               level1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenBuffers(1, &texture_buffer);
  glBindBuffer(GL_TEXTURE_BUFFER, texture_buffer);
  int32_t table[9];
  for (int i = 0; i < 9; i++)
    table[i] = 100 + i;
  glBufferData(GL_TEXTURE_BUFFER, sizeof(table), table, GL_STATIC_DRAW);
  glActiveTexture(GL_TEXTURE7);
  glBindTexture(GL_TEXTURE_BUFFER, textures[1]);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_R32I, texture_buffer);
  GLuint vao, buffers[10];
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(10, buffers);
  /* Every attribute uses distinct data with padding. Vertex records 0..6 and
   * instance records carry distinct indices, so a misplaced base is visible. */
  float positions[10][4] = {{0}};
  const float triangle[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
  for (int i = 0; i < 3; i++)
    memcpy(&positions[7 + i][1], triangle[i], sizeof(triangle[i]));
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)4);
  unsigned char tints[9][8] = {{0}};
  for (int i = 0; i < 9; i++) {
    tints[i][4] = i;
    tints[i][5] = 51;
    tints[i][6] = 102;
    tints[i][7] = 255;
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(tints), tints, GL_STATIC_DRAW);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8, (void *)4);
  int16_t integers[10][4] = {{0}};
  uint16_t halves[10][4] = {{0}};
  for (int i = 7; i < 10; i++) {
    integers[i][2] = -123;
    integers[i][3] = 321;
    halves[i][2] = 0x3800;
    halves[i][3] = 0xc000;
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(integers), integers, GL_STATIC_DRAW);
  glVertexAttribIPointer(2, 2, GL_SHORT, 8, (void *)4);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(halves), halves, GL_STATIC_DRAW);
  glVertexAttribPointer(3, 2, GL_HALF_FLOAT, GL_FALSE, 8, (void *)4);
  for (int i = 0; i < 4; i++)
    glEnableVertexAttribArray(i);
  const uint16_t indices[] = {0xbeef, 0, 1, 2, 0xbeef};
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[4]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
               GL_STATIC_DRAW);
  /* Indirect commands have a 16-byte prefix and 32-byte stride. The final
   * poison command must be ignored when the count buffer exceeds maxdrawcount.
   */
  glBindBuffer(GL_PARAMETER_BUFFER_ARB, buffers[7]);
  glBufferData(GL_PARAMETER_BUFFER_ARB, 4, NULL, GL_DYNAMIC_DRAW);
  uint32_t count = 3;
  glClearBufferSubData(GL_PARAMETER_BUFFER_ARB, GL_R32UI, 0, 4, GL_RED_INTEGER,
                       GL_UNSIGNED_INT, &count);
  unsigned char shared[208] = {0}, triples[45] = {0};
  for (int i = 7; i < 10; i++) {
    memcpy(shared + i * 21 + 1, triangle[i - 7], 8);
    memcpy(shared + i * 21 + 9, &integers[i][2], 4);
    memcpy(shared + i * 21 + 13, &halves[i][2], 4);
  }
  for (int i = 0; i < 9; i++) {
    triples[i * 5 + 2] = i;
    triples[i * 5 + 3] = 51;
    triples[i * 5 + 4] = 102;
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffers[9]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(triples), triples, GL_STATIC_DRAW);
  int failures = 0;
  for (int phase = 0; phase < 11; phase++) {
    if (phase == 8 || phase == 9) {
      /* Three attributes share a VBO with odd stride and byte offsets. The
       * final half and RGB records end exactly at their resource boundaries. */
      glBindBuffer(GL_ARRAY_BUFFER, buffers[8]);
      glBufferData(GL_ARRAY_BUFFER, phase == 8 ? 208 : 206, shared,
                   GL_STATIC_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 21, (void *)1);
      glVertexAttribIPointer(2, 2, GL_SHORT, 21, (void *)9);
      glVertexAttribPointer(3, phase == 8 ? 3 : 2, GL_HALF_FLOAT, GL_FALSE, 21,
                            (void *)13);
      glBindBuffer(GL_ARRAY_BUFFER, buffers[9]);
      glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, 5, (void *)2);
    } else if (phase == 10) {
      glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)4);
      glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
      glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8, (void *)4);
      glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
      glVertexAttribIPointer(2, 2, GL_SHORT, 8, (void *)4);
      glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
      glVertexAttribPointer(3, 2, GL_HALF_FLOAT, GL_FALSE, 8, (void *)4);
    }
    int divisor = phase == 0 || phase == 8 ? 1 : 2;
    int base = phase == 2 ? 0 : 5;
    int indexed = phase == 3 || phase == 6;
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "expected_base"), base);
    glUniform1i(glGetUniformLocation(program, "expected_vertex_base"),
                indexed ? 7 : 0);
    glUniform1i(glGetUniformLocation(program, "divisor"), divisor);
    glVertexAttribDivisor(1, divisor);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, 16, 16);
    if (phase < 3 || phase >= 8) {
      glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 7, 3, 4, base);
    } else if (phase == 3) {
      glDrawElementsInstancedBaseVertexBaseInstance(
          GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (void *)2, 4, 7, base);
    } else {
      uint32_t commands[28];
      for (unsigned i = 0; i < 28; i++)
        commands[i] = 0xdeadbeef;
      for (int draw = 0; draw < 3; draw++) {
        uint32_t *cmd = commands + 4 + draw * 8;
        cmd[0] = 3;
        cmd[1] = phase == 4 ? 4 : 2;
        cmd[2] = indexed ? 1 : 7;
        cmd[3] = indexed ? 7 : (unsigned)base;
        if (indexed)
          cmd[4] = base;
        if (draw == 2)
          cmd[indexed ? 4 : 3] = 0; /* poison: wrong base */
      }
      glBindBuffer(GL_COPY_READ_BUFFER, buffers[5]);
      glBufferData(GL_COPY_READ_BUFFER, sizeof(commands), commands,
                   GL_STREAM_DRAW);
      glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[6]);
      glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(commands), NULL,
                   GL_STREAM_DRAW);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_DRAW_INDIRECT_BUFFER, 0, 0,
                          sizeof(commands));
      if (phase == 4)
        glDrawArraysIndirect(GL_TRIANGLES, (void *)16);
      if (phase == 5)
        glMultiDrawArraysIndirect(GL_TRIANGLES, (void *)16, 2, 32);
      if (phase == 6)
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (void *)16,
                                    2, 32);
      if (phase == 7)
        glMultiDrawArraysIndirectCountARB(GL_TRIANGLES, (void *)16, 0, 2, 32);
    }
    /* The application compute sampler occupies a slot above all VS samplers.
     * Switching back exercises restored views, samplers and trailing slots. */
    glUseProgram(compute_program);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);
    uint32_t payload[4] = {0};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, compute_buffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(payload), payload);
    const uint32_t wanted_payload[4] = {17, 34, 51, 255};
    int compute_bad = memcmp(payload, wanted_payload, sizeof(payload)) != 0;
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int bad = 0;
    for (int i = 0; i < 256; i++) {
      int instance = i % 16 / 4;
      unsigned char expected[4] = {255 * (instance & 1),
                                   255 * ((instance >> 1) & 1), 0, 255};
      if (memcmp(pixels + i * 4, expected, 4))
        bad++;
    }
    char name[64];
    snprintf(name, sizeof(name), "attributes-%d.rgba", phase);
    FILE *f = fopen(name, "wb");
    if (!f)
      return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), f);
    int closed = fclose(f);
    GLenum error = glGetError();
    int failed =
        bad || compute_bad || error || written != sizeof(pixels) || closed;
    printf("ATTRIBUTE_VERTEX phase=%d %s bad_pixels=%d error=0x%x\n", phase,
           failed ? "FAIL" : "PASS", bad, error);
    printf("ATTRIBUTE_COMPUTE_RESTORE phase=%d %s rgba=%u,%u,%u,%u\n", phase,
           compute_bad ? "FAIL" : "PASS", payload[0], payload[1], payload[2],
           payload[3]);
    failures += failed;
  }
  glDeleteProgram(compute_program);
  glDeleteShader(compute_shader);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
  glDeleteBuffers(1, &compute_buffer);
  glDeleteTextures(3, textures);
  glDeleteBuffers(1, &texture_buffer);
  glActiveTexture(GL_TEXTURE0);
  glDeleteBuffers(10, buffers);
  glDeleteVertexArrays(1, &vao);
  glBindVertexArray(0);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++)
    glDeleteShader(shaders[i]);
  return failures ? 2 : 0;
}
