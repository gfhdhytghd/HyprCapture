// Isolated-compositor test driver. It only connects to the explicitly selected
// WAYLAND_DISPLAY, and injects one real protocol axis event, never uinput.
#include "scroll-virtual-pointer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *r, uint32_t name,
                   const char *interface, uint32_t version) {
  if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
    manager = wl_registry_bind(r, name,
                               &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void removed(void *data, struct wl_registry *r, uint32_t name) {}
int main(int argc, char **argv) {
  if (argc != 6 || !getenv("HYPRCAPTURE_SCROLL_LIVE_TEST"))
    return 2;
  struct wl_display *display = wl_display_connect(NULL);
  if (!display)
    return 2;
  struct wl_registry *registry = wl_display_get_registry(display);
  const struct wl_registry_listener listener = {global, removed};
  wl_registry_add_listener(registry, &listener, NULL);
  wl_display_roundtrip(display);
  if (!manager)
    return 3;
  struct zwlr_virtual_pointer_v1 *pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  uint32_t ms = time.tv_sec * 1000 + time.tv_nsec / 1000000;
  zwlr_virtual_pointer_v1_motion_absolute(
      pointer, ms, atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
  zwlr_virtual_pointer_v1_frame(pointer);
  wl_display_roundtrip(display);
  int delta = atoi(argv[5]);
  const int finger = getenv("HYPRCAPTURE_SCROLL_NATIVE_FINGER") != NULL;
  zwlr_virtual_pointer_v1_axis_source(pointer, finger ? WL_POINTER_AXIS_SOURCE_FINGER : WL_POINTER_AXIS_SOURCE_WHEEL);
  if (finger)
    zwlr_virtual_pointer_v1_axis(pointer, ms + 1, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(delta));
  else
    zwlr_virtual_pointer_v1_axis_discrete(pointer, ms + 1, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(delta), delta * 8 / 120);
  zwlr_virtual_pointer_v1_frame(pointer);
  wl_display_roundtrip(display);
  // Keep the source alive through initial-frame acknowledgement and replay.
  if (getenv("HYPRCAPTURE_SCROLL_POINTER_MOTION")) {
    for (int i = 0; i < 150; ++i) {
      usleep(4000);
      clock_gettime(CLOCK_MONOTONIC, &time);
      ms = time.tv_sec * 1000 + time.tv_nsec / 1000000;
      zwlr_virtual_pointer_v1_motion_absolute(pointer, ms,
          atoi(argv[1]) + (i % 2 ? 2 : -2), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
      zwlr_virtual_pointer_v1_frame(pointer);
      wl_display_roundtrip(display);
    }
  } else usleep(500000);
  if (finger) {
    zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_FINGER);
    clock_gettime(CLOCK_MONOTONIC, &time);
    ms = time.tv_sec * 1000 + time.tv_nsec / 1000000;
    zwlr_virtual_pointer_v1_axis_stop(pointer, ms, WL_POINTER_AXIS_VERTICAL_SCROLL);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
  }
  zwlr_virtual_pointer_v1_destroy(pointer);
  wl_display_roundtrip(display);
  wl_display_disconnect(display);
  return 0;
}
