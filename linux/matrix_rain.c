/* matrix_rain.c — Matrix Rain screensaver, Linux/X11 frontend (Xlib + Xft).
 *
 * Modes: fullscreen override-redirect (default), -window, -root,
 * -window-id <id>, and the XSCREENSAVER_WINDOW env var (xscreensaver hack).
 * Simulation lives in rain.c and is display-independent (see -selftest).
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xdbe.h>
#include <X11/Xft/Xft.h>

#include "rain.h"

#define CELL_W_FACTOR 0.62f
#define CELL_H_FACTOR 1.05f
#define SHADES 24               /* quantized trail brightness levels */
#define MAX_GLYPHS 80

/* ---------------------------------------------------------------- settings */

typedef enum { MODE_FULLSCREEN, MODE_WINDOW, MODE_ROOT, MODE_WINDOW_ID } RunMode;

typedef struct {
    unsigned char r, g, b;      /* rain (trail) color */
    float density;
    int   size;
    float speed;
    int   fps;
    char  font[256];
    int   font_given;
} Settings;

static const struct { const char *name; unsigned char r, g, b; } PRESETS[] = {
    { "Matrix Green", 0x00, 0xFF, 0x41 },
    { "Amber",        0xFF, 0xB0, 0x00 },
    { "Ice Blue",     0x00, 0xC8, 0xFF },
    { "Crimson",      0xFF, 0x20, 0x20 },
    { "Violet",       0xB0, 0x40, 0xFF },
    { "White",        0xE0, 0xE0, 0xE0 },
};

/* Compare ignoring case and any non-alphanumeric chars ("ice blue" == "IceBlue"). */
static int name_matches(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b)
            return *a == *b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
}

static int parse_color(const char *s, Settings *st)
{
    for (size_t i = 0; i < sizeof PRESETS / sizeof PRESETS[0]; i++) {
        if (name_matches(s, PRESETS[i].name)) {
            st->r = PRESETS[i].r;
            st->g = PRESETS[i].g;
            st->b = PRESETS[i].b;
            return 0;
        }
    }
    if (*s == '#')
        s++;
    if (strlen(s) == 6 && strspn(s, "0123456789abcdefABCDEF") == 6) {
        unsigned v = (unsigned)strtoul(s, NULL, 16);
        st->r = (unsigned char)(v >> 16);
        st->g = (unsigned char)(v >> 8);
        st->b = (unsigned char)v;
        return 0;
    }
    return -1;
}

static float clampf(float v, float lo, float hi, const char *key)
{
    if (v < lo || v > hi) {
        fprintf(stderr, "matrix-rain: %s=%g out of range [%g, %g], clamping\n",
                key, (double)v, (double)lo, (double)hi);
        return v < lo ? lo : hi;
    }
    return v;
}

static int clampi(int v, int lo, int hi, const char *key)
{
    if (v < lo || v > hi) {
        fprintf(stderr, "matrix-rain: %s=%d out of range [%d, %d], clamping\n",
                key, v, lo, hi);
        return v < lo ? lo : hi;
    }
    return v;
}

static void apply_setting(Settings *st, const char *key, const char *val,
                          const char *origin)
{
    if (!strcmp(key, "color")) {
        if (parse_color(val, st) != 0)
            fprintf(stderr, "matrix-rain: %s: bad color '%s' "
                    "(use #RRGGBB or a preset name)\n", origin, val);
    } else if (!strcmp(key, "density")) {
        st->density = clampf((float)atof(val), RAIN_DENSITY_MIN, RAIN_DENSITY_MAX, key);
    } else if (!strcmp(key, "size")) {
        st->size = clampi(atoi(val), RAIN_SIZE_MIN, RAIN_SIZE_MAX, key);
    } else if (!strcmp(key, "speed")) {
        st->speed = clampf((float)atof(val), RAIN_SPEED_MIN, RAIN_SPEED_MAX, key);
    } else if (!strcmp(key, "fps")) {
        st->fps = clampi(atoi(val), 1, 240, key);
    } else if (!strcmp(key, "font")) {
        snprintf(st->font, sizeof st->font, "%s", val);
        st->font_given = 1;
    } else {
        fprintf(stderr, "matrix-rain: %s: unknown key '%s'\n", origin, key);
    }
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static void load_config(Settings *st)
{
    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(path, sizeof path, "%s/matrix-rain/matrix-rain.conf", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home)
            return;
        snprintf(path, sizeof path, "%s/.config/matrix-rain/matrix-rain.conf", home);
    }
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (*key)
            apply_setting(st, key, val, path);
    }
    fclose(f);
}

static void usage(FILE *out)
{
    fputs(
"Usage: matrix-rain [options]\n"
"\n"
"Matrix digital rain screensaver for X11.\n"
"\n"
"Modes (default: fullscreen override-redirect window, exits on any key or\n"
"mouse press):\n"
"  -window          run in a normal resizable window (for testing)\n"
"  -root            draw on the root window\n"
"  -window-id <id>  draw into an existing window (hex or decimal XID);\n"
"                   the XSCREENSAVER_WINDOW env var is honored too, which\n"
"                   makes this a drop-in xscreensaver hack\n"
"\n"
"Settings (override ~/.config/matrix-rain/matrix-rain.conf):\n"
"  -color <c>       trail color: #RRGGBB or a preset — Matrix Green, Amber,\n"
"                   Ice Blue, Crimson, Violet, White (default #00FF41)\n"
"  -density <f>     0.05 - 1.0, fraction of active columns (default 0.75)\n"
"  -size <px>       glyph size 8 - 64 (default 18)\n"
"  -speed <f>       fall speed multiplier 0.5 - 3.0 (default 1.0)\n"
"  -fps <n>         target frames per second (default 30)\n"
"  -font <name>     fontconfig font name (default: Noto Sans Mono CJK JP,\n"
"                   then monospace)\n"
"  -h, --help       show this help\n",
        out);
}

/* ------------------------------------------------------------- glyph table */

static FcChar32 glyphs[MAX_GLYPHS];
static int glyph_count;

static int build_glyphs(int katakana)
{
    int n = 0;
    if (katakana) {
        for (FcChar32 u = 0xFF71; u <= 0xFF9D; u++)   /* ｱ..ﾝ half-width */
            glyphs[n++] = u;
        for (FcChar32 u = '0'; u <= '9'; u++)
            glyphs[n++] = u;
        static const FcChar32 extra[] = {
            'Z', ':', 0x30FB, '.', '"', '=', '*', '+', '-', '<', '>',
            0x00A6, 0xFF5C
        };
        for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++)
            glyphs[n++] = extra[i];
    } else {
        for (FcChar32 u = '0'; u <= '9'; u++)
            glyphs[n++] = u;
        for (FcChar32 u = 'A'; u <= 'Z'; u++)
            glyphs[n++] = u;
    }
    return n;
}

static int font_has_katakana(Display *dpy, XftFont *font)
{
    for (FcChar32 u = 0xFF71; u <= 0xFF9D; u++)
        if (!XftCharExists(dpy, font, u))
            return 0;
    return 1;
}

/* Open the first candidate font that covers half-width katakana; if none
 * does, keep the first font that opened at all and use the ASCII glyph set. */
static XftFont *pick_font(Display *dpy, int screen, const Settings *st)
{
    const char *candidates[3];
    int ncand = 0;
    if (st->font_given)
        candidates[ncand++] = st->font;
    candidates[ncand++] = "Noto Sans Mono CJK JP";
    candidates[ncand++] = "monospace";

    XftFont *fallback = NULL;
    for (int i = 0; i < ncand; i++) {
        char spec[320];
        snprintf(spec, sizeof spec, "%s:size=%d", candidates[i], st->size);
        XftFont *f = XftFontOpenName(dpy, screen, spec);
        if (!f)
            continue;
        if (font_has_katakana(dpy, f)) {
            if (fallback)
                XftFontClose(dpy, fallback);
            glyph_count = build_glyphs(1);
            return f;
        }
        if (!fallback)
            fallback = f;
        else
            XftFontClose(dpy, f);
    }
    if (!fallback) {
        fprintf(stderr, "matrix-rain: no usable font found\n");
        exit(1);
    }
    fprintf(stderr, "matrix-rain: no katakana coverage, "
            "falling back to digits/latin glyphs\n");
    glyph_count = build_glyphs(0);
    return fallback;
}

/* ----------------------------------------------------------------- selftest */

static int selftest(const Settings *st)
{
    const int cols = 100, rows = 40, frames = 300;
    const float dt = 1.0f / 30.0f;

    glyph_count = build_glyphs(1);
    Rain *r = rain_create(cols, rows, st->density, st->speed, glyph_count, 12345u);
    if (!r) {
        fprintf(stderr, "selftest: rain_create failed\n");
        return 1;
    }
    printf("selftest: grid %dx%d, density=%.2f speed=%.2f glyphs=%d, "
           "%d frames @ dt=%.4f\n",
           cols, rows, (double)st->density, (double)st->speed, glyph_count,
           frames, (double)dt);

    for (int f = 1; f <= frames; f++) {
        rain_step(r, dt);
        if (f % 60 == 0) {
            long visible = 0;
            size_t n = (size_t)cols * (size_t)rows;
            for (size_t i = 0; i < n; i++)
                if (r->cell_bright[i] > 0.0f)
                    visible++;
            printf("selftest: frame %3d  active=%3d  visible-cells=%4ld  "
                   "spawned=%3ld  died=%3ld  mutations=%ld\n",
                   f, r->active_count, visible,
                   r->stat_spawned, r->stat_died, r->stat_mutations);
        }
    }

    long visible = 0;
    float maxb = 0.0f;
    size_t n = (size_t)cols * (size_t)rows;
    for (size_t i = 0; i < n; i++) {
        if (r->cell_bright[i] > 0.0f)
            visible++;
        if (r->cell_bright[i] > maxb)
            maxb = r->cell_bright[i];
    }
    float min_head = 1e9f, max_head = -1e9f;
    for (int c = 0; c < cols; c++) {
        if (r->streams[c].active) {
            if (r->streams[c].head_row < min_head) min_head = r->streams[c].head_row;
            if (r->streams[c].head_row > max_head) max_head = r->streams[c].head_row;
        }
    }
    printf("selftest: final  active=%d  visible=%ld  max-brightness=%.2f  "
           "head-rows=[%.1f, %.1f]\n",
           r->active_count, visible, (double)maxb,
           (double)min_head, (double)max_head);

    int ok = r->stat_spawned > 0 && r->stat_died > 0 &&
             r->active_count > 0 && visible > 0 && max_head > 0.0f;
    printf("selftest: streams spawned=%ld moved(max head %.1f rows) died=%ld "
           "=> %s\n",
           r->stat_spawned, (double)max_head, r->stat_died,
           ok ? "PASS" : "FAIL");
    rain_destroy(r);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ X glue */

static volatile sig_atomic_t quit_flag;

static void on_signal(int sig)
{
    (void)sig;
    quit_flag = 1;
}

static int xdbe_error;
static int (*prev_error_handler)(Display *, XErrorEvent *);

static int trap_xdbe_error(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    xdbe_error = 1;
    return 0;
}

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    int width, height;
    int own_window;             /* we created it (fullscreen or -window) */
    int use_dbe;
    XdbeBackBuffer back;
    Pixmap pix;                 /* fallback back buffer */
    GC gc;
    XftDraw *draw;
    XftFont *font;
    XftColor black, head;
    XftColor trail[SHADES + 1];
    int cell_w, cell_h, baseline;
    short glyph_xoff[MAX_GLYPHS]; /* centering offset per glyph */
} Gfx;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void alloc_colors(Gfx *g, const Settings *st)
{
    Visual *vis = DefaultVisual(g->dpy, g->screen);
    Colormap cmap = DefaultColormap(g->dpy, g->screen);
    XRenderColor rc;

    rc.red = rc.green = rc.blue = 0;
    rc.alpha = 0xFFFF;
    XftColorAllocValue(g->dpy, vis, cmap, &rc, &g->black);

    for (int i = 0; i <= SHADES; i++) {
        float t = (float)i / SHADES;
        rc.red   = (unsigned short)(st->r * 257 * t);
        rc.green = (unsigned short)(st->g * 257 * t);
        rc.blue  = (unsigned short)(st->b * 257 * t);
        rc.alpha = 0xFFFF;
        XftColorAllocValue(g->dpy, vis, cmap, &rc, &g->trail[i]);
    }

    /* head = rain + (white - rain) * 0.75 */
    rc.red   = (unsigned short)(st->r * 257 + (0xFFFF - st->r * 257) * 3 / 4);
    rc.green = (unsigned short)(st->g * 257 + (0xFFFF - st->g * 257) * 3 / 4);
    rc.blue  = (unsigned short)(st->b * 257 + (0xFFFF - st->b * 257) * 3 / 4);
    rc.alpha = 0xFFFF;
    XftColorAllocValue(g->dpy, vis, cmap, &rc, &g->head);
}

static void compute_metrics(Gfx *g, const Settings *st)
{
    g->cell_w = (int)(CELL_W_FACTOR * st->size + 0.5f);
    g->cell_h = (int)(CELL_H_FACTOR * st->size + 0.5f);
    if (g->cell_w < 1) g->cell_w = 1;
    if (g->cell_h < 1) g->cell_h = 1;
    g->baseline = (g->cell_h + g->font->ascent - g->font->descent + 1) / 2;

    for (int i = 0; i < glyph_count; i++) {
        XGlyphInfo gi;
        XftTextExtents32(g->dpy, g->font, &glyphs[i], 1, &gi);
        g->glyph_xoff[i] = (short)((g->cell_w - gi.xOff) / 2);
    }
}

static Cursor make_blank_cursor(Display *dpy, Window win)
{
    char bits = 0;
    Pixmap p = XCreateBitmapFromData(dpy, win, &bits, 1, 1);
    XColor c;
    memset(&c, 0, sizeof c);
    Cursor cur = XCreatePixmapCursor(dpy, p, p, &c, &c, 0, 0);
    XFreePixmap(dpy, p);
    return cur;
}

/* Set up the drawing target: Xdbe back buffer if available, else a Pixmap. */
static void setup_backbuffer(Gfx *g)
{
    int major, minor;
    g->use_dbe = 0;
    if (XdbeQueryExtension(g->dpy, &major, &minor)) {
        xdbe_error = 0;
        prev_error_handler = XSetErrorHandler(trap_xdbe_error);
        g->back = XdbeAllocateBackBufferName(g->dpy, g->win, XdbeUndefined);
        XSync(g->dpy, False);
        XSetErrorHandler(prev_error_handler);
        if (!xdbe_error && g->back != None)
            g->use_dbe = 1;
    }
    if (!g->use_dbe) {
        g->pix = XCreatePixmap(g->dpy, g->win, (unsigned)g->width,
                               (unsigned)g->height,
                               (unsigned)DefaultDepth(g->dpy, g->screen));
        g->gc = XCreateGC(g->dpy, g->win, 0, NULL);
    }

    Drawable target = g->use_dbe ? (Drawable)g->back : (Drawable)g->pix;
    g->draw = XftDrawCreate(g->dpy, target,
                            DefaultVisual(g->dpy, g->screen),
                            DefaultColormap(g->dpy, g->screen));
}

static void resize_backbuffer(Gfx *g)
{
    if (g->use_dbe)
        return;                 /* Xdbe back buffers track the window size */
    XFreePixmap(g->dpy, g->pix);
    g->pix = XCreatePixmap(g->dpy, g->win, (unsigned)g->width,
                           (unsigned)g->height,
                           (unsigned)DefaultDepth(g->dpy, g->screen));
    XftDrawChange(g->draw, g->pix);
}

static void render(Gfx *g, const Rain *r)
{
    XftDrawRect(g->draw, &g->black, 0, 0, (unsigned)g->width, (unsigned)g->height);

    for (int c = 0; c < r->cols; c++) {
        const float *bright = r->cell_bright + (size_t)c * (size_t)r->rows;
        const unsigned short *gly = r->cell_glyph + (size_t)c * (size_t)r->rows;
        int head = rain_head_cell(r, c);
        int x = c * g->cell_w;
        for (int y = 0; y < r->rows; y++) {
            XftColor *color;
            if (y == head) {
                color = &g->head;
            } else {
                float b = bright[y];
                if (b < 0.02f)
                    continue;
                int lvl = (int)(b * SHADES + 0.5f);
                if (lvl > SHADES)
                    lvl = SHADES;
                color = &g->trail[lvl];
            }
            int gi = gly[y];
            XftDrawString32(g->draw, color, g->font,
                            x + g->glyph_xoff[gi], y * g->cell_h + g->baseline,
                            &glyphs[gi], 1);
        }
    }

    if (g->use_dbe) {
        XdbeSwapInfo si = { g->win, XdbeUndefined };
        XdbeSwapBuffers(g->dpy, &si, 1);
    } else {
        XCopyArea(g->dpy, g->pix, g->win, g->gc, 0, 0,
                  (unsigned)g->width, (unsigned)g->height, 0, 0);
    }
    XFlush(g->dpy);
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    Settings st = {
        .r = 0x00, .g = 0xFF, .b = 0x41,
        .density = RAIN_DENSITY_DEF,
        .size = RAIN_SIZE_DEF,
        .speed = RAIN_SPEED_DEF,
        .fps = 30,
        .font = "",
        .font_given = 0,
    };
    RunMode mode = MODE_FULLSCREEN;
    Window target_win = None;
    int want_selftest = 0;

    load_config(&st);

    const char *xss = getenv("XSCREENSAVER_WINDOW");
    if (xss && *xss) {
        target_win = (Window)strtoul(xss, NULL, 0);
        if (target_win != None)
            mode = MODE_WINDOW_ID;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-')
            a++;                                    /* accept --flag too */
        if (!strcmp(a, "-h") || !strcmp(a, "-help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-window")) {
            mode = MODE_WINDOW;
        } else if (!strcmp(a, "-root")) {
            mode = MODE_ROOT;
        } else if (!strcmp(a, "-window-id")) {
            if (++i >= argc) goto missing_arg;
            target_win = (Window)strtoul(argv[i], NULL, 0);
            mode = MODE_WINDOW_ID;
        } else if (!strcmp(a, "-selftest")) {
            want_selftest = 1;
        } else if (!strcmp(a, "-color") || !strcmp(a, "-density") ||
                   !strcmp(a, "-size") || !strcmp(a, "-speed") ||
                   !strcmp(a, "-fps") || !strcmp(a, "-font")) {
            if (++i >= argc) goto missing_arg;
            apply_setting(&st, a + 1, argv[i], "command line");
        } else {
            fprintf(stderr, "matrix-rain: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 2;
        }
        continue;
    missing_arg:
        fprintf(stderr, "matrix-rain: option '%s' needs an argument\n", a);
        return 2;
    }

    if (want_selftest)
        return selftest(&st);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Gfx g;
    memset(&g, 0, sizeof g);
    g.dpy = XOpenDisplay(NULL);
    if (!g.dpy) {
        fprintf(stderr, "matrix-rain: cannot open display\n");
        return 1;
    }
    g.screen = DefaultScreen(g.dpy);
    Window root = RootWindow(g.dpy, g.screen);
    Atom wm_delete = None;

    switch (mode) {
    case MODE_FULLSCREEN: {
        g.width = DisplayWidth(g.dpy, g.screen);
        g.height = DisplayHeight(g.dpy, g.screen);
        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        attrs.background_pixel = BlackPixel(g.dpy, g.screen);
        attrs.event_mask = KeyPressMask | ButtonPressMask | ExposureMask |
                           StructureNotifyMask;
        g.win = XCreateWindow(g.dpy, root, 0, 0,
                              (unsigned)g.width, (unsigned)g.height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel | CWEventMask,
                              &attrs);
        g.own_window = 1;
        Cursor blank = make_blank_cursor(g.dpy, g.win);
        XDefineCursor(g.dpy, g.win, blank);
        XMapRaised(g.dpy, g.win);
        XSync(g.dpy, False);
        for (int try = 0; try < 20; try++) {
            int kb = XGrabKeyboard(g.dpy, g.win, True, GrabModeAsync,
                                   GrabModeAsync, CurrentTime);
            int pt = XGrabPointer(g.dpy, g.win, True, ButtonPressMask,
                                  GrabModeAsync, GrabModeAsync, None, blank,
                                  CurrentTime);
            if (kb == GrabSuccess && pt == GrabSuccess)
                break;
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        break;
    }
    case MODE_WINDOW: {
        g.width = 1024;
        g.height = 768;
        XSetWindowAttributes attrs;
        attrs.background_pixel = BlackPixel(g.dpy, g.screen);
        attrs.event_mask = KeyPressMask | ButtonPressMask | ExposureMask |
                           StructureNotifyMask;
        g.win = XCreateWindow(g.dpy, root, 0, 0,
                              (unsigned)g.width, (unsigned)g.height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixel | CWEventMask, &attrs);
        g.own_window = 1;
        XStoreName(g.dpy, g.win, "Matrix Rain");
        wm_delete = XInternAtom(g.dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(g.dpy, g.win, &wm_delete, 1);
        XMapWindow(g.dpy, g.win);
        break;
    }
    case MODE_ROOT:
    case MODE_WINDOW_ID: {
        g.win = (mode == MODE_ROOT) ? root : target_win;
        XWindowAttributes wa;
        if (!XGetWindowAttributes(g.dpy, g.win, &wa)) {
            fprintf(stderr, "matrix-rain: bad window id 0x%lx\n", g.win);
            return 1;
        }
        g.width = wa.width;
        g.height = wa.height;
        if (mode == MODE_WINDOW_ID)
            XSelectInput(g.dpy, g.win, ExposureMask | StructureNotifyMask);
        break;
    }
    }

    g.font = pick_font(g.dpy, g.screen, &st);
    alloc_colors(&g, &st);
    compute_metrics(&g, &st);
    setup_backbuffer(&g);

    int cols = g.width / g.cell_w;
    int rows = g.height / g.cell_h;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    Rain *rain = rain_create(cols, rows, st.density, st.speed, glyph_count,
                             (unsigned)time(NULL) ^ (unsigned)getpid());
    if (!rain) {
        fprintf(stderr, "matrix-rain: out of memory\n");
        return 1;
    }

    const double frame = 1.0 / st.fps;
    int xfd = ConnectionNumber(g.dpy);
    double last = now_sec();
    double next = last;
    int running = 1;

    while (running && !quit_flag) {
        while (XPending(g.dpy)) {
            XEvent ev;
            XNextEvent(g.dpy, &ev);
            switch (ev.type) {
            case KeyPress:
                if (mode == MODE_FULLSCREEN) {
                    running = 0;
                } else if (mode == MODE_WINDOW) {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_Escape || ks == XK_q)
                        running = 0;
                }
                break;
            case ButtonPress:
                if (mode == MODE_FULLSCREEN)
                    running = 0;
                break;
            case ClientMessage:
                if (wm_delete != None &&
                    (Atom)ev.xclient.data.l[0] == wm_delete)
                    running = 0;
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != g.width ||
                    ev.xconfigure.height != g.height) {
                    g.width = ev.xconfigure.width;
                    g.height = ev.xconfigure.height;
                    resize_backbuffer(&g);
                    cols = g.width / g.cell_w;
                    rows = g.height / g.cell_h;
                    if (cols < 1) cols = 1;
                    if (rows < 1) rows = 1;
                    if (rain_resize(rain, cols, rows) != 0) {
                        fprintf(stderr, "matrix-rain: out of memory\n");
                        running = 0;
                    }
                }
                break;
            default:
                break;
            }
        }
        if (!running || quit_flag)
            break;

        double t = now_sec();
        if (t >= next) {
            float dt = (float)(t - last);
            if (dt > 0.1f)
                dt = 0.1f;      /* clamp after stalls; keep animation smooth */
            last = t;
            rain_step(rain, dt);
            render(&g, rain);
            next += frame;
            if (next < t)
                next = t + frame;
        }

        double wait = next - now_sec();
        if (wait > 0) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            struct timeval tv;
            tv.tv_sec = (time_t)wait;
            tv.tv_usec = (suseconds_t)((wait - (double)tv.tv_sec) * 1e6);
            if (select(xfd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR)
                break;
        }
    }

    rain_destroy(rain);
    if (mode == MODE_FULLSCREEN) {
        XUngrabKeyboard(g.dpy, CurrentTime);
        XUngrabPointer(g.dpy, CurrentTime);
    }
    XftDrawDestroy(g.draw);
    XftFontClose(g.dpy, g.font);
    if (!g.use_dbe) {
        XFreePixmap(g.dpy, g.pix);
        XFreeGC(g.dpy, g.gc);
    }
    if (g.own_window)
        XDestroyWindow(g.dpy, g.win);
    XCloseDisplay(g.dpy);
    return 0;
}
