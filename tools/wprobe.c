// Feasibility probe for the subtitle overlay: can a second wayland client put a
// surface on the screen alongside the video player?
//
// This is the whole question the overlay design rests on, and it is cheaper to
// answer with 100 lines that draw one coloured rectangle than with a subtitle
// renderer.  If a second toplevel can be mapped and drawn on, the subtitle
// program can too; if it cannot, the layout has to change and nothing else
// matters yet.
//
// The panel is 1080x1920 and the intent is video on top, subtitles below, so the
// rectangle is drawn in the lower third: a band that is deliberately not the
// whole screen, because a full-screen surface from a second client would cover
// the video and answer the question in a way that cannot be used.
//
// Build (the toolchain needs its own libdir on LD_LIBRARY_PATH, see build.sh):
//   WL=<cross-toolchain>/aarch64-buildroot-linux-gnu/sysroot
//   wayland-scanner client-header $WL/usr/share/wayland/wayland.xml wl-display.h
//   wayland-scanner private-code  $WL/usr/share/wayland/wayland.xml wl-protocol.c
//   wayland-scanner client-header $WL/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-client-protocol.h
//   aarch64-buildroot-linux-gnu-gcc -O2 -o wprobe wprobe.c wl-protocol.c \
//       -I. -I$WL/usr/include -L$WL/usr/lib -lwayland-client -lm
//
// Run on the board:
//   XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0 ./wprobe

#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wayland-client.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display  *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct xdg_wm_base  *wm_base;
static struct wl_shm      *shm;
static struct wl_surface   *surf;
static struct xdg_surface  *xsurf;
static struct xdg_toplevel *xtop;

static const int PANEL_W = 1080, PANEL_H = 1920;

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)data;
    if (!strcmp(iface, "wl_compositor"))
        comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 3);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
    (void)d; xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void surface_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d;(void)s;(void)o; }
static void surface_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d;(void)s;(void)o; }
static const struct wl_surface_listener surf_l = { surface_enter, surface_leave };

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    (void)d;
    // Acknowledge before painting.  The order matters: the compositor will
    // refuse a buffer committed for a surface whose initial configure has not
    // been acked, and it said so -- "xdg_surface has never been configured" --
    // even though a buffer had been attached.
    xdg_surface_ack_configure(s, serial);
}
static const struct xdg_surface_listener xsurf_l = { xsurf_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *states) {
    (void)d; (void)w; (void)h; (void)states;
    // Unconfigured: the first configure is the one that gives the buffer its
    // size, and painting before acknowledging it shows nothing.
    // Window geometry in surface-local coordinates: the whole surface is the
    // panel, and the subtitle band is drawn at the bottom of it.  No
    // set_bounds_size here -- the request is absent from the generated
    // interface and hand-marshalling it with a guessed opcode corrupted the
    // stream, after which weston never sent a configure at all.
    xdg_surface_set_window_geometry(xsurf, 0, 0, PANEL_W, PANEL_H);
    xdg_toplevel_set_title(xtop, "subtitle probe");

    // Paint the lower third: opaque black, so it is obvious this is our surface
    // and not the video underneath.
    const int stride = PANEL_W * 4;
    const int height = PANEL_H / 3;
    const int size = stride * height;
    int fd = memfd_create("probe", 0);
    if (fd < 0) { perror("memfd_create"); return; }
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return; }
    unsigned char *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); close(fd); return; }
    // ARGB8888 little-endian: B G R A per pixel.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < PANEL_W; ++x) {
            unsigned char *p = px + y * stride + x * 4;
            const int band = (y * 4 / height);      // a green gradient band
            p[0] = 0; p[1] = (unsigned char)(40 + band * 4); p[2] = 40; p[3] = 255;
        }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, PANEL_W, height,
                                                     stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    wl_surface_attach(surf, buf, 0, 0);
    // Damage the whole surface: the window is taller than the band, and only
    // the band is damaged, so nothing above it is repainted.
    wl_surface_damage(surf, 0, PANEL_H - height, PANEL_W, height);
    wl_surface_commit(surf);
    printf("probe: attached a %dx%d buffer at the bottom of a %dx%d panel\n",
           PANEL_W, height, PANEL_W, PANEL_H);
    fflush(stdout);
    close(fd);
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; wl_display_disconnect(dpy); exit(0); }
static void top_state(void *d, struct xdg_toplevel *t, struct wl_array *s) { (void)d;(void)t;(void)s; }
static void top_wm_cap(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d;(void)t;(void)a; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_state, top_wm_cap };

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "step: connect\n");
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "cannot connect to wayland\n"); return 1; }
    fprintf(stderr, "step: got display\n");
    reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    fprintf(stderr, "step: roundtrip globals\n");
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm_base) {
        fprintf(stderr, "missing globals: compositor=%p shm=%p wm_base=%p\n",
                (void *)comp, (void *)shm, (void *)wm_base);
        return 1;
    }
    fprintf(stderr, "step: globals comp=%p shm=%p wm=%p\n",(void*)comp,(void*)shm,(void*)wm_base);
    xdg_wm_base_add_listener(wm_base, &wm_l, NULL);
    surf = wl_compositor_create_surface(comp);
    wl_surface_add_listener(surf, &surf_l, NULL);
    xsurf = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xsurf, &xsurf_l, NULL);
    xtop = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(xtop, &top_l, NULL);
    xdg_toplevel_set_app_id(xtop, "subtitle-probe");
    fprintf(stderr, "step: committed, roundtrip configure\n");
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);                 // the configure arrives here
    printf("probe: running, 15 s.  A green band across the lower third means a\n"
           "       second wayland client can draw without disturbing the video.\n");
    fflush(stdout);
    for (int i = 0; i < 15; ++i) {
        wl_display_dispatch(dpy);
        usleep(100000);
    }
    return 0;
}
