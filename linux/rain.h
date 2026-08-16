/* rain.h — display-independent Matrix rain simulation core (see SPEC.md §1-2). */
#ifndef RAIN_H
#define RAIN_H

#include <stddef.h>

#define RAIN_BASE_SPEED    10.0f   /* rows/second before multipliers */
#define RAIN_MUTATION_RATE 0.025f  /* glyph mutations per visible cell per second */

#define RAIN_DENSITY_MIN 0.05f
#define RAIN_DENSITY_MAX 1.0f
#define RAIN_DENSITY_DEF 0.75f
#define RAIN_SPEED_MIN   0.5f
#define RAIN_SPEED_MAX   3.0f
#define RAIN_SPEED_DEF   1.0f
#define RAIN_SIZE_MIN    8
#define RAIN_SIZE_MAX    64
#define RAIN_SIZE_DEF    18

typedef struct {
    float head_row;   /* fractional row of the bright head glyph */
    float speed;      /* rows/second */
    float length;     /* trail length in cells */
    int   active;
    int   prev_cell;  /* last integer row stamped by the head */
} RainStream;

typedef struct {
    int   cols, rows;
    float density;              /* 0.05 .. 1.0 */
    float speed;                /* 0.5 .. 3.0 multiplier */
    int   glyph_count;          /* indices in cell_glyph are < glyph_count */
    int   active_count;
    RainStream     *streams;    /* one slot per column */
    float          *cell_bright;/* cols*rows, column-major, 0..1 */
    unsigned short *cell_glyph; /* cols*rows, column-major */
    float          *col_decay;  /* per-column brightness decay rate, 1/s */
    unsigned int    rng;
    long stat_spawned, stat_died, stat_mutations;
} Rain;

Rain *rain_create(int cols, int rows, float density, float speed,
                  int glyph_count, unsigned int seed);
void  rain_destroy(Rain *r);
int   rain_resize(Rain *r, int cols, int rows);   /* rebuilds grid; 0 on success */
void  rain_step(Rain *r, float dt);
/* On-screen head cell row for column c, or -1 if none. */
int   rain_head_cell(const Rain *r, int c);

#endif
