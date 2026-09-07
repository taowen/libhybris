/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib-xcb.h>
#include "render.h"
#include <xcb/xcb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int checked(xcb_connection_t *connection, xcb_void_cookie_t cookie, const char *operation) {
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (!error) return 0;
    printf("X11_ERROR operation=%s code=%u major=%u minor=%u\n", operation,
           error->error_code, error->major_code, error->minor_code);
    free(error); return -1;
}
static int control(xcb_connection_t *c, xcb_screen_t *screen, xcb_window_t window) {
    /* This verifies X11 transport only. It must never count as GPU WSI PASS. */
    const uint32_t colors[] = {0x00ff00, 0xff0000};
    const xcb_setup_t *setup = xcb_get_setup(c);
    unsigned bpp = 0, pad = 0;
    for (xcb_format_iterator_t i = xcb_setup_pixmap_formats_iterator(setup); i.rem; xcb_format_next(&i))
        if (i.data->depth == screen->root_depth) { bpp = i.data->bits_per_pixel; pad = i.data->scanline_pad; }
    if (bpp != 32 || pad != 32 || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST) return 3;
    for (unsigned frame = 0; frame < 2; ++frame) {
        if (checked(c, xcb_change_window_attributes_checked(c, window, XCB_CW_BACK_PIXEL, &colors[frame]), "background") ||
            checked(c, xcb_clear_area_checked(c, 0, window, 0, 0, 320, 240), "clear")) return 2;
        xcb_generic_error_t *error = NULL;
        xcb_get_image_reply_t *reply = xcb_get_image_reply(c,
            xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 0, 0, 320, 240, UINT32_MAX), &error);
        if (!reply || error) { free(reply); free(error); return 2; }
        int ok = xcb_get_image_data_length(reply) == 320 * 240 * 4;
        const unsigned char *bytes = xcb_get_image_data(reply);
        for (unsigned i = 0; ok && i < 320 * 240; ++i) {
            uint32_t pixel; memcpy(&pixel, bytes + i * 4, sizeof(pixel));
            ok = (pixel & 0xffffff) == colors[frame];
        }
        free(reply);
        if (!ok) return 2;
        printf("X11_CONTROL frame=%u pixels=76800 exact=1\n", frame);
    }
    return 0;
}
static int present(xcb_connection_t *connection, xcb_screen_t *screen, xcb_window_t window, Display *display) {
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) { printf("X11_VULKAN loader=%s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr gip = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    PFN_vkEnumerateInstanceExtensionProperties enumerate = gip ?
        (PFN_vkEnumerateInstanceExtensionProperties)gip(NULL, "vkEnumerateInstanceExtensionProperties") : NULL;
    uint32_t count = 0;
    if (!enumerate || enumerate(NULL, &count, NULL) != VK_SUCCESS) { dlclose(library); return 2; }
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (count && !extensions) { dlclose(library); return 2; }
    VkResult result = enumerate(NULL, &count, extensions);
    int surface = 0, xcb = 0;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; ++i) {
        surface |= !strcmp(extensions[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
        xcb |= !strcmp(extensions[i].extensionName, display ? VK_KHR_XLIB_SURFACE_EXTENSION_NAME : VK_KHR_XCB_SURFACE_EXTENSION_NAME);
    }
    free(extensions);
    printf("X11_VULKAN surface_extension=%d xcb_extension=%d\n", surface, xcb);
    int code = result != VK_SUCCESS ? 2 : !surface || !xcb ? 3 : x11_render(gip, connection, window, screen->root_visual, display);
    dlclose(library);
    return code;
}
static int version(void) {
    void *library = dlopen("libhybris-vulkan-icd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr resolver = dlsym(library, "vk_icdGetInstanceProcAddr");
    PFN_vkEnumerateInstanceVersion query = resolver
        ? (PFN_vkEnumerateInstanceVersion)resolver(NULL, "vkEnumerateInstanceVersion") : NULL;
    uint32_t value = 0;
    if (!query || query(&value) != VK_SUCCESS) return 2;
    printf("X11_ICD_VERSION %u.%u.%u\n", VK_VERSION_MAJOR(value), VK_VERSION_MINOR(value), VK_VERSION_PATCH(value));
    dlclose(library); return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "version")) return version();
    if (argc != 2 || (strcmp(argv[1], "control") && strcmp(argv[1], "present") && strcmp(argv[1], "missing-protocol") && strcmp(argv[1], "acquire-timeout"))) return 2;
    setvbuf(stdout, NULL, _IONBF, 0); alarm(15);
    if (!strcmp(argv[1], "acquire-timeout")) setenv("HYBRIS_X11_ACQUIRE_TIMEOUT", "1", 1);
    if (!strcmp(argv[1], "missing-protocol")) setenv("HYBRIS_X11_EXPECT_MISSING", "1", 1);
    Display *display = NULL;
    xcb_connection_t *c;
    if (getenv("HYBRIS_X11_XLIB")) {
        display = XOpenDisplay(NULL); if (!display) return 2;
        XSetEventQueueOwner(display, XCBOwnsEventQueue);
        c = XGetXCBConnection(display);
    } else {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        strcpy(address.sun_path, "x11.sock");
        if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address))) return 2;
        c = xcb_connect_to_fd(fd, NULL);
    }
    if (!c || xcb_connection_has_error(c)) { if (c) xcb_disconnect(c); return 2; }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    if (!screen) { xcb_disconnect(c); return 2; }
    const char *extension = "TAWC-DRI";
    xcb_query_extension_reply_t *query = xcb_query_extension_reply(c,
        xcb_query_extension(c, strlen(extension), extension), NULL);
    printf("X11_SERVER vendor=%.*s tawc_dri=%d\n", xcb_setup_vendor_length(xcb_get_setup(c)),
        xcb_setup_vendor(xcb_get_setup(c)), query ? query->present : 0);
    free(query);
    xcb_window_t window = xcb_generate_id(c);
    uint32_t values[] = {0, 1};
    int code = 2;
    if (checked(c, xcb_create_window_checked(c, screen->root_depth, window, screen->root,
            0, 0, 320, 240, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values), "create")) goto done;
    if (checked(c, xcb_map_window_checked(c, window), "map")) goto destroy;
    printf("X11_WINDOW id=%u size=320x240\n", window);
    code = !strcmp(argv[1], "control") ? control(c, screen, window) : present(c, screen, window, display);
destroy:
    if (checked(c, xcb_destroy_window_checked(c, window), "destroy")) code = 2;
done:
    if (display) XCloseDisplay(display); else xcb_disconnect(c);
    printf("X11_RESULT case=%s status=%s\n", argv[1], code == 0 ? "PASS" : code == 3 ? "UNSUPPORTED" : "FAIL");
    return code;
}
