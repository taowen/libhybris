#include "probe.h"

struct context_pair {
  void *e, *g;
  EGLDisplay display;
  EGLContext contexts[2];
  EGLSurface surfaces[2];
  GLuint buffers[2];
  int result;
};

static int check_contexts(struct context_pair *pair, int initialize) {
  void *e = pair->e, *g = pair->g;
  E(eglMakeCurrent);
  E(eglGetCurrentContext);
  E(eglGetCurrentSurface);
  E(eglGetError);
  G(glGenBuffers);
  G(glBindBuffer);
  G(glBufferData);
  G(glGetBufferParameteriv);
  G(glGetIntegerv);
  G(glIsBuffer);
  G(glClearColor);
  G(glClear);
  G(glReadPixels);
  G(glGetError);
  for (int pass = 0; pass < (initialize ? 1 : 8); ++pass) {
    for (int i = 0; i < 2; ++i) {
      if (!p_eglMakeCurrent(pair->display, pair->surfaces[i], pair->surfaces[i], pair->contexts[i])) {
        printf("CONTEXT_SWITCH failed error=%x\n", p_eglGetError()); return 2;
      }
      if (p_eglGetCurrentContext() != pair->contexts[i] ||
          p_eglGetCurrentSurface(EGL_DRAW) != pair->surfaces[i] ||
          p_eglGetCurrentSurface(EGL_READ) != pair->surfaces[i]) return 2;
      if (initialize) {
        /* The second context has no share context and must not see the first buffer. */
        if (i && p_glIsBuffer(pair->buffers[0])) { printf("BUFFER isolation failed\n"); return 2; }
        p_glGenBuffers(1, &pair->buffers[i]);
        p_glBindBuffer(GL_ARRAY_BUFFER, pair->buffers[i]);
        p_glBufferData(GL_ARRAY_BUFFER, 16 + i * 16, NULL, GL_STATIC_DRAW);
        p_glClearColor(i ? 0 : 1, i ? 1 : 0, 0, 1);
      }
      GLint binding = 0, size = 0;
      p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
      p_glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
      if ((GLuint)binding != pair->buffers[i] || size != 16 + i * 16) {
        printf("BUFFER context=%d binding=%d size=%d\n", i, binding, size); return 2;
      }
      /* No state reset here: clear color must survive switches and migration. */
      p_glClear(GL_COLOR_BUFFER_BIT);
      unsigned char pixel[4] = {0};
      p_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      if (pixel[0] != (i ? 0 : 255) || pixel[1] != (i ? 255 : 0) ||
          pixel[2] != 0 || pixel[3] != 255 || p_glGetError() != GL_NO_ERROR) {
        printf("PIXEL context=%d rgba=%u,%u,%u,%u\n", i, pixel[0],pixel[1],pixel[2],pixel[3]); return 2;
      }
    }
  }
  if (!p_eglMakeCurrent(pair->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) return 2;
  return p_eglGetCurrentContext() == EGL_NO_CONTEXT ? 0 : 2;
}

static void *migrate_contexts(void *opaque) {
  struct context_pair *pair = opaque;
  void *e = pair->e;
  E(eglBindAPI);
  E(eglReleaseThread);
  pair->result = p_eglBindAPI(EGL_OPENGL_ES_API) ? check_contexts(pair, 0) : 2;
  if (!p_eglReleaseThread()) pair->result = 2;
  return NULL;
}

static int check_shared(struct context_pair *pair, int cycle) {
  void *e = pair->e, *g = pair->g;
  E(eglMakeCurrent);
  E(eglDestroyContext);
  G(glGenBuffers); G(glBindBuffer); G(glBufferData); G(glGetBufferParameteriv);
  G(glIsBuffer); G(glDeleteBuffers); G(glGetIntegerv);
  G(glGenTextures); G(glBindTexture); G(glTexImage2D); G(glTexSubImage2D);
  G(glTexParameteri); G(glIsTexture); G(glDeleteTextures);
  G(glGenFramebuffers); G(glBindFramebuffer); G(glFramebufferTexture2D);
  G(glCheckFramebufferStatus); G(glDeleteFramebuffers); G(glReadPixels);
  G(glFinish); G(glGetError);
  GLuint buffer = 0, texture = 0, fbo[2] = {0};
  const unsigned char red[] = {255,0,0,255}, green[] = {0,255,0,255};
  for (int step = 0; step < 4; ++step) {
    int i = step == 0 || step == 2 ? 0 : 1;
    if (!p_eglMakeCurrent(pair->display, pair->surfaces[i], pair->surfaces[i], pair->contexts[i])) return 2;
    if (step == 0) {
      p_glGenBuffers(1, &buffer);
      p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
      p_glBufferData(GL_ARRAY_BUFFER, 16, NULL, GL_STATIC_DRAW);
      p_glGenTextures(1, &texture);
      p_glBindTexture(GL_TEXTURE_2D, texture);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    }
    if (!p_glIsBuffer(buffer) || !p_glIsTexture(texture)) return 2;
    if (step == 1) {
      GLint binding = -1;
      p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
      if (binding != 0) return 2; /* Objects share; bindings are context-local. */
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
    GLint size = 0;
    p_glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    if (size != (step < 2 ? 16 : 32)) return 2;
    p_glBindTexture(GL_TEXTURE_2D, texture);
    if (!fbo[i]) p_glGenFramebuffers(1, &fbo[i]);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
    /* Reattach after each context handoff to make shared changes observable. */
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 2;
    unsigned char pixel[4] = {0};
    p_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (memcmp(pixel, step < 2 ? red : green, 4) || p_glGetError() != GL_NO_ERROR) {
      printf("EGL_SHARED step=%d pixel=%u,%u,%u,%u\n", step,pixel[0],pixel[1],pixel[2],pixel[3]); return 2;
    }
    if (step == 1) {
      p_glBufferData(GL_ARRAY_BUFFER, 32, NULL, GL_STATIC_DRAW);
      p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
    }
    if (step == 2 || step == 3) p_glDeleteFramebuffers(1, &fbo[i]);
    if (step == 3) { p_glDeleteTextures(1, &texture); p_glDeleteBuffers(1, &buffer); }
    /* Explicit completion before transferring ownership to the other context. */
    p_glFinish();
    if (p_glGetError() != GL_NO_ERROR ||
        !p_eglMakeCurrent(pair->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) return 2;
    if (step == 2) {
      if (!p_eglDestroyContext(pair->display, pair->contexts[0])) return 2;
      pair->contexts[0] = EGL_NO_CONTEXT;
    }
  }
  printf("EGL_SHARED cycle=%d buffer-size/texture-pixels and creator destruction PASS\n", cycle);
  return 0;
}

int egl_lifecycle_probe(void) {
  struct context_pair pair = {0};
  void *e = pair.e = dlopen(getenv("PROBE_EGL") ?: "libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  void *g = pair.g = dlopen(getenv("PROBE_GLES") ?: "libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!e || !g) { printf("EGL/GLES load failed: %s\n", dlerror()); return 2; }
  E(eglGetDisplay);
  E(eglInitialize);
  E(eglChooseConfig);
  E(eglBindAPI);
  E(eglCreateContext);
  E(eglCreatePbufferSurface);
  E(eglMakeCurrent);
  E(eglDestroyContext);
  E(eglDestroySurface);
  E(eglTerminate);
  E(eglReleaseThread);
  G(glDeleteBuffers);
  pair.display = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!p_eglInitialize(pair.display, NULL, NULL) || !p_eglBindAPI(EGL_OPENGL_ES_API)) return 2;
  EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig config;
  EGLint count;
  if (!p_eglChooseConfig(pair.display, attrs, &config, 1, &count)) return 2;
  if (!count) { p_eglTerminate(pair.display); return 3; }
  EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLint pa[] = {EGL_WIDTH,4,EGL_HEIGHT,4,EGL_NONE};
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (int i = 0; i < 2; ++i) {
      pair.contexts[i] = p_eglCreateContext(pair.display, config, EGL_NO_CONTEXT, ca);
      pair.surfaces[i] = p_eglCreatePbufferSurface(pair.display, config, pa);
      if (pair.contexts[i] == EGL_NO_CONTEXT || pair.surfaces[i] == EGL_NO_SURFACE) return 2;
    }
    if (check_contexts(&pair, 1) || check_contexts(&pair, 0)) return 2;
    pthread_t thread;
    if (pthread_create(&thread, NULL, migrate_contexts, &pair)) return 2;
    if (pthread_join(thread, NULL) || pair.result || check_contexts(&pair, 0)) return 2;
    for (int i = 0; i < 2; ++i) {
      if (!p_eglMakeCurrent(pair.display, pair.surfaces[i], pair.surfaces[i], pair.contexts[i])) return 2;
      p_glDeleteBuffers(1, &pair.buffers[i]);
      if (!p_eglMakeCurrent(pair.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ||
          !p_eglDestroyContext(pair.display, pair.contexts[i]) ||
          !p_eglDestroySurface(pair.display, pair.surfaces[i])) return 2;
    }
    printf("EGL_LIFECYCLE cycle=%d isolated buffers/state/pixels and thread migration PASS\n", cycle);
    for (int i = 0; i < 2; ++i) {
      pair.contexts[i] = p_eglCreateContext(pair.display, config, i ? pair.contexts[0] : EGL_NO_CONTEXT, ca);
      pair.surfaces[i] = p_eglCreatePbufferSurface(pair.display, config, pa);
      if (pair.contexts[i] == EGL_NO_CONTEXT || pair.surfaces[i] == EGL_NO_SURFACE) return 2;
    }
    if (check_shared(&pair, cycle)) return 2;
    if (!p_eglDestroyContext(pair.display, pair.contexts[1])) return 2;
    for (int i = 0; i < 2; ++i)
      if (!p_eglDestroySurface(pair.display, pair.surfaces[i])) return 2;
  }
  if (!p_eglTerminate(pair.display) || !p_eglReleaseThread()) return 2;
  printf("EGL_LIFECYCLE PASS\n");
  return 0;
}
