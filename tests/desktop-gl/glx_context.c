#include <EGL/egl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static __typeof__(&glXGetProcAddressARB) lookup_glx;
static __eglMustCastToProperFunctionPointerType lookup(const char *name) {
  return (__eglMustCastToProperFunctionPointerType)
      lookup_glx((const GLubyte *)name);
}

/* Load the staged GL frontend explicitly; the same draw checks serve EGL/GLX. */
int glx_probe(const char *profile, int (*draw)(PFNEGLGETPROCADDRESSPROC)) {
  void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    puts(dlerror());
    return 2;
  }
#define LOAD(name) __typeof__(&name) p_##name = dlsym(lib, #name); if (!p_##name) return 2
  LOAD(glXChooseFBConfig);
  LOAD(glXGetProcAddressARB);
  LOAD(glXCreatePbuffer);
  LOAD(glXMakeContextCurrent);
  LOAD(glXDestroyPbuffer);
  LOAD(glXDestroyContext);
  lookup_glx = p_glXGetProcAddressARB;
  Display *d = XOpenDisplay(NULL);
  if (!d) { puts("GLX_DISPLAY_FAIL"); return 2; }
  int attrs[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
                GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8,
                GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None};
  int count = 0, result = 2;
  GLXFBConfig *configs = p_glXChooseFBConfig(d, DefaultScreen(d), attrs, &count);
  if (!configs || !count) { XFree(configs); XCloseDisplay(d); return 2; }
  PFNGLXCREATECONTEXTATTRIBSARBPROC create = (PFNGLXCREATECONTEXTATTRIBSARBPROC)
      p_glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
  int ca[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB,
              !strcmp(profile, "core33") ? 3 : 2, GLX_CONTEXT_PROFILE_MASK_ARB,
              !strcmp(profile, "compat32") ? GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
                                           : GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
  GLXContext c = create ? create(d, configs[0], NULL, True, ca) : NULL;
  int pa[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None};
  GLXPbuffer buffer = c ? p_glXCreatePbuffer(d, configs[0], pa) : 0;
  if (buffer && p_glXMakeContextCurrent(d, buffer, buffer, c)) {
    puts("GLX_PBUFFER_CURRENT");
    result = draw(lookup);
    p_glXMakeContextCurrent(d, None, None, NULL);
  }
  if (buffer) p_glXDestroyPbuffer(d, buffer);
  if (c) p_glXDestroyContext(d, c);
  XFree(configs);
  XCloseDisplay(d);
  dlclose(lib);
  return result;
}
