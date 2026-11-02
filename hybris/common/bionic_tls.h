#ifndef HYBRIS_BIONIC_TLS_H
#define HYBRIS_BIONIC_TLS_H

#include <stddef.h>

/* Existing linker/patcher ABI: these callbacks retain default visibility. */
__attribute__((visibility("default")))
void *_hybris_hook___get_tls_hooks(void);
__attribute__((visibility("default")))
void _hybris_init_static_tls_for_thread(size_t static_offset, const void *init_ptr,
                                      size_t init_size, size_t segment_size);

/* Initial-exec slot used by MRS thunks; its value is a simulated bionic TP. */
__attribute__((visibility("hidden")))
void *hybris_bionic_tls_pointer_slot(void);

#endif
