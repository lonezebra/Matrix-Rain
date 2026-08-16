/* Matrix Rain simulation core. Pure C11, no platform dependencies. */
#include "rain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Glyph set (SPEC.md): half-width katakana + digits + a few latin/symbols. */
#define KANA_FIRST 0xFF71  /* U+FF71 HALFWIDTH KATAKANA LETTER A  */
#define KANA_LAST  0xFF9D  /* U+FF9D HALFWIDTH KATAKANA LETTER N  */
#define KANA_COUNT (KANA_LAST - KANA_FIRST + 1)

static const wchar_t EXTRA_GLYPHS[] =
    L"0123456789Z:\u30FB.\"=*+-<>\u00A6\uFF5C"; /* ・ ¦ ｜ */

static wchar_t g_glyphs[KANA_COUNT + sizeof(EXTRA_GLYPHS) / sizeof(wchar_t)];
static int     g_glyphCount;

static void ensure_glyphs(void)
{
    int n = 0;
    if (g_glyphCount)
        return;
    for (wchar_t ch = KANA_FIRST; ch <= KANA_LAST; ch++)
        g_glyphs[n++] = ch;
    for (int i = 0; EXTRA_GLYPHS[i]; i++)
        g_glyphs[n++] = EXTRA_GLYPHS[i];
    g_glyphCount = n;
}

/* xorshift32 — small, fast, deterministic per seed. */
static unsigned xr(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static float frand(unsigned *s)               /* [0, 1) */
{
    return (float)(xr(s) >> 8) / 16777216.0f;
}

static float frange(unsigned *s, float a, float b)
{
    return a + (b - a) * frand(s);
}

static wchar_t random_glyph(unsigned *s)
{
    return g_glyphs[xr(s) % (unsigned)g_glyphCount];
}

static int ifloor(float x)
{
    return (int)floorf(x);
}

static void spawn_stream(RainSim *s, int col)
{
    RainStream *st = &s->streams[col];

    st->active  = 1;
    st->speed   = RAIN_BASE_SPEED * s->speedMul * frange(&s->rng, 0.6f, 1.4f);
    st->length  = frange(&s->rng, 0.35f, 0.95f) * (float)s->rows;
    if (st->length < 2.0f)
        st->length = 2.0f;
    st->headRow = -frange(&s->rng, 0.0f, (float)s->rows);
    /* Linear fade over `length` cells at `speed` rows/s => this decay rate. */
    s->decay[col] = st->speed / st->length;
    s->spawned++;
}

int rain_init(RainSim *s, int cols, int rows, float density, float speedMul,
              unsigned seed)
{
    ensure_glyphs();
    memset(s, 0, sizeof(*s));

    if (cols < 1)
        cols = 1;
    if (rows < 2)
        rows = 2;
    s->cols     = cols;
    s->rows     = rows;
    s->density  = density;
    s->speedMul = speedMul;
    s->rng      = seed ? seed : 0x9E3779B9u;

    s->glyph   = calloc((size_t)cols * rows, sizeof(wchar_t));
    s->bright  = calloc((size_t)cols * rows, sizeof(float));
    s->streams = calloc((size_t)cols, sizeof(RainStream));
    s->decay   = calloc((size_t)cols, sizeof(float));
    s->idle    = calloc((size_t)cols, sizeof(int));
    if (!s->glyph || !s->bright || !s->streams || !s->decay || !s->idle) {
        rain_free(s);
        return -1;
    }

    /* Pre-seed to target density so the screen fills immediately; heads all
     * start above the top edge, so streams still enter staggered. */
    {
        int target = (int)(density * (float)cols);
        for (int i = 0; i < target; i++) {
            int col = (int)(xr(&s->rng) % (unsigned)cols);
            if (!s->streams[col].active)
                spawn_stream(s, col);
        }
    }
    return 0;
}

void rain_free(RainSim *s)
{
    free(s->glyph);
    free(s->bright);
    free(s->streams);
    free(s->decay);
    free(s->idle);
    memset(s, 0, sizeof(*s));
}

void rain_step(RainSim *s, float dt)
{
    int active = 0;

    if (!s->glyph || dt <= 0.0f)
        return;
    if (dt > 0.25f)  /* clamp huge pauses (debugger, suspend) */
        dt = 0.25f;

    for (int c = 0; c < s->cols; c++) {
        float      *colB = &s->bright[(size_t)c * s->rows];
        wchar_t    *colG = &s->glyph[(size_t)c * s->rows];
        RainStream *st   = &s->streams[c];
        float       shed = s->decay[c] * dt;

        /* Trail fade. */
        if (shed > 0.0f) {
            for (int r = 0; r < s->rows; r++) {
                if (colB[r] > 0.0f) {
                    colB[r] -= shed;
                    if (colB[r] < 0.0f)
                        colB[r] = 0.0f;
                }
            }
        }

        /* Head advance; every newly entered cell gets a fresh glyph at full
         * brightness (this is also what changes the head character per row). */
        if (st->active) {
            int prev = ifloor(st->headRow);
            int cur;

            st->headRow += st->speed * dt;
            cur = ifloor(st->headRow);
            for (int r = prev + 1; r <= cur; r++) {
                if (r >= 0 && r < s->rows) {
                    colG[r] = random_glyph(&s->rng);
                    colB[r] = 1.0f;
                }
            }
            if (st->headRow - st->length > (float)s->rows)
                st->active = 0;
            else
                active++;
        }
    }

    /* In-place glyph mutation on visible cells. */
    {
        float p = RAIN_MUTATION_RATE * dt;
        int   n = s->cols * s->rows;
        for (int i = 0; i < n; i++) {
            if (s->bright[i] > 0.0f && frand(&s->rng) < p)
                s->glyph[i] = random_glyph(&s->rng);
        }
    }

    /* Spawn at most one stream per frame, per SPEC section 2. */
    if ((float)active < s->density * (float)s->cols) {
        float rate = 3.0f * (float)s->cols * s->density / (float)s->rows;
        if (frand(&s->rng) < 1.0f - expf(-dt * rate)) {
            int n = 0;
            for (int c = 0; c < s->cols; c++) {
                if (!s->streams[c].active)
                    s->idle[n++] = c;
            }
            if (n > 0)
                spawn_stream(s, s->idle[xr(&s->rng) % (unsigned)n]);
        }
    }
}

int rain_head_cell(const RainSim *s, int col)
{
    const RainStream *st = &s->streams[col];
    int               r;

    if (!st->active)
        return -1;
    r = ifloor(st->headRow);
    return (r >= 0 && r < s->rows) ? r : -1;
}

int rain_active_streams(const RainSim *s)
{
    int n = 0;
    for (int c = 0; c < s->cols; c++)
        n += s->streams[c].active ? 1 : 0;
    return n;
}

int rain_lit_cells(const RainSim *s)
{
    int n     = 0;
    int total = s->cols * s->rows;
    for (int i = 0; i < total; i++)
        n += (s->bright[i] > 0.0f) ? 1 : 0;
    return n;
}
