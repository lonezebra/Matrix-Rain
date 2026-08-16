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

    wchar_t       *glyph;    /* cols*rows, indexed [col*rows + row] */
    unsigned char *glyphIdx; /* cols*rows, index into rain_glyph_table() */
    float         *bright;   /* cols*rows, 0.0 .. 1.0 */
    RainStream    *streams;  /* one per column (at most one live stream/column) */
    float         *decay;    /* per column: brightness units shed per second */
    int           *idle;     /* scratch list of idle columns for spawning */

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

/* ---- dirty-cell rendering support (display-independent) -------------------
 *
 * A cell's on-screen appearance is fully determined by its quantized draw
 * *level* and its glyph index. Levels, for a renderer with `shades` trail
 * ramp steps (ramp step 0 = black .. step shades-1 = full rain color):
 *
 *   0            empty (pure black — includes brightness that quantizes to
 *                ramp step 0, i.e. black-on-black, so dying cells fold
 *                straight to empty instead of redrawing an invisible glyph)
 *   1..shades    trail: ramp step (level - 1)
 *   shades + 1   head color
 *
 * A cell *code* packs both: code = level * rain_glyph_count() + glyphIdx,
 * with code 0 meaning "empty" (glyph forced to 0 so all empties compare
 * equal). Codes are what a renderer caches as its last-drawn state.
 */

/* The shared glyph table (kana + digits + symbols) and its length. */
const wchar_t *rain_glyph_table(int *count);
int            rain_glyph_count(void);

/* Quantized code for one cell. headRow = rain_head_cell(s, col) (pass it in
 * so per-column head lookup is not repeated per row). */
int rain_cell_code(const RainSim *s, int col, int row, int headRow,
                   int shades);

/* Diff the sim's current quantized state against lastCodes (cols*rows
 * entries, indexed [col*rows + row]). Every cell whose code differs is
 * appended to dirtyIdx (capacity cols*rows) and lastCodes is updated to the
 * current code. Returns the number of dirty cells. Initialize lastCodes to
 * all 0 after clearing the target surface to black ("everything drawn
 * empty"), or to -1 to force a full repaint. Cells whose float brightness
 * moved but whose quantized code did not are NOT reported. */
int rain_diff(const RainSim *s, int shades, int *lastCodes, int *dirtyIdx);

#endif
