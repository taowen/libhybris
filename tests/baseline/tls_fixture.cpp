// Bionic-built DSO: compiler-generated thread_local initialization/destruction.
struct Local {
    int value = 73;
    void (*callback)(void *, int) = nullptr;
    void *opaque = nullptr;
    ~Local() { if (callback) callback(opaque, value); }
};
static thread_local Local local;
extern "C" int tls_fixture_touch(void (*callback)(void *, int), void *opaque, int value) {
    int initial = local.value;
    local.callback = callback;
    local.opaque = opaque;
    local.value = value;
    return initial;
}
