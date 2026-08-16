/* rain.c — Matrix rain simulation, no display dependencies. */
#include "rain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned int xorshift32(unsigned int *s)
{
    unsigned int x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9e3779b9u;
    return *s;
}

static float frand(Rain *r)                          /* [0,1) */
{
    return (float)(xorshift32(&r->rng) >> 8) / 16777216.0f;
}

static float frange(Rain *r, float lo, float hi)
{
    return lo + (hi - lo) * frand(r);
}

static void free_grid(Rain *r)
{
    free(r->streams);
    free(r->cell_bright);
    free(r->cell_glyph);
    free(r->col_decay);
    r->streams = NULL;
    r->cell_bright = NULL;
    r->cell_glyph = NULL;
    r->col_decay = NULL;
}

static int alloc_grid(Rain *r, int cols, int rows)
{
    size_t n = (size_t)cols * (size_t)rows;
    r->streams     = calloc((size_t)cols, sizeof *r->streams);
    r->cell_bright = calloc(n, sizeof *r->cell_bright);
    r->cell_glyph  = calloc(n, sizeof *r->cell_glyph);
    r->col_decay   = calloc((size_t)cols, sizeof *r->col_decay);
    if (!r->streams || !r->cell_bright || !r->cell_glyph || !r->col_decay) {
        free_grid(r);
        return -1;
    }
    r->cols = cols;
    r->rows = rows;
    r->active_count = 0;
    return 0;
}

Rain *rain_create(int cols, int rows, float density, float speed,
                  int glyph_count, unsigned int seed)
{
    if (cols < 1 || rows < 1 || glyph_count < 1)
        return NULL;
    Rain *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->density = density;
    r->speed = speed;
    r->glyph_count = glyph_count;
    r->rng = seed ? seed : 1u;
    if (alloc_grid(r, cols, rows) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

void rain_destroy(Rain *r)
{
    if (!r)
        return;
    free_grid(r);
    free(r);
}

int rain_resize(Rain *r, int cols, int rows)
{
    if (cols < 1 || rows < 1)
        return -1;
    if (cols == r->cols && rows == r->rows)
        return 0;
    free_grid(r);
    return alloc_grid(r, cols, rows);
}

static void maybe_spawn(Rain *r, float dt)
{
    if ((float)r->active_count >= r->density * (float)r->cols)
        return;
    float spawn_rate = 3.0f * (float)r->cols * r->density / (float)r->rows;
    if (frand(r) >= 1.0f - expf(-dt * spawn_rate))
        return;
    int idle = r->cols - r->active_count;
    if (idle <= 0)
        return;
    int k = (int)(frand(r) * (float)idle);
    if (k >= idle)
        k = idle - 1;
    int c = -1;
    for (int i = 0; i < r->cols; i++) {
        if (!r->streams[i].active && k-- == 0) {
            c = i;
            break;
        }
    }
    if (c < 0)
        return;
    RainStream *s = &r->streams[c];
    s->active = 1;
    s->speed = RAIN_BASE_SPEED * r->speed * frange(r, 0.6f, 1.4f);
    s->length = frange(r, 0.35f, 0.95f) * (float)r->rows;
    if (s->length < 1.0f)
        s->length = 1.0f;
    s->head_row = -frange(r, 0.0f, (float)r->rows);
    s->prev_cell = (int)floorf(s->head_row);
    r->col_decay[c] = s->speed / s->length;
    r->active_count++;
    r->stat_spawned++;
}

void rain_step(Rain *r, float dt)
{
    if (dt <= 0.0f)
        return;

    /* Fade trails: decay rate is speed/length of the column's stream, so a
     * cell goes 1.0 -> 0.0 in exactly `length` cells behind the head. */
    for (int c = 0; c < r->cols; c++) {
        float d = r->col_decay[c] * dt;
        if (d <= 0.0f)
            continue;
        float *col = r->cell_bright + (size_t)c * (size_t)r->rows;
        for (int y = 0; y < r->rows; y++) {
            if (col[y] > 0.0f) {
                col[y] -= d;
                if (col[y] < 0.0f)
                    col[y] = 0.0f;
            }
        }
    }

    /* Advance heads; stamp every row crossed this frame. */
    for (int c = 0; c < r->cols; c++) {
        RainStream *s = &r->streams[c];
        if (!s->active)
            continue;
        s->head_row += s->speed * dt;
        int cell = (int)floorf(s->head_row);
        float *col = r->cell_bright + (size_t)c * (size_t)r->rows;
        unsigned short *gly = r->cell_glyph + (size_t)c * (size_t)r->rows;
        for (int y = s->prev_cell + 1; y <= cell; y++) {
            if (y >= 0 && y < r->rows) {
                col[y] = 1.0f;
                gly[y] = (unsigned short)(xorshift32(&r->rng) % (unsigned)r->glyph_count);
            }
        }
        s->prev_cell = cell;
        if (s->head_row - s->length > (float)r->rows) {
            s->active = 0;
            r->active_count--;
            r->stat_died++;
        }
    }

    /* In-place glyph mutation on visible cells. */
    float p = 1.0f - expf(-dt * RAIN_MUTATION_RATE);
    size_t n = (size_t)r->cols * (size_t)r->rows;
    for (size_t i = 0; i < n; i++) {
        if (r->cell_bright[i] > 0.0f && frand(r) < p) {
            r->cell_glyph[i] = (unsigned short)(xorshift32(&r->rng) % (unsigned)r->glyph_count);
            r->stat_mutations++;
        }
    }

    maybe_spawn(r, dt);
}

int rain_head_cell(const Rain *r, int c)
{
    const RainStream *s = &r->streams[c];
    if (!s->active)
        return -1;
    int cell = (int)floorf(s->head_row);
    return (cell >= 0 && cell < r->rows) ? cell : -1;
}
