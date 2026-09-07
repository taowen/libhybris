/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_ICD_NATIVE_WINDOW_H
#define HYBRIS_ICD_NATIVE_WINDOW_H
#include <stdint.h>
struct ANativeWindowBuffer;
struct wl_display;
struct wl_surface;
struct xcb_connection_t;
struct hybris_icd_window;
struct hybris_icd_window_ops {
    int (*configure)(struct hybris_icd_window *, unsigned, unsigned, int, unsigned, unsigned);
    int (*dequeue)(struct hybris_icd_window *, int64_t, struct ANativeWindowBuffer **, int *);
    int (*queue)(struct hybris_icd_window *, struct ANativeWindowBuffer *, int);
    int (*cancel)(struct hybris_icd_window *, struct ANativeWindowBuffer *, int);
    void (*disconnect)(struct hybris_icd_window *);
    void (*destroy)(struct hybris_icd_window *);
    /* Zero extent means application-selected (Wayland). */
    int (*extent)(struct hybris_icd_window *, uint32_t *, uint32_t *);
};
struct hybris_icd_window { const struct hybris_icd_window_ops *ops; };
#ifdef __cplusplus
extern "C" {
#endif
int hybris_icd_window_wayland(struct wl_display *, struct wl_surface *, struct hybris_icd_window **);
int hybris_icd_window_xcb(struct xcb_connection_t *, uint32_t, struct hybris_icd_window **);
int hybris_icd_xcb_supported(struct xcb_connection_t *, uint32_t visual);
#ifdef __cplusplus
}
#endif
#endif
