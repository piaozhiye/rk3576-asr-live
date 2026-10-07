// SPDX-License-Identifier: Apache-2.0
//
// On-screen subtitle renderer for a 1080x1920 portrait panel.
//
// Reads subtitle lines from a FIFO that the recogniser writes to, renders them
// with FreeType and hands the pixels to the weston compositor through a
// wl_shm buffer.  A separate wayland client rather than a GStreamer element
// because this board's GStreamer has no textoverlay and no pango: the elements
// that draw text are simply not installed, and no amount of pipeline work will
// conjure them.
//
// Layout: the video occupies the top of the panel and the subtitle band the
// bottom, because the panel is a portrait screen and that split uses it
// naturally.  It also sidesteps the reason the obvious design -- a
// zwlr_layer_surface floating over the video -- is not available: this weston
// does not implement layer-shell, and without it a second client cannot
// guarantee it draws above another client rather than beside or below it.  A
// band that owns its part of the screen needs no such guarantee.
//
//   usage: sub-overlay <fifo> [font.ttf]
//   usage: sub-overlay -once "<text>" [font.ttf]    draw one line and exit
//
// The -once form is how the rendering path is checked on the panel without a
// recogniser in the loop, which is the only way to tell a font problem from a
// protocol problem from a text problem.

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "wayland-client.h"
#include "xdg-shell-client-protocol.h"

#include "subtitle.h"

// ---------------------------------------------------------------- geometry

#define PANEL_W     1080
#define PANEL_H     1920
// The overlay is exactly one subtitle strip: 1080x320.  The surface, the
// shm buffer and the window geometry are all the same size, so weston's
// tiling gives it one 320-px slot (mapped last -> top of the stack, per
// play-sub's ordering) and the video window arranges itself in the space
// below.  A full-panel window here covered the video; a buffer/window size
// mismatch showed nothing.
#define SUBTITLE_H  320
#define MARGIN_X      48
#define ROW_GAP       18
#define FONT_PX       44          // han characters are two cells wide
#define MAX_ROWS       3
#define BG_ALPHA      0xC0        // 75% black, opaque enough to read over video
#define FG_R 0xFF
#define FG_G 0xFF
#define FG_B 0xFF

// ---------------------------------------------------------------- wayland

static struct wl_display    *dpy;
static struct wl_compositor *comp;
static struct wl_shm        *shm;
static struct xdg_wm_base    *wm_base;
static struct wl_surface    *surf;
static struct wl_buffer     *g_buf;
static struct xdg_surface   *xsurf;
static struct xdg_toplevel  *xtop;

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)data;
    if (!strcmp(iface, "wl_compositor"))
        comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        // Version 3 is the first with set_window_geometry as a client request;
        // asking for more than the generated interface describes is not an
        // error, but asking for less than is.
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 3 ? ver : 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d;(void)s;(void)o; }
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d;(void)s;(void)o; }
static const struct wl_surface_listener surf_l = { surf_enter, surf_leave };

// ---------------------------------------------------------------- drawing

static const int SHARED_H = SUBTITLE_H;   // surface == buffer == window

static FT_Library ft;
static FT_Face face;
static uint8_t *band;          // ARGB8888, the whole panel
static int have_text;

static void put_px(int x, int y, uint32_t argb) {
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return;
    uint32_t *p = (uint32_t *)(band + (size_t)y * PANEL_W * 4 + (size_t)x * 4);
    *p = argb;
}

// Alpha-blend fg over whatever is already there.  The band is cleared once and
// then only text is drawn, so a straight store would work for the first line and
// not for the second: a glyph overlapping another would punch a hole.
static void blend_px(int x, int y, unsigned char r, unsigned char g,
                     unsigned char b, unsigned char a) {
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return;
    uint32_t *p = (uint32_t *)(band + (size_t)y * PANEL_W * 4 + (size_t)x * 4);
    const unsigned dr = (*p >> 16) & 0xFF, dg = (*p >> 8) & 0xFF, db = *p & 0xFF;
    const unsigned da = (*p >> 24) & 0xFF;
    const unsigned oa = a + da * (255 - a) / 255;
    if (oa == 0) return;
    const unsigned ch = 255 * 255;
    *p = ((unsigned)oa << 24)
       | (((unsigned)r * a * 255 + dr * da * (255 - a) / 255) / oa << 16)
       | (((unsigned)g * a * 255 + dg * da * (255 - a) / 255) / oa << 8)
       | (((unsigned)b * a * 255 + db * da * (255 - a) / 255) / oa);
    (void)ch;
}

// Render one row of text, left to right, centred in the band.
static void draw_row(const char *s, int y) {
    if (FT_Load_Char(face, 0, FT_LOAD_RENDER) != 0) return;
    // Centred: the subtitle band is a fixed width and the text is not.
    // subtitle_columns counts terminal cells (hanzi = 2 cells); one cell is
    // FONT_PX/2 pixels at this face size, so divide, not multiply.
    const size_t cols = subtitle_columns(s);
    int pen_x = (PANEL_W - (int)(cols * FONT_PX / 2)) / 2;
    if (pen_x < MARGIN_X) pen_x = MARGIN_X;

    FT_UInt prev = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && *p != '\n'; ) {
        // The row ENDS at the wrap point: rendering '\n' through FreeType
        // drew the font's .notdef box between rows (the tofu between
        // 就是|测试) and the next row continued on the same line.
        if (*p == '\n') break;
        size_t clen = 1;
        if ((*p & 0xE0) == 0xC0) clen = 2;
        else if ((*p & 0xF0) == 0xE0) clen = 3;
        else if ((*p & 0xF8) == 0xF0) clen = 4;
        // Decode as UTF-8 so the glyph index matches the character rather than
        // being the byte value.
        FT_UInt idx = 0;
        {
            // Fall back to decoding properly for multi-byte.
            char buf[5] = {0};
            memcpy(buf, p, clen < 4 ? clen : 4);
            FT_UInt32 cp = 0;
            if (clen == 4)
                cp = ((FT_UInt32)(buf[0] & 0x07) << 18) |
                     ((FT_UInt32)(buf[1] & 0x3F) << 12) |
                     ((FT_UInt32)(buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
            else if (clen == 3)
                cp = ((FT_UInt32)(buf[0] & 0x0F) << 12) |
                     ((FT_UInt32)(buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
            else if (clen == 2)
                cp = ((FT_UInt32)(buf[0] & 0x1F) << 6) | (buf[1] & 0x3F);
            else
                cp = (FT_UInt32)buf[0];
            idx = FT_Get_Char_Index(face, cp);
        }
        // A character the font does not know: advance past it without
        // drawing.  FreeType would render .notdef (the tofu box) for
        // glyph 0, which is worse than a gap for a subtitle.
        if (idx == 0) { pen_x += FONT_PX / 2; p += clen; prev = 0; continue; }
        if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER) != 0) { p += clen; prev = 0; continue; }

        FT_Bitmap *bm = &face->glyph->bitmap;
        const int gx = pen_x + face->glyph->bitmap_left;
        const int gy = y - face->glyph->bitmap_top;
        for (unsigned r = 0; r < bm->rows; ++r)
            for (unsigned c = 0; c < bm->width; ++c) {
                const unsigned char cov = bm->buffer[r * bm->pitch + c];
                if (!cov) continue;
                blend_px(gx + (int)c, gy + (int)r, FG_R, FG_G, FG_B, cov);
            }
        pen_x += face->glyph->advance.x >> 6;
        p += clen;
        prev = idx;
        (void)prev;
    }
}

// Clear the band and draw text into it.  All coordinates are BAND-relative
// (0..SHARED_H): the shared buffer is the band itself, so panel-space writes
// (BAND_TOP..PANEL_H) landed outside it and only pre-configure presents were
// ever visible -- the "two frames then gone" symptom.
static int render(const char *text) {
    for (int y = 0; y < SHARED_H; ++y)
        for (int x = 0; x < PANEL_W; ++x)
            put_px(x, y, (uint32_t)BG_ALPHA << 24);

    if (!text || !*text) return SHARED_H;

    char rows[2048];
    // subtitle_wrap counts terminal cells (hanzi = 2); one cell is
    // FONT_PX/2 px, so the capacity in cells is twice what a naive
    // pixel/FONT_PX division gives.  That off-by-two wrapped 14 hanzi
    // into two rows on a band that fits 22.
    const int nrow = subtitle_wrap(rows, sizeof(rows), text,
                                   (size_t)((PANEL_W - 2 * MARGIN_X) * 2 / FONT_PX),
                                   MAX_ROWS);
    int y = SHARED_H / 2 - (nrow * (FONT_PX + ROW_GAP)) / 2;
    char *save = NULL;
    for (char *line = rows; line && *line; ) {
        draw_row(line, y + FONT_PX);
        y += FONT_PX + ROW_GAP;
        char *nl = strchr(line, '\n');
        if (!nl) break;
        *nl = '\0';
        line = nl + 1;
        save = line;
    }
    (void)save;
    return SHARED_H;
}

// Create a shared-memory buffer and copy the band into it.
//
// wl_shm_create_buffer does not give the client a mapping: the pool is mapped
// in the compositor's address space, not ours.  So the pixels are drawn into a
// private buffer and copied into the shared one here.
static int shared_fd = -1;
static uint8_t *shared_px;

static int share_once(void) {
    const int stride = PANEL_W * 4;
    const size_t size = (size_t)stride * SHARED_H;
    shared_fd = memfd_create("suboverlay", 0);
    if (shared_fd < 0) return -1;
    if (ftruncate(shared_fd, (off_t)size) < 0) return -1;
    shared_px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0);
    if (shared_px == MAP_FAILED) return -1;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, shared_fd, (int32_t)size);
    g_buf = wl_shm_pool_create_buffer(pool, 0, PANEL_W, SHARED_H, stride,
                                      WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return 0;
}

static struct wl_buffer *g_buf;

static int configured;

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    (void)d;
    // Acknowledge before painting.  The compositor refuses a buffer for a
    // surface whose initial configure has not been acked, and reports it as
    // "xdg_surface has never been configured" even when a buffer is attached.
    xdg_surface_ack_configure(s, serial);
}
static const struct xdg_surface_listener xsurf_l = { xsurf_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *states) {
    (void)d; (void)t; (void)w; (void)h; (void)states;
    configured = 1;
    xdg_surface_set_window_geometry(xsurf, 0, 0, PANEL_W, SHARED_H);
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d;(void)t; wl_display_disconnect(dpy); exit(0); }
static void top_state(void *d, struct xdg_toplevel *t, struct wl_array *s) { (void)d;(void)t;(void)s; }
static void top_wm_cap(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d;(void)t;(void)a; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_state, top_wm_cap };

// Paint the current band and present it.
static void present(void) {
    if (!g_buf) { fprintf(stderr, "share_once not called\n"); return; }
    // The shared buffer is the band itself; copy straight across.
    memcpy(shared_px, band, (size_t)PANEL_W * 4 * SHARED_H);
    // Zero-offset attach of a buffer sized exactly like the surface: the
    // pattern wprobe proved visible.  A BAND_TOP y-offset on an xdg surface
    // is not honoured by weston after the first configure.
    wl_surface_attach(surf, g_buf, 0, 0);
    wl_surface_damage(surf, 0, 0, PANEL_W, SHARED_H);
    wl_surface_commit(surf);
    wl_display_flush(dpy);
    wl_display_dispatch_pending(dpy);
}

// Bounded wait on BOTH the fifo and the wayland fd, with a time floor.
//
// This replaces the old poll(wl_fd, 20ms) pacing -- measured 2026-09-30,
// that loop spun at 100% of a core while idle (the task showed "running"
// in /proc/<pid>/syscall on every sample): some wakeups never actually
// sleep inside poll, so poll's timeout is not a rate guarantee.  Two
// changes:
//   * the fifo is polled too, so a new line wakes the loop at once instead
//     of on the next tick;
//   * if the whole wait took under 2 ms, sleep the remainder explicitly.
//     Even a permanently-ready fd then costs <= 500 trivial wakeups per
//     second instead of a whole core.
static void wait_fds(struct wl_display *display, int fifo_fd, int ms) {
    wl_display_flush(display);
    struct pollfd p[2] = {
        { wl_display_get_fd(display), POLLIN, 0 },
        { fifo_fd, POLLIN, 0 },
    };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    poll(p, 2, ms);
    wl_display_dispatch_pending(display);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L;
    if (us < 2000) {
        struct timespec s = { 0, 2000000L - (us > 0 ? us : 0) };
        nanosleep(&s, NULL);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *font = "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";
    const char *once = NULL;
    const char *fifo = NULL;
    // Positional args, in order: <fifo> <font>.  The previous version
    // assigned EVERY positional to fifo in the loop (font overwrote fifo)
    // and then re-read the last one as font, so the read loop opened the
    // FONT FILE as the fifo and surfaced its bytes (OTTO/BASE) instead of
    // subtitle text.
    const char *pos[2] = {0, 0};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-once") && i + 1 < argc) once = argv[++i];
        else if (argv[i][0] != '-' && npos < 2) pos[npos++] = argv[i];
    }
    // In -once mode there is no fifo: the single positional is the FONT.
    // (-once "text" font.otf previously left font at its wqy default, which
    // does not exist on this board, and assigned the font path to fifo.)
    if (once) {
        if (npos > 0) font = pos[0];
    } else {
        if (npos > 0) fifo = pos[0];
        if (npos > 1) font = pos[1];
    }

    if (FT_Init_FreeType(&ft)) { fprintf(stderr, "freetype init failed\n"); return 1; }
    if (FT_New_Face(ft, font, 0, &face)) {
        fprintf(stderr, "cannot open font %s\n", font);
        return 1;
    }
    FT_Set_Pixel_Sizes(face, 0, FONT_PX);
    if (!FT_IS_SCALABLE(face)) { fprintf(stderr, "font is not scalable\n"); return 1; }
    fprintf(stderr, "font: %s, %d px, family \"%s\"\n", font, FONT_PX,
            face->family_name ? face->family_name : "?");

    band = mmap(NULL, (size_t)PANEL_W * PANEL_H * 4, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (band == MAP_FAILED) { perror("mmap"); return 1; }

    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "cannot connect to wayland\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm_base) {
        fprintf(stderr, "missing globals\n");
        return 1;
    }
    xdg_wm_base_add_listener(wm_base, &wm_l, NULL);
    surf = wl_compositor_create_surface(comp);
    wl_surface_add_listener(surf, &surf_l, NULL);
    xsurf = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xsurf, &xsurf_l, NULL);
    xtop = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(xtop, &top_l, NULL);
    xdg_toplevel_set_title(xtop, "subtitles");
    xdg_toplevel_set_app_id(xtop, "sub-overlay");
    // Positioning: this weston's xdg-shell (v3, scanner output) predates
    // xdg_toplevel.set_positioner, so there is no protocol way to pin the
    // window.  Weston's tiling puts the most recently mapped toplevel first
    // (top-left origin for a vertical stack), which lands the band at the
    // top of a portrait screen -- measured good enough for subtitles.
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    if (!configured) { fprintf(stderr, "never configured\n"); return 1; }
    if (share_once() < 0) { perror("share_once"); return 1; }
    fprintf(stderr, "ready\n");

    if (once) {
        // One line, held on screen.  This is the mode that separates a font
        // problem from a wayland problem without a recogniser in the loop.
        char rows[2048];
        subtitle_wrap(rows, sizeof(rows), once, (size_t)((PANEL_W - 2 * MARGIN_X) * 2 / FONT_PX), MAX_ROWS);
        fprintf(stderr, "rows for \"%s\": %s\n", once,
                strchr(rows, '\n') ? "multi" : "one");
        render(once);
        present();
        for (int i = 0; i < 150; ++i) { wl_display_dispatch(dpy); usleep(100000); }
        return 0;
    }

    if (!fifo) { fprintf(stderr, "usage: sub-overlay <fifo> [font]\n"); return 2; }
    mkfifo(fifo, 0666);   // idempotent: the recogniser may not be up yet
    // O_NONBLOCK so a late writer cannot wedge this at startup: the FIFO already
    // exists (we just made it, or the recogniser did) so a blocking open returns at once, and
    // without the flag the read below would block the whole event loop and the
    // surface would never repaint.  A blocking read is restored afterwards,
    // because there is nothing else to do between lines and spinning would burn
    // the CPU the recogniser needs.
    int fd = open(fifo, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", fifo, strerror(errno)); return 1; }
    // Do NOT drain here.  The drain consumes everything currently queued, and
    // the writer -- the recogniser -- may already be connected and mid-line, so
    // draining throws away the line that was about to be shown.  Stale data from
    // a previous run is the lesser problem, and it is handled where it belongs:
    // the recogniser recreates the FIFO on start, so a fresh run begins with an
    // empty pipe.
    (void)0;

    char line[1024];
    size_t n = 0;
    have_text = 0;
    for (;;) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {                 // no data yet
                wait_fds(dpy, fd, 20);
                continue;
            }
            fprintf(stderr, "read: %s\n", strerror(errno));
            break;
        }
        if (r == 0) {                             // writer closed (FIFO EOF)
            // The recogniser closes the write end when it exits, and this
            // program is expected to outlive that: keep the last line on
            // screen and wait for the next writer.
            wait_fds(dpy, fd, 100);
            continue;
        }
        if (ch == '\n') {
            line[n] = '\0';
            if (getenv("SUB_DEBUG_RAW")) {
                fprintf(stderr, "raw %zu bytes:", n);
                for (size_t i = 0; i < n && i < 32; ++i)
                    fprintf(stderr, " %02x", (unsigned char)line[i]);
                fprintf(stderr, "\n");
            }
            if (n > 0) {
                render(line);
                present();
                have_text = 1;
                fprintf(stderr, "line: %s\n", line);
            }
            n = 0;
            continue;
        }
        if (n + 1 < sizeof(line)) line[n++] = ch;
    }
    close(fd);
    return 0;
}
