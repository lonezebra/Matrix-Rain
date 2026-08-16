/* Matrix Rain — display-independent simulation core (see SPEC.md section 1). */
#ifndef RAIN_H
#define RAIN_H

#include <wchar.h>

#define RAIN_BASE_SPEED    10.0f   /* rows/second at speed = 1.0 */
#define RAIN_MUTATION_RATE 0.025f  /* glyph swaps per visible cell per second */

typedef struct RainStream {
    int   active;
    float headRow;  /* fractional row of the bright head */
    float speed;    /* rows/second */
    float length;   /* trail length in cells */
} RainStream;

typedef struct RainSim {
    int   cols, rows;
    float density;   /* 0.05 .. 1.0 */
    float speedMul;  /* 0.5 .. 3.0 */

    wchar_t    *glyph;    /* cols*rows, indexed [col*rows + row] */
    float      *bright;   /* cols*rows, 0.0 .. 1.0 */
    RainStream *streams;  /* one per column (at most one live stream/column) */
    float      *decay;    /* per column: brightness units shed per second */
    int        *idle;     /* scratch list of idle columns for spawning */

    unsigned      rng;
    unsigned long spawned; /* lifetime spawn counter (stats/testing) */
} RainSim;

/* Returns 0 on success, -1 on allocation failure. */
int  rain_init(RainSim *s, int cols, int rows, float density, float speedMul,
               unsigned seed);
void rain_free(RainSim *s);
void rain_step(RainSim *s, float dt);

/* Row of the head glyph in a column, or -1 if no head is on screen there. */
int  rain_head_cell(const RainSim *s, int col);

/* Number of currently active streams / cells with brightness > 0. */
int  rain_active_streams(const RainSim *s);
int  rain_lit_cells(const RainSim *s);

#endif
