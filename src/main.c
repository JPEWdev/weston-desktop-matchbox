/*
 * Copyright 2025 Joshua Watt
 *
 * SPDX-License-Identifier: MT
 */
#define _GNU_SOURCE

#include <cairo.h>
#include <ctype.h>
#include <gio/gio.h>
#include <glob.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "viewporter.h"
#include "weston-desktop-shell.h"
#include "xdg-shell.h"

#define MENU_PADDING (10)

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_cursor_theme *cursor_theme;
static struct weston_desktop_shell *desktop_shell;
static struct xdg_wm_base *xdg_wm_base;
static struct wp_viewporter *viewporter;
static struct wl_surface *cursor_surface;
static struct wl_cursor *current_cursor;
static struct wl_list seat_list;
static GList *application_list;
static bool need_roundtrip = true;

struct background;
struct buffer;
struct seat;

struct output {
  struct wl_list link;
  struct wl_output *output;
  struct background *background;
  uint32_t width;
  uint32_t height;
};
static struct wl_list output_list;

struct desktop_surface {
  void (*configure)(void *, struct weston_desktop_shell *, uint32_t,
                    struct wl_surface *, int32_t, int32_t);
  char const *cursor;
  double cursor_x;
  double cursor_y;
  void (*on_cursor_motion)(void *, struct seat *);
  void (*on_cursor_enter)(void *, struct seat *);
  void (*on_cursor_leave)(void *, struct seat *);
  void (*on_pointer_button)(void *, struct seat *, uint32_t, uint32_t);
  void (*on_pointer_axis)(void *, struct seat *, uint32_t, double);
  void (*on_pointer_frame)(void *, struct seat *);
};

struct seat {
  struct wl_seat *seat;
  struct {
    struct wl_pointer *pointer;
    struct desktop_surface *surface;
  } pointer;
};

struct background {
  struct desktop_surface base;
  struct output *output;
  struct wl_surface *surface;
  // struct wp_viewport *viewport;
  struct wl_list buffers;
  uint32_t width;
  uint32_t height;
  double y_scroll;
  struct wl_callback *frame;
  bool needs_draw;
  cairo_font_extents_t extents;
};

struct buffer {
  struct wl_list link;
  struct wl_buffer *buffer;
  bool busy;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t format;
  size_t size;
  void *data;
};

static void draw_background(struct background *background);

static void launch_app(GAppInfo *app) {
  GError *error = NULL;
  if (!g_app_info_launch(app, NULL, NULL, &error)) {
    fprintf(stderr, "Unable to launch '%s': %s\n", g_app_info_get_name(app),
            error->message);
  }

  g_clear_error(&error);
}

// Buffer
static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
  struct buffer *buffer = data;
  buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release,
};

static struct buffer *create_buffer(uint32_t width, uint32_t height,
                                    uint32_t stride, uint32_t format) {
  int fd = memfd_create("buffer", MFD_CLOEXEC);
  if (fd < 0) {
    perror("Unable to create memfd");
    return NULL;
  }

  size_t size = height * stride;

  if (ftruncate(fd, size)) {
    perror("Unable to truncate memfd");
    close(fd);
    return NULL;
  }

  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    perror("Unable to map memfd");
    close(fd);
    return NULL;
  }

  struct buffer *buffer = calloc(1, sizeof(*buffer));

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  buffer->buffer =
      wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
  wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
  wl_shm_pool_destroy(pool);
  close(fd);

  buffer->width = width;
  buffer->height = height;
  buffer->stride = stride;
  buffer->format = format;
  buffer->size = size;
  buffer->data = data;

  return buffer;
}

static void free_buffer(struct buffer *buffer) {
  munmap(buffer->data, buffer->size);
  wl_buffer_destroy(buffer->buffer);
  free(buffer);
}

static void attach_buffer(struct wl_surface *surface, struct buffer *buffer,
                          int32_t x, int32_t y) {
  wl_surface_attach(surface, buffer->buffer, x, y);
  buffer->busy = true;
}

// Cursor
static void set_cursor(char const *name, struct seat *seat, uint32_t serial) {
  if (current_cursor && strcmp(current_cursor->name, name) == 0) {
    return;
  }
  struct wl_cursor *cursor = wl_cursor_theme_get_cursor(cursor_theme, name);
  if (!cursor) {
    return;
  }
  current_cursor = cursor;
  struct wl_buffer *buffer =
      wl_cursor_image_get_buffer(current_cursor->images[0]);
  wl_surface_attach(cursor_surface, buffer, 0, 0);
  wl_surface_commit(cursor_surface);
  wl_pointer_set_cursor(seat->pointer.pointer, serial, cursor_surface,
                        current_cursor->images[0]->hotspot_x,
                        current_cursor->images[0]->hotspot_y);
}

// Desktop Shell
static void desktop_shell_configure(
    void *data, struct weston_desktop_shell *weston_desktop_shell,
    uint32_t edges, struct wl_surface *surface, int32_t width, int32_t height) {

  struct desktop_surface *s = wl_surface_get_user_data(surface);
  s->configure(s, weston_desktop_shell, edges, surface, width, height);
}

static void desktop_shell_prepare_lock_surface(
    void *data, struct weston_desktop_shell *weston_desktop_shell) {
  // No lock screen implemented, unlock immediately
  weston_desktop_shell_unlock(desktop_shell);
}

static void
desktop_shell_grab_cursor(void *data,
                          struct weston_desktop_shell *weston_desktop_shell,
                          uint32_t cursor) {}

static const struct weston_desktop_shell_listener desktop_shell_listener = {
    desktop_shell_configure,
    desktop_shell_prepare_lock_surface,
    desktop_shell_grab_cursor,
};

// SHM
static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {}

static const struct wl_shm_listener shm_listener = {
    shm_format,
};

// Background surface
static void background_enter_output(void *data, struct wl_surface *wl_surface,
                                    struct wl_output *output) {}
static void background_leave_output(void *data, struct wl_surface *wl_surface,
                                    struct wl_output *output) {}

static const struct wl_surface_listener background_surface_listener = {
    background_enter_output,
    background_leave_output,
};

static void background_frame_done(void *data, struct wl_callback *wl_callback,
                                  uint32_t callback_data) {
  struct background *background = data;
  wl_callback_destroy(background->frame);
  background->frame = NULL;
  if (background->needs_draw) {
    draw_background(background);
  }
}

static const struct wl_callback_listener background_frame_listener = {
    background_frame_done,
};

static void draw_background(struct background *background) {
  if (background->frame) {
    // Can't draw right now. Flag as needing to redraw when the frame callback
    // finishes
    background->needs_draw = true;
    return;
  }
  background->needs_draw = false;

  struct buffer *buffer = NULL;
  struct buffer *tmp;
  uint32_t stride =
      cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, background->width);

  // Remove any unused buffers that are the wrong size
  wl_list_for_each_safe(buffer, tmp, &background->buffers, link) {
    if (!buffer->busy &&
        (buffer->width != background->width ||
         buffer->height != background->height || buffer->stride != stride)) {
      wl_list_remove(&buffer->link);
      free_buffer(buffer);
    }
  }

  bool found = false;
  wl_list_for_each_safe(buffer, tmp, &background->buffers, link) {
    if (!buffer->busy && buffer->width == background->width &&
        buffer->height == background->height && buffer->stride == stride) {
      found = true;
      break;
    }
  }

  if (!found) {
    buffer = create_buffer(background->width, background->height, stride,
                           WL_SHM_FORMAT_XRGB8888);
    wl_list_insert(&background->buffers, &buffer->link);
  }

  cairo_surface_t *surface = cairo_image_surface_create_for_data(
      buffer->data, CAIRO_FORMAT_RGB24, background->width, background->height,
      stride);
  cairo_t *cr = cairo_create(surface);

  cairo_rectangle(cr, 0, 0, background->width, background->height);
  cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
  cairo_fill(cr);

  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 20);

  cairo_font_extents(cr, &background->extents);

  double total_height =
      g_list_length(application_list) * background->extents.height;
  double max_y_scroll = total_height - (background->height - MENU_PADDING * 2);
  if (max_y_scroll > 0) {
    background->y_scroll = fmax(background->y_scroll, 0);
    background->y_scroll = fmin(background->y_scroll, max_y_scroll);
  } else {
    background->y_scroll = 0;
  }

  double y = MENU_PADDING - background->y_scroll;
  for (GList *cur = application_list; cur != NULL; cur = cur->next) {
    GAppInfo *app = cur->data;
    if (y + background->extents.height >= 0 && y <= background->height) {
      if (background->base.cursor_y > y &&
          background->base.cursor_y <= y + background->extents.height) {
        cairo_set_source_rgb(cr, 0, 1, 1);
      } else {
        cairo_set_source_rgb(cr, 0, 0, 0);
      }
      cairo_move_to(cr, MENU_PADDING, y + background->extents.ascent);
      cairo_show_text(cr, g_app_info_get_name(app));
    }
    y += background->extents.height;
  }

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  attach_buffer(background->surface, buffer, 0, 0);
  wl_surface_damage(background->surface, 0, 0, background->width,
                    background->height);
  background->frame = wl_surface_frame(background->surface);
  wl_callback_add_listener(background->frame, &background_frame_listener,
                           background);
  wl_surface_commit(background->surface);
}

static void background_configure(
    void *data, struct weston_desktop_shell *weston_desktop_shell,
    uint32_t edges, struct wl_surface *surface, int32_t width, int32_t height) {
  struct background *background = data;
  background->width = width;
  background->height = height;

  draw_background(background);
}

static void background_cursor_motion(void *data, struct seat *seat) {
  struct background *background = data;
  draw_background(background);
}

static void background_cursor_enter(void *data, struct seat *seat) {
  struct background *background = data;
  draw_background(background);
}

static void background_cursor_leave(void *data, struct seat *seat) {
  struct background *background = data;
  draw_background(background);
}

static void background_pointer_axis(void *data, struct seat *seat,
                                    uint32_t axis, double value) {
  struct background *background = data;
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    background->y_scroll += value;
    draw_background(background);
  }
}

static void background_pointer_button(void *data, struct seat *seat,
                                      uint32_t button, uint32_t state) {
  struct background *background = data;
  if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_RELEASED &&
      background->extents.height) {
    double menu_y =
        background->base.cursor_y + background->y_scroll - MENU_PADDING;
    if (menu_y >= 0) {
      guint menu_idx = menu_y / background->extents.height;
      GAppInfo *app = g_list_nth_data(application_list, menu_idx);
      if (app) {
        launch_app(app);
      }
    }
  }
}

// Output
static void output_geometry(void *data, struct wl_output *wl_output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  struct output *output = data;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    output->width = width;
    output->height = height;
  }
}

static void output_done(void *data, struct wl_output *wl_output) {
  struct output *output = data;
  if (!output->background) {
    struct background *b = calloc(1, sizeof(*output->background));
    wl_list_init(&b->buffers);
    output->background = b;
    b->output = output;

    b->surface = wl_compositor_create_surface(compositor);
    wl_surface_add_listener(b->surface, &background_surface_listener, b);
    b->base.configure = background_configure;
    b->base.cursor = "left_ptr";
    b->base.cursor_x = -1;
    b->base.cursor_y = -1;
    b->base.on_cursor_motion = background_cursor_motion;
    b->base.on_cursor_enter = background_cursor_enter;
    b->base.on_cursor_leave = background_cursor_leave;
    b->base.on_pointer_axis = background_pointer_axis;
    b->base.on_pointer_button = background_pointer_button;

    weston_desktop_shell_set_background(desktop_shell, output->output,
                                        b->surface);
  }
}

static void output_scale(void *data, struct wl_output *wl_output,
                         int32_t factor) {}

static void output_name(void *data, struct wl_output *wl_output,
                        const char *name) {}

static void output_description(void *data, struct wl_output *wl_output,
                               const char *description) {}

static const struct wl_output_listener output_listener = {
    output_geometry, output_mode, output_done,
    output_scale,    output_name, output_description,
};

// XDG WM base
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                             uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

// Pointer
static void pointer_enter(void *data, struct wl_pointer *wl_pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
  struct seat *seat = data;
  struct desktop_surface *s = wl_surface_get_user_data(surface);

  seat->pointer.surface = s;
  if (s->cursor) {
    set_cursor(s->cursor, seat, serial);
  }
  s->cursor_x = wl_fixed_to_double(surface_x);
  s->cursor_y = wl_fixed_to_double(surface_y);
  if (s->on_cursor_enter) {
    s->on_cursor_enter(s, seat);
  }
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer,
                          uint32_t serial, struct wl_surface *surface) {
  struct seat *seat = data;
  struct desktop_surface *s = wl_surface_get_user_data(surface);
  s->cursor_x = -1;
  s->cursor_y = -1;
  if (s->on_cursor_leave) {
    s->on_cursor_leave(s, seat);
  }
  seat->pointer.surface = NULL;
  current_cursor = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y) {
  struct seat *seat = data;
  struct desktop_surface *s = seat->pointer.surface;
  if (s) {
    s->cursor_x = wl_fixed_to_double(surface_x);
    s->cursor_y = wl_fixed_to_double(surface_y);
    if (s->on_cursor_motion) {
      s->on_cursor_motion(s, seat);
    }
  }
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
  struct seat *seat = data;
  struct desktop_surface *s = seat->pointer.surface;
  if (s && s->on_pointer_button) {
    s->on_pointer_button(s, seat, button, state);
  }
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
  struct seat *seat = data;
  struct desktop_surface *s = seat->pointer.surface;
  if (s && s->on_pointer_axis) {
    s->on_pointer_axis(s, seat, axis, wl_fixed_to_double(value));
  }
}

static void pointer_frame(void *data, struct wl_pointer *wl_pointer) {
  struct seat *seat = data;
  struct desktop_surface *s = seat->pointer.surface;
  if (s && s->on_pointer_frame) {
    s->on_pointer_frame(s, seat);
  }
}

static void pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                                uint32_t axis_source) {}

static void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                              uint32_t time, uint32_t axis) {}

static void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t axis, int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter,       pointer_leave,     pointer_motion,
    pointer_button,      pointer_axis,      pointer_frame,
    pointer_axis_source, pointer_axis_stop, pointer_axis_discrete,
};

// seat
static void seat_capabilities(void *data, struct wl_seat *wl_seat,
                              uint32_t capabilities) {
  struct seat *seat = data;
  if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
    seat->pointer.pointer = wl_seat_get_pointer(seat->seat);
    seat->pointer.surface = NULL;
    wl_pointer_add_listener(seat->pointer.pointer, &pointer_listener, seat);
  }
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name,
};

// Registry
static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, char const *interface,
                            uint32_t version) {
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                  MIN(version, 5));

    cursor_surface = wl_compositor_create_surface(compositor);

  } else if (strcmp(interface, "wl_seat") == 0) {
    struct seat *seat = calloc(1, sizeof(*seat));
    seat->seat =
        wl_registry_bind(registry, name, &wl_seat_interface, MIN(version, 5));
    wl_seat_add_listener(seat->seat, &seat_listener, seat);

  } else if (strcmp(interface, "wl_shm") == 0) {
    shm = wl_registry_bind(registry, name, &wl_shm_interface, MIN(version, 1));
    wl_shm_add_listener(shm, &shm_listener, NULL);

    cursor_theme = wl_cursor_theme_load(NULL, 32, shm);

  } else if (strcmp(interface, "weston_desktop_shell") == 0) {
    desktop_shell = wl_registry_bind(
        registry, name, &weston_desktop_shell_interface, MIN(version, 1));
    weston_desktop_shell_add_listener(desktop_shell, &desktop_shell_listener,
                                      NULL);

  } else if (strcmp(interface, "xdg_wm_base") == 0) {
    xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                   MIN(version, 4));
    xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);

  } else if (strcmp(interface, "wp_viewporter") == 0) {
    viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface,
                                  MIN(version, 1));

  } else if (strcmp(interface, "wl_output") == 0) {
    struct output *output = calloc(1, sizeof(*output));
    output->output =
        wl_registry_bind(registry, name, &wl_output_interface, MIN(version, 4));
    wl_output_add_listener(output->output, &output_listener, output);
    wl_list_insert(&output_list, &output->link);

    need_roundtrip = true;
  }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
                                   uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

static void sigchild_handler(int s) {
  int status;
  pid_t pid;

  while (pid = waitpid(-1, &status, WNOHANG), pid > 0)
    fprintf(stderr, "child %d exited\n", pid);
}

static int sort_apps(gconstpointer a, gconstpointer b) {
  GAppInfo *app_a = (GAppInfo *)a;
  GAppInfo *app_b = (GAppInfo *)b;

  return g_strcmp0(g_app_info_get_name(app_a), g_app_info_get_name(app_b));
}

int main(int argc, char **argv) {
  // setenv("WAYLAND_DEBUG", "client", 0);
  int ret = 0;
  wl_list_init(&output_list);
  wl_list_init(&seat_list);

  GList *app_list = g_app_info_get_all();
  application_list = NULL;
  for (GList *cur = app_list; cur != NULL; cur = cur->next) {
    GAppInfo *app = cur->data;
    if (g_app_info_should_show(app)) {
      application_list = g_list_insert_sorted(application_list, app, sort_apps);
      g_object_ref(app);
    }
  }
  g_list_free_full(app_list, g_object_unref);

  display = wl_display_connect(NULL);
  registry = wl_display_get_registry(display);

  signal(SIGCHLD, sigchild_handler);

  wl_registry_add_listener(registry, &registry_listener, NULL);
  while (need_roundtrip) {
    need_roundtrip = false;
    wl_display_roundtrip(display);
  }

  if (!desktop_shell) {
    fprintf(stderr, "ERROR: Unable to find weston desktop shell protocol\n");
    ret = 1;
    goto done;
  }

  if (!xdg_wm_base) {
    fprintf(stderr, "ERROR: xdg shell not found\n");
    ret = 1;
    goto done;
  }

  if (!viewporter) {
    fprintf(stderr, "ERROR: viewporter not found\n");
    ret = 1;
    goto done;
  }

  weston_desktop_shell_set_panel_position(
      desktop_shell, WESTON_DESKTOP_SHELL_PANEL_POSITION_TOP);
  weston_desktop_shell_desktop_ready(desktop_shell);

  while (true) {
    if (wl_display_dispatch(display) == -1) {
      perror("Error dispatching display");
      break;
    }
  }

done:
  g_list_free_full(application_list, g_object_unref);
  if (compositor) {
    wl_compositor_destroy(compositor);
  }
  if (shm) {
    wl_shm_destroy(shm);
  }
  if (desktop_shell) {
    weston_desktop_shell_destroy(desktop_shell);
  }
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return ret;
}
