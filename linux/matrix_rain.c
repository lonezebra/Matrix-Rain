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
#include <X11/Xft/Xft.h>

#include "rain.h"

#define CELL_W_FACTOR 0.62f
#define CELL_H_FACTOR 1.05f
#define SHADES 24               /* quantized trail brightness levels */
#define HEAD_LEVEL (SHADES + 1) /* pseudo shade level for the head color */
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

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    int width, height;
    int own_window;             /* we created it (fullscreen or -window) */
    Pixmap pix;                 /* persistent back buffer */
    GC gc;                      /* black-foreground GC for fills and blits */
    XftDraw *draw;
    XftFont *font;
    XftColor head;
    XftColor trail[SHADES + 1];
    int cell_w, cell_h, baseline;
    short glyph_xoff[MAX_GLYPHS];  /* centering offset per glyph */
    FT_UInt glyph_idx[MAX_GLYPHS]; /* FT glyph index per glyph (XftCharIndex) */

    /* Dirty-cell state: what the back buffer currently shows per cell.
     * last_lvl is -1 for empty, 1..SHADES for trail shades, HEAD_LEVEL for
     * the head color; last_gly is the glyph table index drawn there. */
    int    cells;               /* cols * rows the arrays are sized for */
    short          *last_lvl;
    unsigned short *last_gly;
    /* Per-frame scratch (sized to cells). */
    XftGlyphFontSpec *specs;    /* dirty glyphs in scan order */
    short            *spec_lvl; /* shade level per entry in specs */
    XftGlyphFontSpec *sorted;   /* specs grouped by shade level */
    XRectangle       *rects;    /* cell rects to clear to black */
} Gfx;

static long bench_glyph_calls;  /* XftDrawGlyphFontSpec calls this frame */
static long bench_dirty_cells;  /* cells repainted this frame */

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
        g->glyph_idx[i] = XftCharIndex(g->dpy, g->font, glyphs[i]);
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

/* (Re)allocate the per-cell dirty-tracking state and per-frame scratch
 * arrays, and mark every cell as "unknown" so the next frame repaints all
 * visible cells. Exits on OOM (a few bytes per screen cell). */
static void reset_cell_state(Gfx *g, int cols, int rows)
{
    int n = cols * rows;
    if (n != g->cells) {
        free(g->last_lvl);
        free(g->last_gly);
        free(g->specs);
        free(g->spec_lvl);
        free(g->sorted);
        free(g->rects);
        g->last_lvl = malloc((size_t)n * sizeof *g->last_lvl);
        g->last_gly = malloc((size_t)n * sizeof *g->last_gly);
        g->specs    = malloc((size_t)n * sizeof *g->specs);
        g->spec_lvl = malloc((size_t)n * sizeof *g->spec_lvl);
        g->sorted   = malloc((size_t)n * sizeof *g->sorted);
        g->rects    = malloc((size_t)n * sizeof *g->rects);
        if (!g->last_lvl || !g->last_gly || !g->specs || !g->spec_lvl ||
            !g->sorted || !g->rects) {
            fprintf(stderr, "matrix-rain: out of memory\n");
            exit(1);
        }
        g->cells = n;
    }
    for (int i = 0; i < n; i++) {
        g->last_lvl[i] = -1;
        g->last_gly[i] = 0;
    }
}

static void free_cell_state(Gfx *g)
{
    free(g->last_lvl);
    free(g->last_gly);
    free(g->specs);
    free(g->spec_lvl);
    free(g->sorted);
    free(g->rects);
}

/* Create the persistent back-buffer Pixmap and clear it to black. */
static void setup_backbuffer(Gfx *g)
{
    g->pix = XCreatePixmap(g->dpy, g->win, (unsigned)g->width,
                           (unsigned)g->height,
                           (unsigned)DefaultDepth(g->dpy, g->screen));
    XGCValues gv;
    gv.foreground = BlackPixel(g->dpy, g->screen);
    gv.graphics_exposures = False;
    g->gc = XCreateGC(g->dpy, g->win, GCForeground | GCGraphicsExposures, &gv);
    XFillRectangle(g->dpy, g->pix, g->gc, 0, 0,
                   (unsigned)g->width, (unsigned)g->height);
    g->draw = XftDrawCreate(g->dpy, g->pix,
                            DefaultVisual(g->dpy, g->screen),
                            DefaultColormap(g->dpy, g->screen));
}

static void resize_backbuffer(Gfx *g)
{
    XFreePixmap(g->dpy, g->pix);
    g->pix = XCreatePixmap(g->dpy, g->win, (unsigned)g->width,
                           (unsigned)g->height,
                           (unsigned)DefaultDepth(g->dpy, g->screen));
    XFillRectangle(g->dpy, g->pix, g->gc, 0, 0,
                   (unsigned)g->width, (unsigned)g->height);
    XftDrawChange(g->draw, g->pix);
}

/* Quantized shade level for a cell: -1 empty, 1..SHADES trail, HEAD_LEVEL
 * head. Levels that would render pure black count as empty. */
static inline int cell_level(float b, int is_head)
{
    if (is_head)
        return HEAD_LEVEL;
    if (b < 0.02f)
        return -1;
    int lvl = (int)(b * SHADES + 0.5f);
    if (lvl > SHADES)
        lvl = SHADES;
    return lvl >= 1 ? lvl : -1;
}

/* Dirty-cell render onto the persistent Pixmap:
 *  1. diff each cell's quantized shade level + glyph against what the back
 *     buffer already shows; unchanged cells are skipped entirely,
 *  2. clear the changed cells' rects to black (one batched fill request),
 *  3. repaint changed visible cells with ONE XftDrawGlyphFontSpec call per
 *     non-empty color bucket (head + each trail shade, <= SHADES+1 calls),
 *  4. blit the full frame to the window (one cheap atomic copy). */
static void render(Gfx *g, const Rain *r)
{
    int ndirty = 0, nrect = 0;

    for (int c = 0; c < r->cols; c++) {
        const float *bright = r->cell_bright + (size_t)c * (size_t)r->rows;
        const unsigned short *gly = r->cell_glyph + (size_t)c * (size_t)r->rows;
        short *last_lvl = g->last_lvl + c * r->rows;
        unsigned short *last_gly = g->last_gly + c * r->rows;
        int head = rain_head_cell(r, c);
        int x = c * g->cell_w;
        for (int y = 0; y < r->rows; y++) {
            int lvl = cell_level(bright[y], y == head);
            unsigned short gi = gly[y];
            if (lvl == last_lvl[y] && (lvl < 0 || gi == last_gly[y]))
                continue;       /* back buffer already shows this cell */
            if (last_lvl[y] >= 0) {         /* erase what was there */
                g->rects[nrect].x = (short)x;
                g->rects[nrect].y = (short)(y * g->cell_h);
                g->rects[nrect].width = (unsigned short)g->cell_w;
                g->rects[nrect].height = (unsigned short)g->cell_h;
                nrect++;
            }
            if (lvl >= 1) {                 /* draw the new glyph */
                g->specs[ndirty].font = g->font;
                g->specs[ndirty].x = (short)(x + g->glyph_xoff[gi]);
                g->specs[ndirty].y = (short)(y * g->cell_h + g->baseline);
                g->specs[ndirty].glyph = g->glyph_idx[gi];
                g->spec_lvl[ndirty] = (short)lvl;
                ndirty++;
            }
            last_lvl[y] = (short)lvl;
            last_gly[y] = gi;
        }
    }

    /* Batched black fill of every changed cell (chunked to stay well under
     * the X protocol's maximum request size). */
    for (int i = 0; i < nrect; i += 4096) {
        int chunk = nrect - i > 4096 ? 4096 : nrect - i;
        XFillRectangles(g->dpy, g->pix, g->gc, g->rects + i, chunk);
    }

    /* Group dirty glyphs by shade level (counting sort), then draw each
     * bucket with batched XftDrawGlyphFontSpec calls.
     *
     * Batches must stay spatially compact: Xft renders glyph specs through
     * XRender CompositeGlyphs with a mask format, and the server rasterizes
     * that via a scratch mask covering the union bounding box of all glyphs
     * in the call. A single call whose glyphs are scattered across the
     * screen therefore costs a full-screen composite (catastrophic on
     * software servers). Specs are emitted in column-major scan order, so
     * spatial neighbors are adjacent in each bucket; greedily merge a glyph
     * into the current chunk only while the chunk's bounding box grows a
     * little, and flush otherwise. Clusters (same-column runs, adjacent
     * columns) share one call; isolated cells get their own tiny call. */
    int count[HEAD_LEVEL + 1];
    memset(count, 0, sizeof count);
    for (int i = 0; i < ndirty; i++)
        count[g->spec_lvl[i]]++;
    int offset[HEAD_LEVEL + 1];
    int acc = 0;
    for (int l = 1; l <= HEAD_LEVEL; l++) {
        offset[l] = acc;
        acc += count[l];
    }
    int fill[HEAD_LEVEL + 1];
    memcpy(fill, offset, sizeof fill);
    for (int i = 0; i < ndirty; i++)
        g->sorted[fill[g->spec_lvl[i]]++] = g->specs[i];

    long cell_area = (long)g->cell_w * g->cell_h;
    long merge_area = 10 * cell_area;   /* max bbox growth per merged glyph */
    long max_area = 160 * cell_area;    /* absolute chunk bbox cap */
    for (int l = 1; l <= HEAD_LEVEL; l++) {
        if (count[l] == 0)
            continue;
        XftColor *color = (l == HEAD_LEVEL) ? &g->head : &g->trail[l];
        const XftGlyphFontSpec *sp = g->sorted + offset[l];
        int nsp = count[l];
        int start = 0;
        int x1 = sp[0].x, x2 = sp[0].x, y1 = sp[0].y, y2 = sp[0].y;
        for (int k = 1; k < nsp; k++) {
            int nx1 = sp[k].x < x1 ? sp[k].x : x1;
            int nx2 = sp[k].x > x2 ? sp[k].x : x2;
            int ny1 = sp[k].y < y1 ? sp[k].y : y1;
            int ny2 = sp[k].y > y2 ? sp[k].y : y2;
            long old_area = (long)(x2 - x1 + g->cell_w) * (y2 - y1 + g->cell_h);
            long new_area = (long)(nx2 - nx1 + g->cell_w) * (ny2 - ny1 + g->cell_h);
            if (new_area - old_area > merge_area || new_area > max_area) {
                XftDrawGlyphFontSpec(g->draw, color, sp + start, k - start);
                bench_glyph_calls++;
                start = k;
                x1 = x2 = sp[k].x;
                y1 = y2 = sp[k].y;
            } else {
                x1 = nx1; x2 = nx2; y1 = ny1; y2 = ny2;
            }
        }
        XftDrawGlyphFontSpec(g->draw, color, sp + start, nsp - start);
        bench_glyph_calls++;
    }
    bench_dirty_cells += ndirty + nrect;

    XCopyArea(g->dpy, g->pix, g->win, g->gc, 0, 0,
              (unsigned)g->width, (unsigned)g->height, 0, 0);
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
    int bench_seconds = 0;      /* hidden: -bench <seconds> */

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
        } else if (!strcmp(a, "-bench")) {
            if (++i >= argc) goto missing_arg;
            bench_seconds = atoi(argv[i]);
            if (bench_seconds < 1) bench_seconds = 1;
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
        if (bench_seconds > 0) {        /* bench at full display resolution */
            g.width = DisplayWidth(g.dpy, g.screen);
            g.height = DisplayHeight(g.dpy, g.screen);
        }
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
    reset_cell_state(&g, cols, rows);
    Rain *rain = rain_create(cols, rows, st.density, st.speed, glyph_count,
                             (unsigned)time(NULL) ^ (unsigned)getpid());
    if (!rain) {
        fprintf(stderr, "matrix-rain: out of memory\n");
        return 1;
    }

    if (bench_seconds > 0) {
        /* Warm up: ~60 simulated seconds so the grid reaches the filled
         * steady state before timing starts. */
        for (int i = 0; i < 1200; i++)
            rain_step(rain, 0.05f);
        /* Uncapped render loop: advance the simulation with real dt, render
         * each frame, measure. XSync makes the server finish each frame so
         * the timing includes server-side raster work. */
        XSync(g.dpy, False);
        double start = now_sec(), last_b = start;
        long frames = 0, total_calls = 0, total_dirty = 0;
        double total_ms = 0.0, max_ms = 0.0;
        while (now_sec() - start < (double)bench_seconds && !quit_flag) {
            while (XPending(g.dpy)) {
                XEvent ev;
                XNextEvent(g.dpy, &ev);
            }
            double t = now_sec();
            float dt = (float)(t - last_b);
            if (dt > 0.1f)
                dt = 0.1f;
            last_b = t;
            rain_step(rain, dt);
            bench_glyph_calls = 0;
            bench_dirty_cells = 0;
            double r0 = now_sec();
            render(&g, rain);
            XSync(g.dpy, False);
            double ms = (now_sec() - r0) * 1000.0;
            total_ms += ms;
            if (ms > max_ms)
                max_ms = ms;
            total_calls += bench_glyph_calls;
            total_dirty += bench_dirty_cells;
            frames++;
        }
        double elapsed = now_sec() - start;
        printf("bench: %dx%d cells=%dx%d  %.1fs\n",
               g.width, g.height, cols, rows, elapsed);
        printf("bench: frames=%ld  fps=%.1f\n",
               frames, (double)frames / elapsed);
        printf("bench: render avg=%.3f ms  max=%.3f ms\n",
               frames ? total_ms / (double)frames : 0.0, max_ms);
        printf("bench: glyph-draw-calls avg=%.1f per frame\n",
               frames ? (double)total_calls / (double)frames : 0.0);
        printf("bench: dirty-cell ops avg=%.1f per frame\n",
               frames ? (double)total_dirty / (double)frames : 0.0);
        rain_destroy(rain);
        free_cell_state(&g);
        XftDrawDestroy(g.draw);
        XftFontClose(g.dpy, g.font);
        XFreePixmap(g.dpy, g.pix);
        XFreeGC(g.dpy, g.gc);
        if (g.own_window)
            XDestroyWindow(g.dpy, g.win);
        XCloseDisplay(g.dpy);
        return 0;
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
                    reset_cell_state(&g, cols, rows); /* full repaint next frame */
                    if (rain_resize(rain, cols, rows) != 0) {
                        fprintf(stderr, "matrix-rain: out of memory\n");
                        running = 0;
                    }
                }
                break;
            case Expose:
                if (ev.xexpose.count == 0)  /* repaint from persistent buffer */
                    XCopyArea(g.dpy, g.pix, g.win, g.gc, 0, 0,
                              (unsigned)g.width, (unsigned)g.height, 0, 0);
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
    free_cell_state(&g);
    XftDrawDestroy(g.draw);
    XftFontClose(g.dpy, g.font);
    XFreePixmap(g.dpy, g.pix);
    XFreeGC(g.dpy, g.gc);
    if (g.own_window)
        XDestroyWindow(g.dpy, g.win);
    XCloseDisplay(g.dpy);
    return 0;
}
