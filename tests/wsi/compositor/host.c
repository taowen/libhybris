#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdlib.h>

JNIEXPORT jint JNICALL Java_io_taowen_hybriswsitest_CompositorActivity_run(
        JNIEnv *env, jclass type, jobject surface, jint width, jint height,
        jstring runtime, jstring library) {
    (void)type;
    const char *path = (*env)->GetStringUTFChars(env, library, NULL);
    void *module = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    (*env)->ReleaseStringUTFChars(env, library, path);
    if (!module) { __android_log_print(ANDROID_LOG_ERROR, "HybrisWSITest", "%s", dlerror()); return -1; }
    int (*run)(ANativeWindow *, int, int, const char *) = dlsym(module, "anlabwc_run");
    if (!run) return -2;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) return -3;
    const char *directory = (*env)->GetStringUTFChars(env, runtime, NULL);
    setenv("TMPDIR", directory, 1);
    setenv("WLR_XWAYLAND", "/system/bin/false", 1);
    int result = run(window, width, height, directory);
    (*env)->ReleaseStringUTFChars(env, runtime, directory);
    ANativeWindow_release(window);
    return result;
}
