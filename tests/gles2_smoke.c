// Minimal GLES2 smoke test for the svga-vlkn harness.
// Uses surfaceless EGL with a pbuffer, clears to red, reads back a pixel.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
static void segv_handler(int sig) {
   void *bt[32];
   int n = backtrace(bt, 32);
   fprintf(stderr, "SIGSEGV caught, backtrace:\n");
   backtrace_symbols_fd(bt, n, 2);
   _exit(139);
}

#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

int main(void) {
    signal(SIGSEGV, segv_handler);
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(dpy != EGL_NO_DISPLAY, "eglGetDisplay");

    EGLint major, minor;
    CHECK(eglInitialize(dpy, &major, &minor), "eglInitialize");
    printf("EGL version: %d.%d\n", major, minor);
    printf("EGL vendor: %s\n", eglQueryString(dpy, EGL_VENDOR));

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg;
    CHECK(eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &ncfg) && ncfg > 0, "eglChooseConfig");

    EGLint pb_attribs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attribs);
    CHECK(surf != EGL_NO_SURFACE, "eglCreatePbufferSurface");

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");

    CHECK(eglMakeCurrent(dpy, surf, surf, ctx), "eglMakeCurrent");
    printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL version: %s\n", glGetString(GL_VERSION));

    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    unsigned char px[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("pixel(32,32) = %u %u %u %u\n", px[0], px[1], px[2], px[3]);

    int ok = (px[0] > 200 && px[1] < 50 && px[2] < 50);
    printf(ok ? "SMOKE TEST PASSED\n" : "SMOKE TEST FAILED\n");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    return ok ? 0 : 1;
}
