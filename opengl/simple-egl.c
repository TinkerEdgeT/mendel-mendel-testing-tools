/*
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Port to Mendel Linux by Peter Nordström 1 June 2020
 *
 * GL code is done in glesgears.c and es2gears.c for GLES1 and GLES2
 * respectively
 *
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

#include <linux/input.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>

#if GLES==2
#include <GLES2/gl2.h>
#else
#include <GLES/gl.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 600
#define GOLDEN_IMG_DIR "/home/mendel/golden_images"
#define NUM_GOLDEN_IMAGES 10

static bool test = false;
static bool generate_ref_images = false;
static char *AppName;

struct window;

struct display {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shell *shell;
  struct {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig conf;
  } egl;
  struct window *window;
};

struct geometry {
  int width, height;
};

struct window {
  struct display *display;
  struct geometry geometry, window_size;
  struct {
    GLuint rotation_uniform;
    GLuint pos;
    GLuint col;
  } gl;

  uint32_t benchmark_time, frames;
  struct wl_egl_window *native;
  struct wl_surface *surface;
  EGLSurface egl_surface;
  int fullscreen, maximized, opaque, buffer_size, frame_sync, delay;
  bool wait_for_configure;
};

extern void RunGears(void *);

/* return current time (in seconds) */
static double
    current_time(void)
{
  struct timeval tv;
  (void) gettimeofday(&tv, NULL );

  return (double) tv.tv_sec + tv.tv_usec / 1000000.0;
}

static struct window *glwindow;
static GLubyte pixeldata[WINDOW_WIDTH * WINDOW_HEIGHT * 4];
static GLubyte golden_image_data[WINDOW_WIDTH * WINDOW_HEIGHT * 4];
void CheckFrame(int frame) {
  // This is a bit ugly and should be threaded, but for the purpose of
  // this testing it's okay
  FILE *fp;
  char filename[50];
  int size;
  sprintf(filename, "%s/%s_frame%d", GOLDEN_IMG_DIR, AppName, frame);

  // Read the framebuffer
  glReadPixels(0, 0, glwindow->geometry.width,
               glwindow->geometry.height,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               pixeldata);
  if (test) {
    // Compare the current frame with a saved golden image
    fp = fopen(filename, "r");
    if (fp == 0) {
      if (frame == 0) {
        printf("FAIL : No golden images to compare with\n");
        exit(1);
      } else {
        printf("PASS : All %d frames identical to golden images\n", frame - 1);
        exit(0);
      }
    }
    size = fread(golden_image_data, 4,
                 sizeof(golden_image_data)/4, fp);
    if (size != sizeof(golden_image_data)/4) {
      printf("FAIL : golden image has wrong size\n");
      fclose(fp);
      exit(1);
    }
    for (int i = 0; i < sizeof(golden_image_data); i++) {
      if (golden_image_data[i] != pixeldata[i]) {
        printf("FAIL : golden image mismatch frame: %d\n", frame);
        fclose(fp);
        exit(1);
      }
    }
  } else if (generate_ref_images) {
    // Save the frame as a golden image
    fp = fopen(filename, "w");
    fwrite(pixeldata, 4, sizeof(pixeldata)/4, fp);
    fclose(fp);
    if (frame == NUM_GOLDEN_IMAGES) {
      printf("Done generating golden images, exiting\n");
      exit(1);
    }
  }
}

void HandleFrame(void) {
  static int frame0, frame = 0;
  static double tRate0 = -1.0;
  double t = current_time();

  if (frame % 60 == 0) {
    CheckFrame(frame/60);
  }
  eglSwapBuffers(glwindow->display->egl.dpy, glwindow->egl_surface);
  frame++;

  if (tRate0 < 0.0) {
    tRate0 = t;
    frame0 = frame;
  }
  if (t - tRate0 >= 5.0) {
    GLfloat seconds = t - tRate0;
    GLfloat fps = (frame - frame0) / seconds;
    printf("%d frames in %3.1f seconds = %6.3f FPS\n",
           (frame - frame0), seconds, fps);
    fflush(stdout);
    tRate0 = t;
    frame0 = frame;
  }

}

static const char *vert_shader_text =
    "uniform mat4 rotation;\n"
    "attribute vec4 pos;\n"
    "attribute vec4 color;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  gl_Position = rotation * pos;\n"
    "  v_color = color;\n"
    "}\n";

static const char *frag_shader_text =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  gl_FragColor = v_color;\n"
    "}\n";

static int running = 1;

static void
    init_egl(struct display *display, struct window *window)
{
  static const EGLint context_attribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, GLES,
    EGL_NONE
  };
  const char *extensions;

  EGLint config_attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_DEPTH_SIZE, 16,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };

  EGLint major, minor, n, count, i, size;
  EGLConfig *configs;
  EGLBoolean ret;

  if (window->opaque || window->buffer_size == 16)
    config_attribs[9] = 0;

  display->egl.dpy = eglGetDisplay(display->display);

  ret = eglInitialize(display->egl.dpy, &major, &minor);
  assert(ret == EGL_TRUE);
  ret = eglBindAPI(EGL_OPENGL_ES_API);
  assert(ret == EGL_TRUE);

  if (!eglGetConfigs(display->egl.dpy, NULL, 0, &count) || count < 1)
    assert(0);

  configs = calloc(count, sizeof *configs);
  assert(configs);

  ret = eglChooseConfig(display->egl.dpy, config_attribs,
                        configs, 1, &n);
  assert(ret && n >= 1);

  display->egl.conf = configs[0];

  free(configs);
  if (display->egl.conf == NULL) {
    fprintf(stderr, "did not find config with buffer size %d\n",
            window->buffer_size);
    exit(EXIT_FAILURE);
  }

  display->egl.ctx = eglCreateContext(display->egl.dpy,
                                      display->egl.conf,
                                      EGL_NO_CONTEXT, context_attribs);
  assert(display->egl.ctx);

}

static void
    fini_egl(struct display *display)
{
  eglTerminate(display->egl.dpy);
  eglReleaseThread();
}

static void
    create_surface(struct window *window)
{
  struct display *display = window->display;
  struct wl_shell_surface *shell_surface;

  EGLBoolean ret;

  window->surface = wl_compositor_create_surface(display->compositor);

  shell_surface = wl_shell_get_shell_surface(display->shell,
                                             window->surface);
  wl_shell_surface_set_toplevel(shell_surface);

  window->native =
      wl_egl_window_create(window->surface,
                           window->geometry.width,
                           window->geometry.height);
  window->egl_surface = eglCreateWindowSurface(display->egl.dpy,
                                               display->egl.conf,
                                               window->native, NULL);
  window->wait_for_configure = true;
  wl_surface_commit(window->surface);

  ret = eglMakeCurrent(window->display->egl.dpy, window->egl_surface,
                       window->egl_surface, window->display->egl.ctx);
  assert(ret == EGL_TRUE);

  if (!window->frame_sync)
    eglSwapInterval(display->egl.dpy, 0);

}

static void
    destroy_surface(struct window *window)
{
  /* Required, otherwise segfault in egl_dri2.c: dri2_make_current()
   * on eglReleaseThread(). */
  eglMakeCurrent(window->display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);

  eglDestroySurface(window->display->egl.dpy,
                    window->egl_surface);
  wl_egl_window_destroy(window->native);
  wl_surface_destroy(window->surface);

}

static void
    registry_handle_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface, uint32_t version)
{
  struct display *d = data;

  if (strcmp(interface, "wl_compositor") == 0) {
    d->compositor =
        wl_registry_bind(registry, name,
                         &wl_compositor_interface,
                         1);
  } else if (strcmp(interface, "wl_shell") == 0) {
    d->shell = wl_registry_bind(registry, name,
                                &wl_shell_interface, 1);
  }
}

static void
    registry_handle_global_remove(void *data, struct wl_registry *registry,
                                  uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  registry_handle_global_remove
};

static void
    signal_int(int signum)
{
  running = 0;
}

static void usage(char *appname) {
  printf("Usage: %s [-golden | -test | -h]\n", appname);
}

int
    main(int argc, char **argv)
{
  struct sigaction sigint;
  struct display display = { 0 };
  struct window	 window	 = { 0 };
  int i, ret = 0;

  window.display = &display;
  glwindow = display.window = &window;
  window.geometry.width	 = WINDOW_WIDTH;
  window.geometry.height = WINDOW_HEIGHT;
  window.window_size = window.geometry;
  window.buffer_size = 32;
  window.frame_sync = 1;
  window.delay = 0;

  AppName = basename(argv[0]);
  if (argc > 2) {
    usage(AppName);
    exit(1);
  }

  if (argc == 2) {
    if (strcmp("-golden", argv[1]) == 0) {
      struct stat st = {0};
      if (stat(GOLDEN_IMG_DIR, &st) == -1) {
        mkdir(GOLDEN_IMG_DIR, 0700);
      }
      generate_ref_images = true;
    } else if (strcmp("-test", argv[1]) == 0) {
      test = true;
    } else if (strcmp("-h", argv[1]) == 0) {
      usage(AppName);
      exit(0);
    } else {
      usage(AppName);
      exit(1);
    }
  }

  display.display = wl_display_connect(NULL);
  assert(display.display);

  display.registry = wl_display_get_registry(display.display);
  wl_registry_add_listener(display.registry,
                           &registry_listener, &display);

  ret = wl_display_dispatch(display.display);
  wl_display_roundtrip(display.display);

  init_egl(&display, &window);
  create_surface(&window);

  sigint.sa_handler = signal_int;
  sigemptyset(&sigint.sa_mask);
  sigint.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sigint, NULL);

  /* The mainloop here is a little subtle.  Redrawing will cause
   * EGL to read events so we can just call
   * wl_display_dispatch_pending() to handle any events that got
   * queued up as a side effect. */

  RunGears((void *)&window);

  fprintf(stderr, "simple-egl exiting\n");

  destroy_surface(&window);
  fini_egl(&display);

  if (display.compositor)
    wl_compositor_destroy(display.compositor);

  wl_registry_destroy(display.registry);
  wl_display_flush(display.display);
  wl_display_disconnect(display.display);

  return 0;
}
