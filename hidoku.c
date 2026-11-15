/* See "Code ends here, the rest is comments" at the bottom of this file if
 * you want a few pointers before diving into the code. */

/* ----------------------------------------------------------------------
 * Development macros
 * ---------------------------------------------------------------------- */

#define TODO (assert(!"todo")) /* { ...; TODO; ...; } */

#define GENERATOR_SOLVERS 6 /* 2 disables recursive clue shrinking */

/* Enable these to run thorough but slow checks during solver and generator
 * execution.  Great for debugging; disable these in release builds. */

/* #define CHECK_SOLVER_INVARIANT */
/* #define CHECK_SUFFICIENT_CLUES_INVARIANT */
/* #define CHECK_PICK_MINIMAL_CLUE_SET_POSTCONDITION */

/* Enable this to see performance measurements on stdout (and also a partial
 * trace of much of the generator's internal state and solver's progress). */

/* #define TIMING */

/* Print all `game_ui` fields (etc.) on every call to `interpret_move` */

/* #define TRACE_UI */

/* Enabling STANDALONE_SOLVER makes my editor/LSP work well with standalone
 * solver code.  Enabling PRINT_FULL_SOLVER_STATE helps debug the solver. */

/* #define STANDALONE_SOLVER */
/* #define PRINT_FULL_SOLVER_STATE */

/* Do `assert(f(...));` and `f() { require(...); return true; }`, then you can
 * choose whether to get assertion failures inside f or at the call site. */
#define FINE_GRAINED_FAILURES
#if defined(FINE_GRAINED_FAILURES)
#define REQUIRE(b) do assert(b); while (false)
#else
#define REQUIRE(b) do if (!(b)) return false; while (false)
#endif

/* ----------------------------------------------------------------------
 * Headers
 * ---------------------------------------------------------------------- */

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __STDC__VERSION__ < 199900L /* makes this compile with -std=c89 (gcc) */
#define inline
#endif

#include "puzzles.h"

#if defined(TIMING)

#if !defined(__USE_POSIX199309)
#define __USE_POSIX199309 1
#endif

#include <time.h>

/* capture the current time with nanosecond precision */
static struct timespec now()
{
    /* https://linuxvox.com/blog/c-linux-equivalent-of-windows-queryperformancecounter/ */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return now;
}

/* print out the time elapsed between `before` and `after` */
static void print_time_delta(struct timespec before,
                             struct timespec after)
{
    const bool wrapped = (after.tv_nsec < before.tv_nsec);
    struct timespec delta;
    delta.tv_sec = after.tv_sec - before.tv_sec - wrapped;
    delta.tv_nsec = (after.tv_nsec - before.tv_nsec) + wrapped * 1000000000L;
    printf("[%02ld:%09ld] ", delta.tv_sec, delta.tv_nsec);
}

#define TIMER_START(name)                              \
    {                                                  \
        const struct timespec before_ ## name = now()

#define TIMER_PRINT(name, block)                                \
        {                                                       \
            const struct timespec after_ ## name = now();       \
            print_time_delta(before_ ## name, after_ ## name);  \
            block;                                              \
        }                                                       \
    }

#else
#define TIMER_START(name)
#define TIMER_PRINT(name, block)
#endif

/* ----------------------------------------------------------------------
 * Types and typedefs
 * ---------------------------------------------------------------------- */

/* "Show me your flowcharts and conceal your [data definitions], and I shall
 * continue to be mystified. Show me your [data definitions], and I won’t
 * usually need your flowcharts; they’ll be obvious." --Fred Brooks */

/* shared generation/play parameters: `.w` and `.h` and `.neighbours` */
struct rules {
    int w;          /* the width of the grid */
    int h;          /* the height of the grid */
    int neighbours; /* the number of neighbours of each non-boundary square */
};

/* has `.rules` and `.symmetry_index` and `clue_1_and_n` */
struct game_params {
    struct rules rules; /* has `.w` and `.h` and `.neighbours` */
    int symmetry_index; /* indexes into `symmetry_choices` */
    bool clue_1_and_n;  /* give `1` and `n` as clues */
};

/* has `.params` and `.name` */
struct game_preset {
    struct game_params params; /* preset parameters */
    char *name;                /* preset menu item text */
};

/* Difference vector from a square to a neighbour.  Has `.dx` and `.dy` */
struct dxy {
    int dx; /* horizontal (column) offset to neighbour */
    int dy; /* vertical (row) offset to neighbour */
};

/* a pair of 4-bit unsigned integers (see "Essay: Solver") */
typedef unsigned char counters;

/* has `.w` and `.n` and `.neighbours` and `.possible` and `.solved_pairs`
 * and `.possible_values_in_square` and `.possible_squares_for_value` */
struct solver {
    int w;          /* the width of the grid */
    int n;          /* the number of squares in the grid */
    int neighbours; /* the number of neighbours of each non-boundary square */

    /* `.solved_pairs` counts proven index/value0 pairings.  This equals:
     *  - how many `k` satisify `.possible_values_in_square[k] <= 1`; and
     *  - how many `k` satisify `.possible_squares_for_value[k] <= 1`. */
    int solved_pairs;

    /* `.possible[idx(index, value0, .n)]` is:
     * - zero if `value0` is impossible in square `index`.
     * - non-zero otherwise, but not an arbitrary non-zero value:
     *   - the value consists of two 4-bit unsigned counters
     *   - the low 4 bits count how many neighbouring squares could
     *     contain `value1 - 1` (it's always `bogus` for `value1 = 1`).
     *   - the high 4 bits count how many neighbouring squares could
     *     contain `value1 + 1` (it's always `bogus` for `value1 = n`).
     */
    counters *possible;

    /* `.possible_values_in_square[index]` counts how many values of `v`
     * satisfy `.possible[idx(index, v, .n)] != 0`. */
    int *possible_values_in_square;

    /* `.possible_squares_for_value[value0]` counts how many values of `i`
     * satisfy `.possible[idx(i, value0, .n)] != 0`. */
    int *possible_squares_for_value;
};

/* Representation of candidate clues, for use by the generator. Has
 * `.indices` and `.offsets` and `.classes` and `.sufficient` and
 * `.estimated_first_sufficient`. */
struct clues {
    int *indices; /* indices of squares, grouped into symmetry classes */
    int *offsets; /* starting index in `.indices` of each class, then `n` */
    int *classes; /* unique class handles; each indexes into `.offsets` */
    int sufficient; /* cluing `.classes[0..(.sufficent-1)]` => solvable */
    int estimated_first_sufficient; /* guess at first `.sufficient < n` */
};

/* recursive solver forks into two with `.value0` at/not-at `.index` */
struct index_value_pair {
    int index;  /* the square where the recursive solver gueses a value */
    int value0; /* the value guessed by the recursive solver */
};

/* a link in a list of recursive solver guesses (signed index/value pairs) */
struct guesses {
    int guess; /* `1 + idx(index, value0, solver->n)`, negated if excluded */
    struct guesses *prev; /* the previous guess-link */
};

/* I define two incompatible `square` types to force use of the interface. */
#if !defined(STANDALONE_SOLVER)

/* an opaque type representing contents of a square */
typedef int square;

/* create a `square` based on its `int` representation */
static square mk_square(int s) { return s; }

#define PRIVATE(s) (s)

#else

/* an opaque type representing contents of a square */
typedef struct { int impl; } square;

/* create a `square` based on its `int` representation */
static square mk_square(int s) {
    square t;
    t.impl = s;
    return t;
}

#define PRIVATE(s) ((s).impl)

#endif

/* an empty square */
static square mk_empty()           { return mk_square(0); }

/* a square with the given 1-based clue value */
static square mk_clue1(int value1) { return mk_square(+value1); }

/* a square with the given 1-based user-supplied value */
static square mk_user1(int value1) { return mk_square(-value1); }

/* true iff the given square is empty */
static bool is_empty(square s)     { return PRIVATE(s) == 0; }

/* true iff the given square contains a clue */
static bool is_clue(square s)      { return 0 < PRIVATE(s);  }

/* true iff the given square contains a user-supplied alue */
static bool is_user(square s)      { return PRIVATE(s) < 0;  }

/* the 1-based value in the given square, or 0 if empty */
static int get_value1(square s) {
    const int t = PRIVATE(s);
    return t < 0 ? -t : t;
}

/* the 0-based value in the given square */
static int get_value0(square s)    { return get_value1(s) - 1; }

#undef PRIVATE

/* has `.rules` and `.squares` and `.completed` and `.used_solver` */
struct game_state {
    struct rules rules; /* has `.w` and `.h` and `.neighbours` */
    square *squares;    /* the grid (in row major order) */
    bool completed;     /* true iff the instance was ever solved */
    bool used_solver;   /* true iff the solver was ever used */
};

/* has fields:
 * `.dragged_squares`
 * `.dragged_length`
 * `.x`
 * `.y`
 * `.last_mouse_x`
 * `.last_mouse_y`
 * `.number`
 * `.visible`
 * `.drag_increasing`
 * `.is_mouse_dragging`
 * `.click_canceled`
 */
struct game_ui {
    int *dragged_squares;    /* squares on the drag curve */
    int dragged_length;      /* number of squares on the drag curve */
    int x;                   /* the column of the cursor */
    int y;                   /* the row of the cursor */
    int last_mouse_x;        /* the column of the last mouse event */
    int last_mouse_y;        /* the row of the last mouse event */
    int number;              /* the number being entered, or `0` if none. */
    bool visible;            /* true iff the cursor is shown */
    bool drag_increasing;    /* true iff drag numbers are ascending */
    bool is_mouse_dragging;  /* true iff drag curve is mouse-driven */
    bool click_canceled;     /* true iff keyboard move started after click */
};

/* Bitmask; has bits `1 << (0..7)` and `highlight_error` and `cursor` and
 * `clue` and `user` and `entering` and `diminished` and `dragging` */
typedef unsigned short drawflag;

/* has `.tilesize` and `.{immediate,retained}_flags` and `.overlay_squares`
 * and `.retained_numbers` */
struct game_drawstate {
    int tilesize;              /* raw size parameter (picked by midend) */
    drawflag *immediate_flags; /* current flags (computed on each redraw) */
    drawflag *retained_flags;  /* previous flags (remembered and updated) */
    square *overlay_squares;   /* current grid numbers plus pending move */
    int *retained_numbers;     /* previous grid numbers (updated on redraw) */
};

/* render a text grid cell given `(w, h, x, y, maxdigits, context, output)` */
typedef void (*cell_renderer)(int, int, int, int, int, const void *, char *);

/* ----------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

/* `exclude(solver, index, value0)`: rule out `value0` in square `index`.
 * Recursively rule out all other possibilities which are inconsistent with
 * the updated solver state.  Requires `value0` to be possible at `index`. */
static void exclude(struct solver *solver, int index, int value0);

/* ----------------------------------------------------------------------
 * Global variables
 * ---------------------------------------------------------------------- */

#if defined(STANDALONE_SOLVER)
static bool verbose_standalone_solver = false;
static bool last_output_was_print_solver = true;
#endif

/* ----------------------------------------------------------------------
 * Enumerations and global constants
 * ---------------------------------------------------------------------- */

/* flag & (1 << i): draw line segment in direction i, where directions are
 * taken from neighbour_dxy[i], for 0 <= i < 8 */

enum {
    highlight_error = 1 << 8, /* number is wrong, something must be deleted */
    cursor = 1 << 9,          /* keyboard/mouse cursor is here */
    clue = 1 << 10,           /* square contains a clue */
    user = 1 << 11,           /* square was filled by the user */
    entering = 1 << 12,       /* a number is being typed here */
    diminished = 1 << 13,     /* `x` has `x-1` and `x+1` among neighbours */
    dragging = 1 << 14        /* square is part of the ui drag curve */
};

/* preset menu items */
static struct game_preset hidoku_presets[] = {
    /* I never know which parameters to select as presets.  According to
     * https://en.wikipedia.org/wiki/Hidato, Marilyn Vos Savant has run a
     * daily Numbrix column in Parade magazine using sizes 7x7 and 9x9, see
     * https://parade.com/2596/marilynvossavant/play-todays-numbrix/
     *
     * I have found at least one puzzle with n not given as a clue.
     *
     * On https://playhidoku.com/ the default is 6x6, with 1 and n;
     * this is with the 8-neighbour rule.
     *
     * The designs of Vos Savant tend to be four-fold rotationally symmetric
     * which I don't support.  I found no symmetry on playhidoku.com.
     *
     * So let's give people approximately what they might be used to.
     */
    {{{7, 7, 4}, 3, false}, "7x7 Numbrix (4 neighbours)"},
    {{{9, 9, 4}, 4, false}, "9x9 Numbrix (4 neighbours)"},
    {{{6, 6, 8}, 0, true},  "6x6 Hidoku (8 neighbours)"},
};

enum {
    horizontal_mirroring = 1 << 0, /* `(x, y) <-> (w-1 - x, y)` */
    vertical_mirrroring = 1 << 1,  /* `(x, y) <-> (x, h-1 - y)` */
    two_way_rotation = 1 << 2,     /* `(x, y) <-> (w-1 - x, h-1 - y)` */
    /* clue `[(c, r) for c in [x, w-1 - x] for r in [y, h-1 - y]]` */
    four_way_symmetry = (horizontal_mirroring |
                         vertical_mirrroring |
                         two_way_rotation)
};

#define MAP_SYMMETRIES(F)                                               \
    F(0, ":None")                                                       \
    F(horizontal_mirroring, ":Horizontal mirroring")                    \
    F(vertical_mirrroring, ":Vertical mirroring")                       \
    F(two_way_rotation, ":Two-way rotation")                            \
    F(four_way_symmetry, ":Four-way symmetry")

#define PROJECT_VALUE(value, label) (value),
#define PROJECT_CHOICE(value, label) label

/* map symmetry index parameter to symmetry bit mask */
static const int symmetry_choices[] = { MAP_SYMMETRIES(PROJECT_VALUE) };

/* `.dx` and `.dy` are deltas to each neighbouring square */
static const struct dxy neighbour_dxy[8] = {
    {-1, 0},
    {+1, 0},

    {0, -1},
    {0, +1},

    {-1, -1},
    {+1, +1},

    {-1, +1},
    {+1, -1},
};

enum {numeric_parse_error = -1}; /* custom parameter input was not a number */
enum {bogus = 15}; /* fake neighbour count in out-of-bounds solver data */

enum {
    multiple = -2,       /* multiple values or solutions are possible */
    none = -1,           /* no value or solution is found or possible */
    unique_solution = 0  /* (recursive solver only) exactly one solution */
};

enum {
    COL_BACKGROUND,
    COL_CURSOR,
    COL_GRID,
    COL_CLUE,
    COL_ENTERING,
    COL_USER,
    COL_ERROR,
    COL_DIMINISH_CLUE,
    COL_DIMINISH_USER,
    NCOLOURS
};

static const float flash_duration = 0.7; /* `0.7s` feels good to me */

/* ----------------------------------------------------------------------
 * Macros and miscellaneous helper methods
 * ---------------------------------------------------------------------- */

#define SNEWA(ptr, len) ((ptr) = smalloc((len) * sizeof *(ptr)))
#define COPY(d, s, n) memcpy((d), (s), n * sizeof *(s))
#define DUPMEM(p, n) COPY(smalloc((n) * sizeof *(p)), (p), (n))
#define SHALLOW_CLONE(ptr) (DUPMEM((ptr), 1))

#define MAX_SIZE 46340 /* no overflow: 46340**2 < 2**31 < 46341**2 */

/* `sprintf` into a newly allocated buffer of size `n+1` (with `'\0'`).
 *
 * It is the callers responsibility to not overrun the buffer.  This may be
 * impossible to ensure as the length of the string depends on locale and may
 * be difficult to predict, according to my `sprintf` man page.  */
static char *string(int n, const char *fmt, ...)
{
    va_list va;
    char *ret;
    int m;
    va_start(va, fmt);
    m = vsprintf(SNEWA(ret, n + 1), fmt, va);
    va_end(va);
    if (n < m) fatal("memory corruption");
    return ret;
}

/* map 2d array indices `(x, y, w)` into the 1d array index `x + (y * w)` */
static int idx(int x, int y, int w)
{
    return x + y * w;
}

/* return `true` iff `x` is outside `[0..w)` or `y` is outside `[0..h)` */
static bool out_of_bounds(int x, int y, int w, int h)
{
    return x < 0 || w <= x || y < 0 || h <= y;
}

/* ----------------------------------------------------------------------
 * Parameters
 * ---------------------------------------------------------------------- */

/* map `0` to `4` and `1` to `8`, the inverse of `neighbours_to_index` */
static int index_to_neighbours(int index) {
    return (index + 1) * 4;
}

/* map `4` to `0` and `8` to `1`, the inverse of `index_to_neighbours` */
static int neighbours_to_index(int neighbours) {
    return (neighbours / 4) - 1;
}

/* parse the first number in `*string_pointer`, advancing the pointer past it.
 * Return the value of the shortest prefix exceeding `limit`, if any. */
static int parse_number(const char **string_pointer, int limit)
{
    int result;

    assert(string_pointer);
    assert(*string_pointer);

    for (result = 0; isdigit(**string_pointer); ++(*string_pointer)) {
        result = 10 * result + (**string_pointer - '0');
        if (limit < result) return result; /* to prevent integer overflow */
    }

    return result;
}

/* parse the first number in `*(++string_pointer)`, like `parse_number` */
static int skip_and_parse_number(const char **string_pointer, int limit)
{
    assert(string_pointer);
    assert(*string_pointer);
    ++(*string_pointer);
    return parse_number(string_pointer, limit);
}

/* advance `string_pointer` past all occurrences of `' '` */
static void skip_spaces(const char **string_pointer)
{
    assert(string_pointer);
    assert(*string_pointer);
    while (**string_pointer == ' ') ++(*string_pointer);
}

/* parse a user-supplied numeric parameter value */
static int parse_numeric_parameter(const char *input_string, int limit)
{
    int result = 0;

    assert(input_string);

    skip_spaces(&input_string);
    result = parse_number(&input_string, limit);
    skip_spaces(&input_string);

    return *input_string ? numeric_parse_error : result;
}

/* return a newly allocated copy of the default parameter preset */
static struct game_params *default_params(void)
{
    return SHALLOW_CLONE(&hidoku_presets[0].params);
}

/* return a newly allocated copy of `params` */
static struct game_params *dup_params(const struct game_params *params)
{
    return SHALLOW_CLONE(params);
}

/* deallocate `params` and all allocations inside it */
static void free_params(struct game_params *params)
{
    sfree(params);
}

/* If `!full`: `"${width}x${height}e${neighbours-index}"`. If `full`: that,
 * plus `"s${symmetry-index}c${0-or-1:clue-1-and-n}` */
static char *encode_params(const struct game_params *params, bool full)
{
    char buffer[256];
    int n = sprintf(buffer,
                    "%dx%de%d",
                    params->rules.w,
                    params->rules.h,
                    neighbours_to_index(params->rules.neighbours));
    assert(n < lenof(buffer));

    if (full) {
        n += sprintf(buffer + n, "s%dc%d",
                     params->symmetry_index,
                     (int) params->clue_1_and_n);
        assert(n < lenof(buffer));
    }

    /* If we ever add another symmetry type or neighbour count (or more
     * generally, edge set), its number should be one larger than the largest
     * number currently in use: the next edge set gets the number `2` and the
     * next symmetry type gets the number `lenof(symmetry_choices)`.  It
     * should be appended to the list of symmetries.
     *
     * This way, parameters encoded by version n of the program will have
     * their meaning preserved when decoded by version n+k.
     *
     * I could definitely encode the edge set in terms of the neighbour count
     * but maybe there's more than one way of having, say, 5 neighbours.  By
     * numbering the meanings I'm treating them as opaque and thus not baking
     * any assumptions about them into the encoding.
     */

    return dupstr(buffer);
}

/* parse `string` into `params`; see `encode_params` for encoding format */
static void decode_params(struct game_params *params, const char *string)
{
    const int limit = 255; /* completely arbitrary, but big enough */
    params->clue_1_and_n = false;
    params->symmetry_index = 0;

    params->rules.w = params->rules.h = parse_number(&string, MAX_SIZE);
    while (*string) {
        if (*string == 'x') {
            params->rules.h = skip_and_parse_number(&string, MAX_SIZE);
        } else if (*string == 'e') {
            const int neighbour_index = skip_and_parse_number(&string, limit);
            params->rules.neighbours = index_to_neighbours(neighbour_index);
        } else if (*string == 'c') {
            const int boolean_int = skip_and_parse_number(&string, limit);
            params->clue_1_and_n = (boolean_int != 0);
        } else if (*string == 's') {
            const int symmetry = skip_and_parse_number(&string, limit);
            if (0 <= symmetry && symmetry < lenof(symmetry_choices))
                params->symmetry_index = symmetry;
            else
                params->symmetry_index = -1;
        } else
            ++string; /* skip unknown character */
    }
}

/* write a heap copy of preset no. `preset` into `*name` and `*params` */
static bool game_fetch_preset(int preset, char **name,
                              struct game_params **params)
{
    if (0 <= preset && preset < lenof(hidoku_presets)) {
        *params = SHALLOW_CLONE(&hidoku_presets[preset].params);
        *name = dupstr(hidoku_presets[preset].name);
        return true;
    } else {
        return false;
    }
}

/* append a `config_item` to `array`, advancing the pointer */
static void config(config_item **array, char *name,
                   int type, int ival, char *sval)
{
    config_item *element = (*array)++;
    element->name = name;
    element->type = type;

    if (type == C_STRING) {
        element->u.string.sval = sval;
    } else if (type == C_BOOLEAN) {
        element->u.boolean.bval = (ival != 0);
    } else if (type == C_CHOICES) {
        element->u.choices.selected = ival;
        element->u.choices.choicenames = sval;
    } else if (type == C_END) {
        /* do nothing */
    } else {
        assert(false);
    }
}

/* set up custom parameter dialog widgets */
static struct config_item *game_configure(const struct game_params *params)
{
    const int n_items = 6;
    config_item *result = snewn(n_items, config_item), *p = result;

    char *width = string(20, "%d", params->rules.w);
    char *height = string(20, "%d", params->rules.h);

    const int e = neighbours_to_index(params->rules.neighbours);
    char *variants = ":Numbrix (4 neighbours):Hidoku (8 neighbours)";

    const int s = params->symmetry_index;
    char *symmetry_types = (MAP_SYMMETRIES(PROJECT_CHOICE));

    const int c = params->clue_1_and_n;

    config(&p, "Width",                        C_STRING,   0, width);
    config(&p, "Height",                       C_STRING,   0, height);
    config(&p, "Variant",                      C_CHOICES,  e, variants);
    config(&p, "Clue symmetry",                C_CHOICES,  s, symmetry_types);
    config(&p, "Always give 1 and n as clues", C_BOOLEAN,  c, NULL);
    config(&p, NULL,                           C_END,      0, NULL);

    assert(p - result == n_items);

    return result;
}

/* parse user-supplied parameter values */
static game_params *custom_params(const struct config_item *cfg)
{
    struct game_params *params = snew(game_params);

    params->rules.w = parse_numeric_parameter(cfg[0].u.string.sval, MAX_SIZE);
    params->rules.h = parse_numeric_parameter(cfg[1].u.string.sval, MAX_SIZE);
    params->rules.neighbours = index_to_neighbours(cfg[2].u.choices.selected);

    assert(0 <= cfg[3].u.choices.selected);
    assert(cfg[3].u.choices.selected < lenof(symmetry_choices));
    params->symmetry_index = cfg[3].u.choices.selected;

    params->clue_1_and_n = cfg[4].u.boolean.bval;

    return params;
}

/* rule out a few small grids with unsatisfiable constraints */
static const char *validate_params_full(const struct game_params *params,
                                        int symmetry)
{
    const int n = params->rules.w * params->rules.h;

    if (n < 3 && params->clue_1_and_n) {
        return "Giving 1 and n as clues requires at least 3 squares";
    }

    if (params->rules.h == 1) {
        if (params->rules.w == 2 && (symmetry & ~vertical_mirrroring))
            /* we must give either all clues or no clues */
            return ("It is impossible to generate unambiguously solvable"
                    " symmetric 2x1 puzzles (except for the do-nothing"
                    " vertical symmetry)");
        return NULL;
    }

    if (params->rules.w == 1) {
        if (params->rules.h == 2 && (symmetry & ~horizontal_mirroring))
            /* we must give either all clues or no clues */
            return ("It is impossible to generate unambiguously solvable"
                    " symmetric 1x2 puzzles (except for the do-nothing"
                    " horizontal symmetry)");
        return NULL;
    }

    assert(2 <= params->rules.w);
    assert(2 <= params->rules.h);
    assert(4 <= n);

    if (params->rules.neighbours == 4) {
        if (n == 4 && (symmetry & two_way_rotation)) {
            assert(2 == params->rules.w);
            assert(2 == params->rules.h);
            /* Clues are given one diagonal at a time.  If we give exactly one
             * diagonal the other diagonal is ambiguous.  Otherwise we give
             * either all clues or no clues. */
            return ("It is impossible to generate unambiguously solvable"
                    " rotationally symmetric 2x2 puzzles with 4 neighbours");
        }
        return NULL;
    }

    assert(params->rules.neighbours == 8);

    if (!symmetry) return NULL;

    if (params->rules.w == 2 && params->rules.h == 2) {
        /* if symmetry == four_way_symmetry we must give either all clues or
         * no clues.  Otherwise, if we empty two squares containing x and y
         * there's a second solution if we swap x and y. */
        return ("It is impossible to generate unambiguously solvable"
                " symmetric 2x2 puzzles with 8 neighbours");
    }

    if (params->rules.w == 2 && (symmetry & horizontal_mirroring))
        /* If at least one row is empty we can find a second solution by
         * swapping its two numbers. */
        return ("It is impossible to generate unambiguously solvable"
                " horizontally symmetric 2xH puzzles with 8 neighbours");

    if (params->rules.h == 2 && (symmetry & vertical_mirrroring))
        /* If at least one column is empty we can find a second solution by
         * swapping its two numbers. */
        return ("It is impossible to generate unambiguously solvable"
                " vertically symmetric Wx2 puzzles with 8 neighbours");

    /* See "Correctness of instance generation and `validate_params_full`"
     * at the bottom of this file for a statement and proof of correctness */
    return NULL;
}

/* return an error message (static storage, e.g. literal) or `NULL`.
 * validate generation-time parameters (symmetries etc.) if `full` */
static const char *validate_params(const struct game_params *params,
                                   bool full)
{
    assert(numeric_parse_error < 1);
    /* ^ then parse errors result in "must be a positive integer [...]" */

    if (params->rules.w < 1 || MAX_SIZE < params->rules.w)
        return "Width must be an integer between 1 and " STR(MAX_SIZE);

    if (params->rules.h < 1 || MAX_SIZE < params->rules.h)
        return "Height must be an integer between 1 and " STR(MAX_SIZE);

    if (MAX_SIZE < (params->rules.w * params->rules.h))
        return "The number of squares must be at most " STR(MAX_SIZE);

    if (params->rules.neighbours != 4 && params->rules.neighbours != 8)
        return "The number of neighbours must be 4 or 8";

    assert(params->rules.neighbours <= lenof(neighbour_dxy));

    if (!full) return NULL;

    if (params->symmetry_index == -1) {
        return "Invalid symmetry choice";
    } else {
        const int symmetry = symmetry_choices[params->symmetry_index];
        return validate_params_full(params, symmetry);
    }
}

/* ----------------------------------------------------------------------
 * Game state: definition and basic operations (incl. game_text_format)
 * ---------------------------------------------------------------------- */

/* create a `game_state` with `squares` allocated but uninitialized */
static struct game_state *create_blank_game(const struct game_params *params)
{
    const int n = params->rules.w * params->rules.h;
    struct game_state *state = snew(game_state);
    state->rules = params->rules; /* struct copy */
    state->squares = snewn(n, square);
    state->completed = false;
    state->used_solver = false;
    return state;
}

/* return a newly allocated copy of `state` */
static struct game_state *dup_game(const struct game_state *state)
{
    const int n = state->rules.w * state->rules.h;
    struct game_state *clone = SHALLOW_CLONE(state);
    clone->squares = DUPMEM(state->squares, n);
    return clone;
}

/* deallocate `state` and all allocations inside it */
static void free_game(struct game_state *state)
{
    sfree(state->squares);
    sfree(state);
}

/* return `true` when parameters allow for stringification (i.e. always) */
static bool game_can_format_as_text_now(const struct game_params *params)
{
    return true;
}

/* `digits(n)`: count the digits in `n` */
static int digits(int n)
{
    int d;
    for (d = 1; n /= 10; ++d);
    return d;
}

/* stringify `value1` into `output[i]` where `1 <= i <= maxdigits + 1` */
static void render_number_in_cell(char *output, int maxdigits, int value1)
{
    if (1 <= value1) {
        char *p = output + 1 + maxdigits; /* right justified */
        do {
            *(p--) = '0' + (value1 % 10);
            assert(output < p);
        } while (value1 /= 10);
    }
}

/* stringify one grid cell in a stringified game state */
static void render_state_cell(int w, int h, int x, int y, int maxdigits,
                              const void *void_game_state, char *cell)
{
    const struct game_state *state = void_game_state;
    const int index = idx(x, y, w);
    const int value1 = get_value1(state->squares[index]);
    render_number_in_cell(cell, maxdigits, value1);
}

/* stringify a game state or solver using the callback for each grid cell */
static char *stringify_grid(int w, int h, int n, const void *context,
                            cell_renderer render_cell)
{
    const int maxdigits = digits(n); /* n is upper bound on cell numbers */
    const int cellwidth = maxdigits + 3; /* "| %d ": 3 chars plus number */
    const int cellheight = 2; /* 1 for top border, 1 for cell contents */
    const int rowlength = (w * cellwidth) + 2; /* + 2 for "+\n" or "|\n" */
    const int rows = h * cellheight + 1; /* + 1 for bottom border */
    const int length = rows * rowlength + 1; /* + 1 for '\0' */

    char *result = snewn(length, char);
    int x, y;

    /* blank out everything */
    memset(result, ' ', length);

    /* render the top two rows */
    memset(result, '-', rowlength);
    for (x = 0; x <= w; ++x) { /* "<=": w+1 iterations */
        result[x * cellwidth + 0]         = '+';  /* +0: even row */
        result[x * cellwidth + rowlength] = '|';  /* +rowlength: odd row */
    }
    result[w * cellwidth + 1 + 0]         = '\n';
    result[w * cellwidth + 1 + rowlength] = '\n';

    /* copy them to subsequent rows */
    for (y = 1; y < h; ++y) { /* "y=1": h-1 iterations */
        memcpy(result + y * 2 * rowlength, result, 2 * rowlength);
    }
    memcpy(result + h * 2 * rowlength, result, rowlength); /* bottom border */

    /* fill in each of the cells */
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const int base = rowlength * (y * cellheight + 1) + x * cellwidth;
            render_cell(w, h, x, y, maxdigits, context, result + base);
        }
    }
    result[length - 1] = '\0';

    /* I only draw one '|' in the left boundary of each cell (and one at
     * the end of each row), but I should really draw `cellheight - 1`. */
    assert(cellheight == 2);

    return result;
}

/* return newly allocated string representation of `state` */
static char *game_text_format(const struct game_state *state)
{
    const int w = state->rules.w, h = state->rules.h, n = w * h;
    return stringify_grid(w, h, n, state, render_state_cell);
}

/* ----------------------------------------------------------------------
 * Solver       (See "Essay: Solver" towards the bottom of this file)
 * ---------------------------------------------------------------------- */

/* `possible(solver, index, value0)` iff `value0` not ruled out at `index` */
static bool possible(const struct solver *solver, int index, int value0)
{
    return solver->possible[idx(index, value0, solver->n)] != 0;
}

/* render value1@index if no other value or index is possible */
static void render_solver_cell(int w, int h, int x, int y, int maxdigits,
                               const void *void_solver, char *cell)
{
    const struct solver *solver = void_solver;
    const int index = idx(x, y, w);
    int value0 = solver->n;

    if (solver->possible_values_in_square[index] == 1)
        for (value0 = 0; value0 < solver->n; ++value0)
            if (solver->possible[idx(index, value0, solver->n)]) {
                render_number_in_cell(cell, maxdigits, value0 + 1);
                return;
            }

    for (value0 = 0; value0 < solver->n; ++value0)
        if (solver->possible_squares_for_value[value0] == 1)
            if (solver->possible[idx(index, value0, solver->n)])
                render_number_in_cell(cell, maxdigits, value0 + 1);

}

/* return newly allocated string representation of `solver` */
static char *stringify_solver(const struct solver *solver)
{
    const int n = solver->n, w = solver->w, h = n / w;
    return stringify_grid(w, h, n, solver, render_solver_cell);
}

/* print the solver grid (and `.possible`, if `PRINT_FULL_SOLVER_STATE`) */
static void print_solver(const struct solver *solver)
{
    char *string = stringify_solver(solver);
    fputs(string, stdout);
    sfree(string);

#if defined(PRINT_FULL_SOLVER_STATE)
    {
        int index, value0;
        for (index = 0; index < solver->n; ++index) {
            printf("square (%d, %d), %d numbers:",
                   index / solver->w + 1,
                   index % solver->w + 1,
                   solver->possible_values_in_square[index]);
            for (value0 = 0; value0 < solver->n; ++value0) {
                if (possible(solver, index, value0))
                    printf(" %d", value0 + 1);
            }
            puts("");
        }

        for (value0 = 0; value0 < solver->n; ++value0) {
            printf("value %d has %d squares:",
                   value0 + 1,
                   solver->possible_squares_for_value[value0]);
            for (index = 0; index < solver->n; ++index) {
                if (possible(solver, index, value0))
                    printf(" (%d,%d)",
                           index / solver->w + 1,
                           index % solver->w + 1);
            }
            puts("");
        }
    }
#endif
}

/* Allocate a solver, initialize its non-pointer members and allocate memory
 * for its pointers.  Use `initialize_solver` to (re-)initialize the memory at
 * the pointers.  (Together we form one whole from-scratch constructor.) */
static struct solver *new_generator_solver(const struct rules *rules)
{
    struct solver *solver = snew(struct solver);
    solver->w = rules->w;
    solver->n = rules->h * rules->w;
    solver->neighbours = rules->neighbours;
    solver->solved_pairs = 0;

    /* caller (or someone) must initialize the contents of these arrays */
    SNEWA(solver->possible, solver->n * solver->n);
    SNEWA(solver->possible_squares_for_value, solver->n);
    SNEWA(solver->possible_values_in_square, solver->n);

    return solver;
}

/* Copy all mutable data from `src` into `dst`.  This includes the data at all
 * pointer members and all fields mutated after solver construction, but
 * excludes the fields copied or derived from the initial `struct rules`. */
static void copy_solver_state(struct solver *dst, const struct solver *src)
{
    const int n = src->n;
    dst->solved_pairs = src->solved_pairs;
    COPY(dst->possible, (const counters *) src->possible, n * n);
    COPY(dst->possible_squares_for_value, src->possible_squares_for_value, n);
    COPY(dst->possible_values_in_square, src->possible_values_in_square, n);
}

/* return a copy of `solver` with all memory freshly allocated */
static struct solver *dup_solver(const struct solver *solver)
{
    struct solver *clone = SHALLOW_CLONE(solver);

    clone->possible =
        DUPMEM(solver->possible, solver->n * solver->n);

    clone->possible_squares_for_value =
        DUPMEM(solver->possible_squares_for_value, solver->n);

    clone->possible_values_in_square =
        DUPMEM(solver->possible_values_in_square, solver->n);

    return clone;
}

/* deallocate `solver` and all allocations inside it */
static void free_solver(struct solver *solver)
{
    sfree(solver->possible_squares_for_value);
    sfree(solver->possible_values_in_square);
    sfree(solver->possible);
    sfree(solver);
}

/* return true iff `solver` has deduced that no solutions are possible */
static bool no_solutions(const struct solver *solver)
{
    /* if some square has no possible values or vice versa then `exclude`
     * zeroes out everything, see "Essay: Solver". */
    return solver->possible_values_in_square[0] == 0;
}

/* return true iff `solver` has deduced that any solution is unique */
static bool at_most_one_solution(const struct solver *solver)
{
    /* We increment `solved_pairs` when the number of possible values in a
     * square goes to 1.  We don't change it if the number of possible values
     * drops further (i.e. to 0), so counting all the way up to n only tells
     * us that we have _at most_ one solution, not _exactly_ one.
     *
     * You may want to use this function together with `no_solutions`.
     */
    return solver->solved_pairs == solver->n;
}

#if !defined(NDEBUG)
/* return `true` iff `solver` is in a solved state. */
static bool is_solved(const struct solver *solver)
{
    return at_most_one_solution(solver) && !no_solutions(solver);
}

#if defined(CHECK_SOLVER_INVARIANT)
/* count the neighbours of `(x, y)` in which the value is possible */
static unsigned char count_neighbours_with_value(const struct solver *solver,
                                                 int x, int y, int value0)
{
    const int w = solver->w, n = solver->n, h = n / w;
    unsigned char count = 0;
    int direction;

    if (value0 < 0 || n <= value0)
        return bogus;

    for (direction = 0; direction < solver->neighbours; ++direction) {
        const int nx = x + neighbour_dxy[direction].dx;
        const int ny = y + neighbour_dxy[direction].dy;
        if (!out_of_bounds(nx, ny, w, h)) {
            const int neighbour = idx(nx, ny, w);
            if (possible(solver, neighbour, value0))
                ++count;
        }
    }

    return count;
}
#endif

/* return `false` iff we detect a violation in the state of `solver` */
static bool solver_invariant(const struct solver *solver)
{
    /* While `exclude` is on the call stack the invariant may be temporarily
     * violated.  Hence don't call this from `exclude` or its descendants. */
#if defined(CHECK_SOLVER_INVARIANT)
    const int w = solver->w, n = solver->n, h = n / w;

    int solved_values = 0, solved_squares = 0, x, y, index, value0;

    for (value0 = 0; value0 < n; ++value0) {
        int count = 0, index;
        for (index = 0; index < n; ++index)
            if (possible(solver, index, value0))
                ++count;
        REQUIRE(solver->possible_squares_for_value[value0] == count);
        if (count <= 1) ++solved_values;
    }

    REQUIRE(solver->solved_pairs == solved_values);

    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const int index = idx(x, y, w);
            int count = 0;
            for (value0 = 0; value0 < n; ++value0) {
                if (possible(solver, index, value0)) {
                    const counters predecessors =
                        count_neighbours_with_value(solver, x, y, value0 - 1);
                    const counters successors =
                        count_neighbours_with_value(solver, x, y, value0 + 1);
                    const counters c = predecessors + 16 * successors;

                    REQUIRE(solver->possible[idx(index, value0, n)] == c);

                    ++count;
                }
            }
            REQUIRE(solver->possible_values_in_square[index] == count);
            if (count <= 1) ++solved_squares;
        }
    }

    REQUIRE(solver->solved_pairs == solved_squares);

    if (at_most_one_solution(solver)) {
        const int target = no_solutions(solver) ? 0 : 1;
        for (index = 0; index < n; ++index)
            REQUIRE(solver->possible_values_in_square[index] == target);
        for (value0 = 0; value0 < n; ++value0)
            REQUIRE(solver->possible_squares_for_value[value0] == target);
    } else {
        bool some_ambiguity;
        REQUIRE(!no_solutions(solver));

        some_ambiguity = false;
        for (index = 0; index < n; ++index) {
            const int count = solver->possible_values_in_square[index];
            REQUIRE(1 <= count);
            some_ambiguity = some_ambiguity || (1 < count);
        }
        REQUIRE(some_ambiguity);

        some_ambiguity = false;
        for (value0 = 0; value0 < n; ++value0) {
            const int count = solver->possible_squares_for_value[value0];
            REQUIRE(1 <= count);
            some_ambiguity = some_ambiguity || (1 < count);
        }
        REQUIRE(some_ambiguity);
    }
#endif

    return true;
}
#endif

/* count how many neighbouring squares of `idx(x, y, w)` are in-bounds */
static counters count_neighbours(int x, int y, int w, int h, int neighbours)
{
    counters count = 0;
    int direction;

    for (direction = 0; direction < neighbours; ++direction) {
        const int nx = x + neighbour_dxy[direction].dx;
        const int ny = y + neighbour_dxy[direction].dy;
        if (!out_of_bounds(nx, ny, w, h)) ++count;
    }

    return count;
}

/* initialize `solver->possible[idx(index, value0, solver->n)]` based
 * on the number of valid neighbouring placements of `value0 - 1` and
 * `value0 + 1` (given in `predecessors` and `successors`). */
static void initialize_possible(struct solver *solver,
                                int index,
                                int value0,
                                unsigned char predecessors,
                                unsigned char successors)
{
    const counters counter_pair = successors * 16 + predecessors;
    solver->possible[idx(index, value0, solver->n)] = counter_pair;
}

/* represent every number as possible in every square */
static void initialize_solver(struct solver *solver)
{
    const int w = solver->w, n = solver->n, h = n / w;
    int i, x, y;

    /* we decrement counters when losing a successor or predecessor neighbour.
     * When counters reach 1 we check that successor and predecessor don't
     * have to go in the same square.  This check makes no sense for 1 and n
     * so we skip it by setting their out-of-range counters to `bogus`. */

    for (i = 0; i < n; ++i) {
        solver->possible_squares_for_value[i] = n;
        solver->possible_values_in_square[i] = n;
    }

    if (n == 1) {
        solver->solved_pairs = 1;
        initialize_possible(solver, 0, 0, bogus, bogus);
    } else {
        const int nbrs = solver->neighbours;
        solver->solved_pairs = 0;
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                const counters count = count_neighbours(x, y, w, h, nbrs);
                const int index = idx(x, y, w);
                int value0;
                initialize_possible(solver, index, 1 - 1, bogus, count);
                initialize_possible(solver, index, n - 1, count, bogus);
                for (value0 = 1; value0 < n - 1; ++value0)
                    initialize_possible(solver, index, value0, count, count);
            }
        }
    }

    assert(solver_invariant(solver));
}

/* let `value0` be minimal such that `possible(solver, index, value0)`;
 * exclude `value0` at square `i` for all `i != index` */
static void learn_contents_of_square(struct solver *solver, int index)
{
    int value0, other_index;

    for (value0 = 0; value0 < solver->n; ++value0)
        if (possible(solver, index, value0))
            break;

    assert(value0 < solver->n); /* See "Essay: Solver" */
    if (solver->possible_squares_for_value[value0] < 2) return;

#if defined(STANDALONE_SOLVER)
    if (verbose_standalone_solver) {
        if (last_output_was_print_solver) putchar('\n');
        printf("placed %d in (%d, %d): no other value is possible there\n",
               value0 + 1,
               index / solver->w + 1,
               index % solver->w + 1);
        print_solver(solver);
        last_output_was_print_solver = true;
    }
#endif

    for (other_index = 0; other_index < solver->n; ++other_index)
        if (other_index != index && possible(solver, other_index, value0))
            exclude(solver, other_index, value0);
}

/* let `index` be minimal such that `possible(solver, index, value0)`;
 * exclude `v` at square `index` for all `v != value0` */
static void learn_location_of_value(struct solver *solver, int value0)
{
    int index, other_value;

    for (index = 0; index < solver->n; ++index)
        if (possible(solver, index, value0))
            break;

    if (index == solver->n) return; /* See "Essay: Solver" */
    if (solver->possible_values_in_square[index] < 2) return;

#if defined(STANDALONE_SOLVER)
    if (verbose_standalone_solver) {
        if (last_output_was_print_solver) putchar('\n');
        printf("placed %d in (%d, %d): value not possible anywhere else\n",
               value0 + 1,
               index / solver->w + 1,
               index % solver->w + 1);
        print_solver(solver);
        last_output_was_print_solver = true;
    }
#endif

    for (other_value = 0; other_value < solver->n; ++other_value)
        if (other_value != value0 && possible(solver, index, other_value))
            exclude(solver, index, other_value);
}

/* return the smallest index `i` into `neighbour_d[xy]` where `value0`
 * is possible as the `i`th neighbour of `idx(x, y, solver->w)` */
static int neighbour_at(const struct solver *solver, int x, int y, int value0)
{
    const int w = solver->w, n = solver->n, h = n / w;
    int direction;

    for (direction = 0; direction < solver->neighbours; ++direction) {
        const int nx = x + neighbour_dxy[direction].dx;
        const int ny = y + neighbour_dxy[direction].dy;
        if (out_of_bounds(nx, ny, w, h)) continue;
        if (possible(solver, idx(nx, ny, w), value0)) return direction;
    }

    return -1;
}

/* exclude `value0` at `idx(x, y, solver->w)` if the predecessor and
 * successor of `value0` must go in the same square */
static void check_adjacency_overlap(struct solver *solver,
                                    int x, int y, int value0)
{
    const int pred = neighbour_at(solver, x, y, value0 - 1);
    const int succ = neighbour_at(solver, x, y, value0 + 1);
    if (pred == succ || pred == -1 || succ == -1) {
        const int index = idx(x, y, solver->w);
        exclude(solver, index, value0);
    }
}

/* `exclude(solver, index, value0)`: rule out `value0` in square `index`.
 * Recursively rule out all other possibilities which are inconsistent with
 * the updated solver state.  Requires `value0` to be possible at `index`. */
static void exclude(struct solver *solver, int index, int value0)
/* See "Essay: Solver" towards the bottom of this file */
{
    const int w = solver->w;
    const int n = solver->n;
    const int h = n / w;
    const int x = index % w;
    const int y = index / w;
    int direction;

    assert(possible(solver, index, value0));
    solver->possible[idx(index, value0, solver->n)] = 0;
    assert(!possible(solver, index, value0));

    if (--solver->possible_values_in_square[index] == 1) {
        ++solver->solved_pairs;
        learn_contents_of_square(solver, index);
    }

    if (--solver->possible_squares_for_value[value0] == 1)
        learn_location_of_value(solver, value0);

    for (direction = 0; direction < solver->neighbours; ++direction) {
        const int nx = x + neighbour_dxy[direction].dx;
        const int ny = y + neighbour_dxy[direction].dy;

        if (!out_of_bounds(nx, ny, w, h)) {
            const int adj = idx(nx, ny, w);
            const int predecessor = value0 - 1, successor = value0 + 1;

            if (0 <= predecessor && possible(solver, adj, predecessor)) {
                /* my predecessor at `adj` lost a neighbouring successor */
                const int pidx = idx(adj, predecessor, n);
                const int remaining = (solver->possible[pidx] -= 0x10);
                if ((remaining & 0xf0) == 0) {
                    exclude(solver, adj, predecessor);
                } else if (remaining == 0x11) {
                    check_adjacency_overlap(solver, nx, ny, predecessor);
                }
            }

            if (successor < n && possible(solver, adj, successor)) {
                /* my successor at `adj` lost a neighbouring predecessor */
                const int sidx = idx(adj, successor, n);
                const int remaining = (solver->possible[sidx] -= 0x01);
                if ((remaining & 0x0f) == 0) {
                    exclude(solver, adj, successor);
                } else if (remaining == 0x11) {
                    check_adjacency_overlap(solver, nx, ny, successor);
                }
            }
        }
    }
}

/* `learn_clue(solver, index, value0)`: place `value0` at `index` */
static void learn_clue(struct solver *solver, int index, int value0)
{
    int other_index;
    for (other_index = 0; other_index < solver->n; ++other_index) {
        if (other_index != index && possible(solver, other_index, value0)) {
            exclude(solver, other_index, value0);
            assert(solver_invariant(solver));
        }
    }
}

/* exclude each number `value0` from each square `index` if:
 * - `value0` is a clue given somewhere other than `index`; or
 * - `index` contains a clue different from `value0`
 */
static void learn_all_clues(struct solver *solver,
                            const struct game_state *state)
{
    int index;
    for (index = 0; index < solver->n; ++index) {
        const square square = state->squares[index];
        if (is_clue(square)) {
            const int value0 = get_value0(square);

#if defined(STANDALONE_SOLVER)
            if (verbose_standalone_solver) {
                printf("\nlearning clue %d at (%d, %d)\n",
                       value0 + 1,
                       index / solver->w + 1,
                       index % solver->w + 1);
                last_output_was_print_solver = false;
            }
#endif

            learn_clue(solver, index, value0);

#if defined(STANDALONE_SOLVER)
            if (verbose_standalone_solver) {
                printf("learned clue %d at (%d, %d)\n",
                       value0 + 1, /* get_value1(square), */
                       index / solver->w + 1,
                       index % solver->w + 1);
            }
#endif
        }
    }
}

/* allocate and initialize a new solver instance; learn all clues */
static struct solver *new_recursive_solver(const struct game_state *state)
{
    struct solver *solver = new_generator_solver(&state->rules);
    initialize_solver(solver);
    learn_all_clues(solver, state);
    return solver;
}

/* count the branch points, i.e. elements of the linked list `link` */
static int count_guesses(const struct guesses *link)
{
    int count;
    for (count = 0; link != NULL; link = link->prev) ++count;
    return count;
}

/* return most constrained ambiguous `{index, value}` pair to split on */
static struct index_value_pair pick_ambiguity(const struct solver *solver)
{
    /* A common technique is to find the most constrained square, then pick a
     * value to either place or eliminate there.  Instead we find the most
     * constrained value and then place/eliminate it in one of its squares.
     *
     * This is because the most constrained value is usually more constrained
     * than the most constrained square.
     *
     * Consider a contrived example: an empty grid save for the clue '1'.  The
     * most constrained number (2) is possible in `neighbours` squares; the
     * most constrained square has a number of possibilities equal to the
     * number of different path lengths from '1' to it.
     *
     * But actually, if the most constrained square is more constrained than
     * the most constrained value we pick it (and one of its values).
     *
     * For most constrained values we break ties in favor of v over w if the
     * squares that can contain v can contain fewer values in total than the
     * squares that can contain w.  Rationale: if we rule out v we are closest
     * to determining the value that goes in whatever square we chose.
     *
     * Among the squares that can contain v we pick the one which can contain
     * the most values.  Rationale: this balances the search effort.
     */

    struct index_value_pair result = {none, none};
    int index, value0;
    int best_score = solver->n + 1;
    int best_tiebreaker = (solver->n * solver->n) + 1;

    /* find the most constrained value; ties go to constrained squares */
    for (value0 = 0; value0 < solver->n; ++value0) {
        const int score = solver->possible_squares_for_value[value0];
        assert(score); /* caller promises this */
        if (1 < score && score <= best_score) {
            int max_possibilities = -1, tiebreaker = 0, chosen_index = none;
            for (index = 0; index < solver->n; ++index) {
                if (possible(solver, index, value0)) {
                    const int k = solver->possible_values_in_square[index];
                    tiebreaker += k;
                    if (max_possibilities < k) {
                        max_possibilities = k;
                        chosen_index = index;
                    }
                }
            }
            assert(chosen_index != none);
            if (score < best_score || tiebreaker < best_tiebreaker) {
                result.index = chosen_index;
                result.value0 = value0;
                best_score = score;
                best_tiebreaker = tiebreaker;
            }
        }
    }

    /* ... or pick the most constrained square, if it's more constrained than
     * the most constrained value (happens rarely, but sometimes) */
    for (index = 0; index < solver->n; ++index) {
        const int score = solver->possible_values_in_square[index];
        assert(score); /* caller promises this */
        if (1 < score && score <= best_score) {
            int max_possibilities = -1, tiebreaker = 0, chosen_value0 = none;
            for (value0 = 0; value0 < solver->n; ++value0) {
                if (possible(solver, index, value0)) {
                    const int k = solver->possible_squares_for_value[value0];
                    tiebreaker += k;
                    if (max_possibilities < k) {
                        max_possibilities = k;
                        chosen_value0 = value0;
                    }
                }
            }
            assert(chosen_value0 != none);
            if (score < best_score || tiebreaker < best_tiebreaker) {
                result.index = index;
                result.value0 = chosen_value0;
                best_score = score;
                best_tiebreaker = tiebreaker;
            }
        }
    }

    assert(result.index != none);
    assert(result.value0 != none);

    return result;
}

#define TIMER_STOP5(name, text1, text2, s0, s1, s2plus)         \
    TIMER_PRINT(                                                \
        name,                                                   \
        printf("%*sguess (%d,%d) " text1 " %d" text2 "%s\n",    \
               2 * count_guesses(prev),                         \
               "",                                              \
               split.index / solver->w + 1,                     \
               split.index % solver->w + 1,                     \
               split.value0 + 1,                                \
               solution_count == none ? s0 :                    \
               solution_count == unique_solution ? s1 :         \
               s2plus));

#define TIMER_STOP2(name, text1, text2)                 \
    TIMER_STOP5(name, text1, text2, "0", "1", "2+")

#define TIMER_STOP1(name, text1)                \
    TIMER_STOP5(name, text1, "", "", "", "")

/* return one of: `none` or `unique_solution` or `multiple`
 *
 *  - `solver` will contain the first solution we find (if any).
 *    If necessary it points to new addresses (and the old are freed).
 *  - `solved_already` counts how many solutions we've seen so far (`0..1`).
 *  - `prev` (initially `NULL`) tracks the solver's guesses.
 *  - if `trace` is not `NULL` then `*trace` will be reallocated to point to a
 *    record of guesses leading to the last solution (i.e. the second one if
 *    there are multiple solutions, else the first).
 *  - if `trace` is not `NULL` and there is at least one solution, then
 *    `*trace` will be reallocated to an array containing:
 *     - at `[0]`, two less than the length of the array;
 *     - at `[1]`, the index at which the two solutions diverge;
 *     - at `[2..[0]-1]`, the guesses made leading to a solution.
 *    A guess is `1 + idx(index, value0, n)` when we put `value0` at `index`,
 *    or the negation of that if we exclude `value0` at `index`.
 */
static int solve_recursively(struct solver *solver,
                             bool solved_already,
                             struct guesses *prev,
                             int **trace)
{
    int solution_count; /* `none` or `unique_solution` or `multiple` */
    struct solver *duplicate;
    struct index_value_pair split; /* bifurcate on ambiguous index+value */
    struct guesses link; /* linked list in stack frames; goes into `trace` */

    if (no_solutions(solver)) {
        return none;
    } else if (at_most_one_solution(solver)) {
        if (trace) {
            const int count = count_guesses(prev);
            int index = count + 2;
            *trace = srealloc(*trace, (count + 2) * sizeof **trace);
            (*trace)[0] = (*trace)[1] = count;
            while (prev != NULL) {
                (*trace)[--index] = prev->guess;
                prev = prev->prev;
            }
            assert(index == 2);
        }

        return unique_solution;
    }

    TIMER_START(solver);

    TIMER_START(learn1);
    split = pick_ambiguity(solver);
    link.guess = 1 + idx(split.index, split.value0, solver->n);
    link.prev = prev;

    /* split the universe in two: with/without that value at that index */
    duplicate = dup_solver(solver); /* must now make sure to free this */
    learn_clue(solver, split.index, split.value0);
    TIMER_STOP1(learn1, "contains");

    TIMER_START(recurse1);
    solution_count = solve_recursively(solver, solved_already, &link, trace);
    TIMER_STOP2(recurse1, "contains", ": ");

    if (solution_count == none) {
        TIMER_START(learn2);
        exclude(duplicate, split.index, split.value0);
        assert(solver_invariant(duplicate));
        link.guess = -link.guess; /* (.guess < 0 ? exclude : learn_clue) */
        TIMER_STOP1(learn2, "without");

        TIMER_START(recurse2);
        solution_count = /* 0 + x = x */
            solve_recursively(duplicate, solved_already, &link, trace);
        TIMER_STOP2(recurse2, "without", ": 0 -> ");

        if (solution_count != none) {
            struct solver swap = *duplicate; /* struct copy */
            *duplicate = *solver;            /* struct copy */
            *solver = swap;                  /* struct copy */
        }
    } else if (solution_count == multiple) {
        /* many + x = many so don't even calculate x */
    } else if (solved_already) {
        assert(solution_count == unique_solution);
        solution_count = multiple; /* 1 + 1 = many */
    } else {
        assert(solution_count == unique_solution);
        TIMER_START(learn3);
        exclude(duplicate, split.index, split.value0);
        assert(solver_invariant(duplicate));
        link.guess = -link.guess; /* (.guess < 0 ? exclude : learn_clue) */
        TIMER_STOP1(learn3, "without");

        TIMER_START(recurse3);
        if (solve_recursively(duplicate, true, &link, trace) != none) {
            solution_count = multiple; /* 1 + 1 = many and 1 + many = many */
            if (trace) {
                assert(*trace);
                (*trace)[1] = count_guesses(prev); /* divergence point */
            }
        }
        TIMER_STOP2(recurse3, "without", ": 1 -> ");
    }

    TIMER_STOP2(solver, "having", " or not: ");

    free_solver(duplicate);
    return solution_count;
}

/* return a move representing the solution in `solver` */
static char *encode_solution(struct solver* solver)
{
    const int n = solver->n;
    const int allocation_size = 20 * n;
    char *move = snewn(allocation_size, char);
    char *p = move;
    int index, value0;

    for (index = 0; index < n; ++index) {
        for (value0 = 0; value0 < n; ++value0) {
            if (possible(solver, index, value0)) {
                p += sprintf(p, "%c%d", index ? '_' : 'S', value0 + 1);
                assert(p - move < allocation_size);
                break;
            }
        }
        if (value0 == n) {
            print_solver(solver);
            assert(false); /* we found (at least) one possible value */
        }
    }

    assert(!*p);
    ++p;
    assert(p - move < allocation_size);

    return srealloc(move, p - move);
}

/* return `NULL` or an error message based on `solution_count` */
static char *solver_error_message(int solution_count)
{
    if (solution_count == multiple)
        return "Puzzle is ambiguous";
    if (solution_count == none)
        return "Puzzle doesn't have any solutions";
    return NULL;
}

/* run solver, returning allocated move or writing message into `*error` */
static char *solve_game(const struct game_state *state,
                        const struct game_state *currstate,
                        const char *aux,
                        const char **error)
{
    struct solver *solver = new_recursive_solver(state);
    const int result = solve_recursively(solver, false, NULL, NULL);

    if (result == unique_solution) {
        char *move = encode_solution(solver);
        free_solver(solver);
        return move;
    } else {
        *error = solver_error_message(result);
        assert(*error != NULL);
        free_solver(solver);
        return NULL;
    }
}

/* ----------------------------------------------------------------------
 * Generator    (See "Essay: Generator" towards the bottom of this file)
 * ---------------------------------------------------------------------- */

/* return the index of a uniformly random neighbour of `index` */
static int random_neighbour(int index,
                            int neighbours,
                            int w,
                            int h,
                            random_state *rs)
{
    const int x = index % w, y = index / w;
    int neighbouring_squares[lenof(neighbour_dxy)];
    int outdegree = 0, direction;

    for (direction = 0; direction < neighbours; ++direction) {
        const int nx = x + neighbour_dxy[direction].dx;
        const int ny = y + neighbour_dxy[direction].dy;
        if (out_of_bounds(nx, ny, w, h)) continue;
        assert(outdegree < lenof(neighbouring_squares));
        neighbouring_squares[outdegree++] = idx(nx, ny, w);
    }
    assert(0 < outdegree);

    return neighbouring_squares[random_upto(rs, (unsigned long) outdegree)];
}

/* sample a random solution using `rs` and write it into `state` */
static void pick_a_hamiltonian_path(struct game_state *state,
                                    random_state *rs)
/* Via a local search inspired by "Fast probabilistic algorithms for
 * hamiltonian circuits and matchings", Angluin and Valiant, 1977.  See
 * https://dl.acm.org/doi/10.1145/800105.803393 and the full 1979 paper,
 * https://www.sciencedirect.com/science/article/pii/002200007990045X. */
{
    const int w = state->rules.w, h = state->rules.h, n = w * h;
    const int neighbours = state->rules.neighbours;

    int i, sink, path_length, *successor;

    SNEWA(successor, n); /* we store a simple path in `successor'. */

    /* Initially, no node is on the path (successor[i] = multiple).
     * Every node i on the path has 0 <= successor[i] < n, except for
     * the final node which has successor[_] = none. */
    for (i = 0; i < n; ++i)
        successor[i] = multiple; /* multiple candidate successors */

    /* we build the path by first choosing a random last node, which
     * is a sink for those nodes that are not on the path yet. */
    sink = random_upto(rs, (unsigned long) n);
    path_length = 1;
    successor[sink] = none;

    /* then, so long as the path is not Hamiltonian yet... */
    while (path_length < n) {
        /* try to extend the path with one of `sink`'s random neighbours */
        const int predecessor = random_neighbour(sink, neighbours, w, h, rs);
        /* if the random neighbour is not on the path yet */
        if (successor[predecessor] == multiple) {
            /* then add the edge (predecessor --> sink) */
            successor[predecessor] = sink;
            sink = predecessor;
            ++path_length;
        } else /* see (x) */ {
            const bool reverse = (predecessor == successor[sink]);
            const int end_node = reverse ? none : predecessor;

            int target = end_node;
            while (true) {
                const int temp = successor[sink];
                successor[sink] = target;
                if (temp == end_node) break;
                target = sink;
                sink = temp;
            }
        }
    }

    /* (x) If the candidate predecessor is already on the path, we would
     * have created a loop by adding it.  For instance, let u=sink and
     * r=predecessor in the following:
     *
     * blah  <--  r  <--  s
     *
     * try to add |       ^
     * this edge: v       |
     *
     *            u  -->  t
     *
     * To resolve this we do a Pósa rotation, after which we have:
     *
     * blah  <--  r      s
     *
     *            ^      |
     *            |      v
     *
     *            u  <-- t
     *
     * with sink=s.  That is, add the loop-creating edge, then remove
     * the other edge going into predecessor (r) and reverse all the
     * edges in the no-longer-cycle.
     *
     * The way we perform the rotation is by reversing the linked list
     * (u -> t -> s) and concatenating the reversed list onto r, in
     * place.
     *
     * Exception: if `predecessor == successor[sink]`, in our example
     * (u --> t --> s -> ...) plus the new edge (t --> u), then we simply
     * reverse the whole path.
     */

    /* We have a solution! Write it into `state->squares` and validate it. */
    for (path_length = 0; sink != none; sink = successor[sink]) {
        const int x = sink % w;
        const int y = sink / w;

        assert(path_length < n);            /* no cycles */
        assert(sink != multiple);           /* no dangling reference */
        assert(!out_of_bounds(x, y, w, h)); /* no escaping the grid */

        state->squares[sink] = mk_clue1(++path_length);

        if (path_length < n) {
            int direction;
            for (direction = 0; direction < neighbours; ++direction) {
                const int nx = x + neighbour_dxy[direction].dx;
                const int ny = y + neighbour_dxy[direction].dy;
                if (!out_of_bounds(nx, ny, w, h))
                    if (idx(nx, ny, w) == successor[sink])
                        break;
            }
            assert(direction < neighbours); /* successors are neighbours */
        }
    }

    assert(path_length == n);               /* the path is hamiltonian */
    sfree(successor);
}

/* half, rounded up to include the middle column/row/square */
static int bigger_half(int k)
{
    return (k + 1) / 2;
}

/* allocate shuffled array of clue class representatives; return its size */
static int symmetry_classes(int w, int h, int symmetry, random_state *rs,
                            int **output)
{
    const int n = w * h;
    const int max_w = (symmetry & horizontal_mirroring) ? bigger_half(w) : w;
    const int max_h = (symmetry & vertical_mirrroring)  ? bigger_half(h) : h;
    const int max_i = (symmetry & two_way_rotation)     ? bigger_half(n) : n;
    const int limit = min(max_w * max_h, max_i);

    int *indices = snewn(limit, int);
    int length = 0, x, y;

    for (y = 0; y < max_h; ++y) {
        for (x = 0; x < max_w; ++x) {
            const int i = idx(x, y, w);
            if (max_i <= i) goto done;
            assert(length < limit);
            indices[length++] = i;
        }
    }

done:
    assert(length == limit);
    shuffle(indices, length, sizeof (*indices), rs);
    *output = indices;
    return length;
}

/* allocate and initialize the fields of `clues`:
 *
 *  - set `.sufficient` to the number of symmetry classes.
 *  - put all grid indices into `.indices`, grouped into symmetry classes with
 *    the classes in a random order.
 *  - put the starting indices of symmetry classes into `.offsets`,
 *    and the number of elements of `.indices` at the end.
 *  - put `[0..(.sufficient)-1]` into `.classes`.
 *  - put into `.estimated_first_sufficient` a guess at the length of the
 *    shortest prefix of `.classes` sufficient to solve the puzzle.
 */
static int initialize_clues(struct clues *clues,
                            const struct rules *rules,
                            int symmetry,
                            random_state *rs)
{
    const int w = rules->w, h = rules->h, n = w * h;
    const int max_symmetry_class_size =
        (symmetry == 0 ? 1 : symmetry == four_way_symmetry ? 4 : 2);

    const int classes = symmetry_classes(w, h, symmetry, rs, &clues->classes);

    const int percentage /* determined empirically */
        = (symmetry == 0)
        ? (rules->neighbours == 4 ? 24 : 58)
        : (symmetry != four_way_symmetry)
        ? (rules->neighbours == 4 ? 28 : 66)
        : (rules->neighbours == 4 ? 32 : 72);
    const int estimate = (percentage * classes) / 100;

    int indices_so_far = 0, i;

    clues->estimated_first_sufficient = max(1, estimate);

    clues->sufficient = classes;
    clues->offsets = snewn(classes + 1, int);
    clues->indices = snewn(n, int);

    for (i = 0; i < classes; ++i) {
        const int index = clues->classes[i]; /* lowest-index class member */
        const int x = index % w;
        const int y = index / w;
        const int flip_x = (w - 1) - x;
        const int flip_y = (h - 1) - y;
        const int horizontal_partner = idx(flip_x, y, w);
        const int vertical_partner = idx(x, flip_y, w);
        const int rotational_partner = idx(flip_x, flip_y, w);

        /* A normal square x has four symmetric partners including itself:
         * [i, horizontal(i), vertical(i), rotation(i)] <- this is sorted,
         * provided i is the lowest-index member of its symmetry class.
         *
         * If i is the middle square then all four are equal.
         *
         * We get i = horizontal(i) < vertical(i) = rotation(i) in the middle
         * column; the code deduplicates horizontal and rotation.
         *
         * On the middle row vertical(i) = i < horizontal(i) = rotation(i);
         * the code deduplicates vertical and rotation.
         */
        clues->offsets[i] = indices_so_far;

        clues->indices[indices_so_far++] = index;

        if (symmetry & horizontal_mirroring)
            if (clues->indices[indices_so_far - 1] < horizontal_partner)
                clues->indices[indices_so_far++] = horizontal_partner;

        if (symmetry & vertical_mirrroring)
            if (clues->indices[indices_so_far - 1] < vertical_partner)
                clues->indices[indices_so_far++] = vertical_partner;

        if (symmetry & two_way_rotation)
            if (clues->indices[indices_so_far - 1] < rotational_partner)
                clues->indices[indices_so_far++] = rotational_partner;

        if (max_symmetry_class_size < indices_so_far - clues->offsets[i])
            assert(false);

        clues->classes[i] = i; /* actually initialize clues->classes */
    }
    clues->offsets[classes] = indices_so_far;

#if !defined(NDEBUG)
    assert(classes == i);
    assert(indices_so_far == n);

    assert(classes == n || symmetry);

    /* for each i let x = clues->indices[i].  Store in clues->indices[x] the
     * fact that we have seen x, by inverting it; but first assert that we
     * haven't seen x before.  Success implies clues->indices is injective.
     * By a simple counting argument it must therefore be a permutation; that
     * is, it contains {0, ..., n-1} in some order. */
    for (i = 0; i < n; ++i) {
        const int entry = clues->indices[i]; /* maybe already inverted */
        const int index = (0 <= entry) ? entry : -(entry + 1);
        assert(0 <= index);
        assert(index < n);
        assert(0 <= clues->indices[index]); /* catches duplicates */
        clues->indices[index] = -(clues->indices[index] + 1); /* invert */
    }

    for (i = 0; i < n; ++i) {
        clues->indices[i] = -(clues->indices[i] + 1); /* restore the sign */
        assert(0 <= clues->indices[i]);
        assert(clues->indices[i] < n);
    }

    for (i = 0; i < classes; ++i) {
        const int start = clues->offsets[i], stop = clues->offsets[i+1];
        const int size = stop - start;
        int k;

        assert(1 <= size);
        assert(size != 3);
        assert(size <= max_symmetry_class_size);

        if (symmetry == 0)
            assert(size == 1);
        if (symmetry == horizontal_mirroring && (w % 2 == 0))
            assert(size == 2);
        if (symmetry == vertical_mirrroring && (h % 2 == 0))
            assert(size == 2);
	if (symmetry == two_way_rotation && (n % 2 == 0))
            assert(size == 2);
	if (symmetry == four_way_symmetry && (w % 2 == 0) && (h % 2 == 0))
            assert(size == 4);

        /* we sorted each clue class to easily deduplicate them */
        for (k = start + 1; k < stop; ++k)
            assert(clues->indices[k-1] < clues->indices[k]);
    }
#endif

    return classes;
}

/* learn each clue at offset `k` with `start <= k < stop` */
static void learn_clue_span(struct solver *solver,
                            const struct game_state *state,
                            const struct clues *clues,
                            int start,
                            int stop)
{
    int k;
    for (k = start; k < stop; ++k) {
        const int index = clues->indices[k];
        const int value0 = get_value0(state->squares[index]);
        learn_clue(solver, index, value0);
        assert(possible(solver, index, value0));
    }
}

/* learn each clue in a given symmetry class */
static void learn_clue_class(struct solver *solver,
                             const struct game_state *state,
                             const struct clues *clues,
                             int class_id)
{
    const int start = clues->offsets[class_id];
    const int stop = clues->offsets[class_id+1];
    learn_clue_span(solver, state, clues, start, stop);
}

/* make `solver` learn the one or two clue classes which contain `1` and `n`;
 * move them to the front of `clues->classes`; return how many there are. */
static int give_clues_1_and_n(struct solver *solver,
                              const struct game_state *state,
                              struct clues *clues)
{
    int found = 0, k;
    const int n = state->rules.w * state->rules.h;

    TIMER_START(clue_1_and_n);
    for (k = 0; k < clues->sufficient && found < 2; ++k) {
        const int id = clues->classes[k];
        const int start = clues->offsets[id], stop = clues->offsets[id+1];
        int m;
        for (m = start; m < stop; ++m) {
            const int index = clues->indices[m];
            const int value1 = get_value1(state->squares[index]);
            if (value1 == 1 || value1 == n) {
                if (found == 1 && id == clues->classes[0]) /* see (x) */
                    break; /* 1 and n are in the same symmetry class */
                if (found < k) {
                    clues->classes[k] = clues->classes[found];
                    clues->classes[found] = id;
                }
                assert(clues->classes[found] == id);
                ++found;
                learn_clue_span(solver, state, clues, start, stop);
            }
        }
        if (m < stop) break; /* 1 and n in the same symmetry class */
    }
    TIMER_PRINT(clue_1_and_n, puts("clue 1 and n"));

    return found;

    /* (x) it was not obvious to my past future self why this condition is
     * true iff 1 and n are in the same symmetry class.
     *
     * Let `id1` and `idn` be the values of `id` when `get_value1` returns 1
     * and n, respectively.  Assume WLOG `get_value1` returns 1 before n.
     * When it returns 1 we set `found` to 1 and `clues->classes[0]` to id1.
     * When it returns n `clues->classes[0]` contains `id1`, so the condition
     * at (x) is true iff `id1` equals `id` which equals `idn` at that time.
     */
}

#if !defined(NDEBUG)
/* check that giving clues `[0..clues->sufficient)` solves the puzzle */
static bool last_clue_solves_it(struct solver *solver,
                                const struct game_state *state,
                                const struct clues *clues)
{
#if defined(CHECK_SUFFICIENT_CLUES_INVARIANT)
    /* call this in a context with clues `0..clues->sufficient-2` given */
    const int clue = clues->classes[clues->sufficient - 1];
    REQUIRE(!is_solved(solver));
    learn_clue_class(solver, state, clues, clue);
    REQUIRE(is_solved(solver)); /* failure => bug likely to be in `exclude` */

    /* I have seen this fail during a half-finished experiment in `exclude`
     * which resulted in the solver reaching different conclusions depending
     * on the order in which it learned a fixed set of clues.
     *
     * Note that `shrink_clue_set` reorders clues.  In an iteration which does
     * not change `clues->sufficient` the _only_ change to the clue set is the
     * ordering.  So in case of assertion failure: suspect order sensitivity.
     */
#endif

    return true;
}
#endif

#define TIMER_TRACE(name, label)                        \
    TIMER_PRINT(                                        \
        name,                                           \
        printf("[%d: %d-%d-%d-%d] %s\n",                \
               n_solvers,                               \
               necessary,                               \
               k,                                       \
               clues->sufficient - margin,              \
               clues->sufficient,                       \
               label));

/* Calculate initial number of clues to leave for next level of recursion */
static int initial_margin(const struct clues *clues, int necessary)
{
    const int estimated = clues->estimated_first_sufficient;
    const int sufficient = min(clues->sufficient, estimated);
    const int spread_midpoint = (sufficient - necessary) / 2;
    const int recursion_point = necessary + spread_midpoint;
    return clues->sufficient - recursion_point;
}

/* Calculate a minimal clue set consistent with the solution in `state` but
 * not any other solution (as far as the non-recursive solver can tell). */
/* Calculate a minimal clue set such that the non-recursive solver proves that
 * (a) `state` contains a valid solution; and (b) it's the only one */
static int shrink_clue_set(int n_solvers,
                           struct solver **solvers,
                           const struct game_state *state,
                           struct clues *clues,
                           int necessary)
/* See "Essay: Generator" towards the bottom of this file */
{
    int margin = (n_solvers == 2) ? 0 : initial_margin(clues, necessary);

    TIMER_START(call);
    assert(2 <= n_solvers);

    /* invariant: `solvers[0]` has learned the first `necessary` clues */
    while (!at_most_one_solution(solvers[0])) {
        int k;

        /* the `state` solution is consistent with every subset of itself */
        assert(!no_solutions(solvers[0]));

        assert(necessary < clues->sufficient);

        if (necessary + 1 == clues->sufficient) {
            TIMER_START(last);
            assert(last_clue_solves_it(solvers[0], state, clues));
            ++necessary;
            TIMER_TRACE(last, "the last clue is required");
            break;
        }

        k = necessary;

        TIMER_START(copy);
        copy_solver_state(solvers[1], (const struct solver *) solvers[0]);
        TIMER_TRACE(copy, "copy solver");

        TIMER_START(loop);
        /* invariant: `solvers[1]` has learned the first `k` clues */
        while (k + max(1, margin) < clues->sufficient) {
            const int id = clues->classes[k];
            const int start = clues->offsets[id], stop = clues->offsets[id+1];
            int m;
            for (m = start; m < stop; ++m) {
                const int index = clues->indices[m];
                if (solvers[1]->possible_values_in_square[index] != 1)
                    break;
            }
            if (m == stop) {
                TIMER_START(omit); /* only timed for logging purposes */
                TIMER_TRACE(omit, "omit deducible clue");
                clues->classes[k] = clues->classes[--clues->sufficient];
            } else {
                TIMER_START(learn1);
                learn_clue_span(solvers[1], state, clues, start, stop);
                TIMER_TRACE(learn1, "learn clue");
                if (at_most_one_solution(solvers[1])) break;
                ++k;
            }
        }
        TIMER_TRACE(loop, "finish adding clues");
        assert(k + 1 <= clues->sufficient);

        if (k + margin < clues->sufficient) {
            const int clue = clues->classes[k]; /* this clue is necessary */

            if (k + 1 < clues->sufficient) {
                /* we break'd the loop i.e. solved with clues `[0..k]` */
                assert(is_solved(solvers[1]));
                clues->sufficient = k + 1;
            } else {
                /* we know from the last iteration (or initialization) that
                 * `[0..k]` is a sufficient set of clues */
                assert(clues->sufficient == k + 1);
                assert(last_clue_solves_it(solvers[1], state, clues));
            }

            if (necessary < k) {
                clues->classes[k] = clues->classes[necessary];
                clues->classes[necessary] = clue;
            }
            ++necessary;
            assert(necessary <= clues->sufficient);

            TIMER_START(learn0);
            learn_clue_class(solvers[0], state, clues, clue);
            TIMER_TRACE(learn0, "learn necessary clue");
        } else {
            /* the `k/solvers[1]` invariant here guarantees that we initially
             * satisfy the `necessary/solvers[0]` invariant recursively */
            struct solver **tail = solvers + 1;
            const int necessary_in_suffix =
                shrink_clue_set(n_solvers - 1, tail, state, clues, k);
            const int increment_necessary_by = necessary_in_suffix - k;
            int m;

            TIMER_START(propagate);
            for (m = 0; m < increment_necessary_by; ++m) {
                const int clue = clues->classes[k+m];
                clues->classes[k+m] = clues->classes[necessary+m];
                clues->classes[necessary+m] = clue;
                learn_clue_class(solvers[0], state, clues, clue);
            }

            /* If solvers[1] was already solved we would've break'd the loop
             * that adds clues. We didn't, thus we needed at least one more
             * clue. Hence `increment_necessary_by` is at least 1. */
            assert(0 < increment_necessary_by); /* progress is ensured */
            necessary += increment_necessary_by;
            TIMER_TRACE(propagate, "propagate");
        }

        if (2 < n_solvers) margin = (clues->sufficient - necessary + 1) / 2;
    }

    /* the `state` solution is consistent with every subset of itself */
    assert(!no_solutions(solvers[0]));

#if defined(TIMING)
    {
        const int k = necessary; /* just to make TIMER_TRACE happy */
        TIMER_TRACE(call, "return");
    }
#endif

    return necessary;
    /* let p be the value of `necessary` at the time this function was called
     * and let q be the current value.  Then `clues->classes[p..q)` are all
     * necessary, given our decision to never give any clues after `q`. */
}

/* erase all clues from `state` except the fist `necssary` ones */
static void erase_unnecessary_clues(struct game_state *state,
                                    const struct clues *clues,
                                    int necessary)
{
    const int n = state->rules.w * state->rules.h;
    int i;

    /* mark each necessary clue */
    for (i = 0; i < necessary; ++i) {
        const int id = clues->classes[i];
        const int start = clues->offsets[id], stop = clues->offsets[id+1];
        int j;
        for (j = start; j < stop; ++j) {
            const int index = clues->indices[j];
            const square s = state->squares[index];
            assert(is_clue(s));
            state->squares[index] = mk_user1(get_value1(s));
        }
    }

    /* erase all unmarked clues */
    for (i = 0; i < n; ++i)
        if (!is_user(state->squares[i]))
            state->squares[i] = mk_empty();
}

#if !defined(NDEBUG)
/* verify clue symmetry and minimality and that `1` and `n` were clued */
static bool pick_minimal_clue_set_postcondition(const struct game_state *g,
                                                int symmetry,
                                                bool clue_1_and_n,
                                                struct solver *solver,
                                                struct clues *clues,
                                                int necessary)
{
#if defined(CHECK_PICK_MINIMAL_CLUE_SET_POSTCONDITION)
    const struct game_state *state = g;
    const int w = state->rules.w, h = state->rules.h, n = w * h;
    int n_clues = 0, x, y, i, k;
    bool clued_1 = false, clued_n = false;

    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const int index = idx(x, y, w);
            const square s = state->squares[index];
            const bool empty = is_empty(s);

            if (!empty) {
                const int value1 = get_value1(s);
                if (value1 == 1) clued_1 = true;
                if (value1 == n) clued_n = true;
                ++n_clues;
            }

            if (symmetry & horizontal_mirroring) {
                const square t = state->squares[idx(w-1 - x, y, w)];
                REQUIRE(empty == is_empty(t));
            }

            if (symmetry & vertical_mirrroring) {
                const square t = state->squares[idx(x, h-1 - y, w)];
                REQUIRE(empty == is_empty(t));
            }

            if (symmetry & two_way_rotation) {
                const square t = state->squares[idx(w-1 - x, h-1 - y, w)];
                REQUIRE(empty == is_empty(t));
            }

        }
    }

    if (clue_1_and_n) {
        REQUIRE(clued_1);
        REQUIRE(clued_n);
    }

    for (i = 0; i < necessary; ++i) {
        const int id = clues->classes[i];
        const int start = clues->offsets[id], stop = clues->offsets[id+1];
        for (k = start; k < stop; ++k)
            REQUIRE(is_user(state->squares[clues->indices[k]]));
        n_clues -= (stop - start);
    }
    REQUIRE(n_clues == 0);

    if (n == 1) {
        REQUIRE(necessary == 0);
        initialize_solver(solver);
        REQUIRE(is_solved(solver));
    } else {
        for (i = 0; i < necessary; ++i) {
            if (clue_1_and_n) {
                const int id = clues->classes[i];
                const int start = clues->offsets[id];
                const int stop = clues->offsets[id+1];
                for (k = start; k < stop; ++k) {
                    const int index = clues->indices[k];
                    const int value1 = get_value1(state->squares[index]);
                    if (value1 == 1 || value1 == n) break;
                }
                if (k < stop) continue;
            }

            TIMER_START(check);
            initialize_solver(solver);
            for (k = 0; k < necessary; ++k)
                if (k != i)
                    learn_clue_class(solver, state, clues, clues->classes[k]);

            REQUIRE(!is_solved(solver));
            TIMER_PRINT(check, printf("checked all clues but %d\n", i));
        }

        initialize_solver(solver);
        for (i = 0; i < necessary; ++i)
            learn_clue_class(solver, state, clues, clues->classes[i]);
        REQUIRE(is_solved(solver));
    }
#endif

    return true;
}
#endif

/* pick a minimal symmetric set of clues with a unique solution */
static bool pick_minimal_clue_set(struct game_state *state,
                                  random_state *rs,
                                  int symmetry,
                                  bool clue_1_and_n)
{
    struct clues clues;
    const int all = initialize_clues(&clues, &state->rules, symmetry, rs);
    struct solver *solvers[GENERATOR_SOLVERS];
    const int n_solvers = lenof(solvers);
    int necessary = 0, i;

    for (i = 0; i < n_solvers; ++i)
        solvers[i] = new_generator_solver(&state->rules);

    TIMER_START(initialize);
    initialize_solver(solvers[0]);
    TIMER_PRINT(initialize, puts("initialize solvers[0]"));

    if (clue_1_and_n)
        necessary = give_clues_1_and_n(solvers[0], state, &clues);

    necessary = shrink_clue_set(n_solvers, solvers, state, &clues, necessary);

    erase_unnecessary_clues(state, &clues, necessary);

    assert(pick_minimal_clue_set_postcondition(state,
                                               symmetry,
                                               clue_1_and_n,
                                               solvers[0],
                                               &clues,
                                               necessary));

    for (i = 0; i < n_solvers; ++i)
        free_solver(solvers[i]);

    sfree(clues.classes);
    sfree(clues.offsets);
    sfree(clues.indices);

    return necessary < all;
}

#if !defined(NDEBUG)
/* assert(params admit a solvable board only for some hamiltonian paths) */
static bool may_need_rejection_sampling(const struct game_params *params)
{
    const int n = params->rules.w * params->rules.h;
    const int symmetry = symmetry_choices[params->symmetry_index];

    REQUIRE(params->clue_1_and_n);
    REQUIRE(2 <= params->rules.w);
    REQUIRE(2 <= params->rules.h);

    if (params->rules.neighbours == 4) {
        if (symmetry == four_way_symmetry) {
            REQUIRE(n == 6 || n == 8);
        } else {
            REQUIRE(symmetry == horizontal_mirroring ||
                    symmetry == vertical_mirrroring);
            REQUIRE(n == 4);
        }
    } else {
        REQUIRE(params->rules.neighbours == 8);
        if (symmetry == four_way_symmetry) {
            REQUIRE(n == 16 || n == 24);
            REQUIRE(params->rules.w == 4 || params->rules.h == 4);
        } else {
            REQUIRE(symmetry == two_way_rotation);
            REQUIRE(n == 6);
        }
    }

    return true;
}
#endif

/*
 * - clues are encoded as base 10 numbers
 * - letters (a..z) before and after clues count empty squares
 * - we use `'z'` as tally marks if there's a run of 52+ empty squares
 * - an underscore denotes zero empty squares (as a number separator)
 * - squares are encoded in row major order
 * - all squares are encoded, including trailing empty squares
 * - for example `(8x7e0:)1zza2a` is valid (but unsolvable)
 */
static char *encode_game(const struct game_state *state)
{
    const int n = state->rules.w * state->rules.h, allocation = 20 * n;
    char *desc = snewn(allocation, char), *p = desc;
    int i, runlength;

    for (runlength = i = 0; i <= n; ++i, ++runlength) {
        if (i < n && is_empty(state->squares[i]))
            continue;
        if (runlength == 0 && 0 < i && i < n)
            *(p++) = '_';
        /* when i == n this encodes a trailing run of empty squares */
        for (; 26 < runlength; runlength -= 26)
            *(p++) = 'a' - 1 + 26;
        if (0 < runlength)
            *(p++) = 'a' - 1 + runlength;
        runlength = -1;
        if (i < n)
            p += sprintf(p, "%d", get_value1(state->squares[i]));
    }
    *(p++) = '\0';

    assert(p - desc < allocation);
    return srealloc(desc, p - desc);
    /* count clues with `cut -f2 -d: | egrep -o '[0-9]*' | wc -l` */
}

/* randomly sample a new game description; see `encode_game` for format */
static char *new_game_desc(const struct game_params *params,
                           random_state *rs,
                           char **aux,
                           bool interactive)
{
    const int symmetry = symmetry_choices[params->symmetry_index];
    game_state *state = create_blank_game(params);
    char *desc;

    assert(validate_params(params, true) == NULL);

    TIMER_START(generate);
    while (true) {
        TIMER_START(path);
        pick_a_hamiltonian_path(state, rs);
        TIMER_PRINT(path, puts("pick a hamiltonian path"));
        if (pick_minimal_clue_set(state, rs, symmetry, params->clue_1_and_n))
            break;
        assert(may_need_rejection_sampling(params));
    }
    TIMER_PRINT(generate, puts("generate puzzle"));

    desc = encode_game(state);
    free_game(state);
    return desc;
}

/* ----------------------------------------------------------------------
 * Parsing of game IDs (validate_desc, new_game)
 * ---------------------------------------------------------------------- */

/* - see `encode_game` for the format of `desc`
 * - return an error message if `desc` is invalid, else NULL.
 * - if `squares` is not NULL then parse `desc` into it.
 */
static char *parse_desc(const struct game_params *params,
                        const char *desc,
                        square *squares)
{
    const int n = params->rules.w * params->rules.h;
    int index, value0, *location;
    char *errmsg = NULL;

    SNEWA(location, n);

    for (value0 = 0; value0 < n; ++value0)
        location[value0] = none;

    if (squares)
        for (index = 0; index < n; ++index)
            squares[index] = mk_empty();

    index = 0;
    while (*desc && index < n) {
        if (isdigit(*desc)) {
            const int value1 = parse_number(&desc, n);
            const int value0 = value1 - 1;

            if (value1 < 1 || n < value1) {
                errmsg = "Clue value outside valid range (1..n)";
                goto done;
            }

            if (location[value0] == none) {
                location[value0] = index;
                if (squares)
                    squares[index] = mk_clue1(value1);
            } else {
                errmsg = "A clue occurs more than once";
                goto done;
            }
            ++index;
        } else if (isalpha(*desc) && islower(*desc)) {
            index += *(desc++) - 'a' + 1;
        } else if (*desc == '_') {
            ++desc;
        } else {
            errmsg = "Unknown character in game description";
            goto done;
        }
    }

    for (; *desc == '_'; ++desc);

    if (*desc || n < index) {
        errmsg = "Data in game description overflows the board";
    } else if (index < n) {
        errmsg = "Not enough data in game description to fill the board";
    }

done:
    sfree(location);
    return errmsg;
}

/* return `NULL` or an error message if `desc` is invalid */
static const char *validate_desc(const struct game_params *params,
                                 const char *desc)
{
    return parse_desc(params, desc, NULL);
}

/* parse `desc` into a newly allocated game state and return it */
static struct game_state *new_game(midend *me,
                                   const struct game_params *params,
                                   const char *desc)
{
    struct game_state *state = create_blank_game(params);
    if (parse_desc(params, desc, state->squares))
        assert(false); /* `new_game` never called if this would fail */
    return state;
}

/* ----------------------------------------------------------------------
 * Sizes and drawstate
 * ---------------------------------------------------------------------- */

/* return a newly allocated drawstate */
static game_drawstate *game_new_drawstate(drawing *dr,
                                          const struct game_state *state)
{
    struct game_drawstate *ds = snew(struct game_drawstate);
    ds->tilesize = -1;
    ds->immediate_flags = NULL;
    ds->retained_flags = NULL;
    ds->retained_numbers = NULL;
    ds->overlay_squares = NULL;
    return ds;
}

/* deallocate `ds` and all allocations inside it */
static void game_free_drawstate(drawing *dr, struct game_drawstate *ds)
{
    sfree(ds->overlay_squares);
    sfree(ds->immediate_flags);
    sfree(ds->retained_flags);
    sfree(ds->retained_numbers);
    sfree(ds);
}

/* from an abstract `tilesize` size parameter, give the true tile size */
static int tile_size(int tilesize)
{
    /* I want to draw line segments through the middle pixels of the internal
     * borders; middle pixels only exists if length is odd, so make it odd. */
    return tilesize | 1;
}

/* from an abstract `tilesize` size parameter, give the outer margin */
static int margin_size(int tilesize)
{
    /* `-1` to make `canvas_dimension` (see below) an injective function of
     * `tilesize`, for each fixed w and h.  This is probably not required, but
     * satisfied by most puzzles (I think) and it's a fine aesthetic choice */
    return (tilesize - 1) / 2;
}

/* from an abstract `tilesize` size parameter, give inner border thickness */
static int internal_border_size(int tilesize)
{
    return 1;
}

/* from an abstract `tilesize` size parameter, give outer border thickness */
static int external_border_size(int tilesize)
{
    return 3 + (tilesize / 64);
}

/* compute the width in pixels before the first tile and after the last */
static int first_tile_pixel_offset(int tilesize)
{
    return margin_size(tilesize) + external_border_size(tilesize);
}

/* compute the distance between the start of one tile and the next */
static int next_tile_pixel_gap(int tilesize)
{
    return tile_size(tilesize) + internal_border_size(tilesize);
}

/* calculate canvas width/height from tilesize and tile count */
static int canvas_dimension(int tilesize, int w_or_h)
{
    const int margin_and_border = first_tile_pixel_offset(tilesize);

    const int interior
        = (w_or_h - 1) * next_tile_pixel_gap(tilesize)
        + tile_size(tilesize);

    return margin_and_border + interior + margin_and_border;
}

/* calculate desired canvas size from tile size */
static void game_compute_size(const struct game_params *params,
                              int tilesize,
                              const struct game_ui *ui,
                              int *x,
                              int *y)
{
    *x = canvas_dimension(tilesize, params->rules.w);
    *y = canvas_dimension(tilesize, params->rules.h);
}

/* receive and store the tile size */
static void game_set_size(drawing *dr,
                          struct game_drawstate *ds,
                          const struct game_params *params,
                          int tilesize)
{
    assert(ds->tilesize == -1);
    ds->tilesize = tilesize;
}

/* ----------------------------------------------------------------------
 * User interface logic (input processing and making moves)
 * ---------------------------------------------------------------------- */

/* return a newly allocated and initialized `game_ui` */
static struct game_ui *new_ui(const struct game_state *state)
{
    struct game_ui *ui = snew(game_ui);

    ui->dragged_squares = snewn(state->rules.w * state->rules.h, int);

    ui->dragged_length = 0;
    ui->x = 0;
    ui->y = 0;
    ui->last_mouse_x = 0;
    ui->last_mouse_y = 0;
    ui->number = 0;

    ui->visible = false;
    ui->drag_increasing = false;
    ui->is_mouse_dragging = false;
    ui->click_canceled = false;

    return ui;
}

/* deallocate `ui` and all allocations inside it */
static void free_ui(struct game_ui *ui)
{
    sfree(ui->dragged_squares);
    sfree(ui);
}

/* return NULL (or hypothetically a string representation of `ui`) */
static char *encode_ui(const struct game_ui *ui)
{
    return NULL;
}

/* parse `encoding` into `ui`, mutating it in place (noop) */
static void decode_ui(struct game_ui *ui,
                      const char *encoding,
                      const struct game_state *state)
{
}

/* return the contents of the square at cursor */
static square current_square(const struct game_ui *ui,
                             const struct game_state *state)
{
    return state->squares[idx(ui->x, ui->y, state->rules.w)];
}

#if !defined(NDEBUG)
/* assert that `ui` violates none of its invariants */
static bool ui_invariant(const struct game_ui *ui,
                         const struct game_state *state)
{
    int k, m;
    REQUIRE(!out_of_bounds(ui->x, ui->y, state->rules.w, state->rules.h));
    REQUIRE(ui->dragged_length <= state->rules.w * state->rules.h);

    for (k = 1; k < ui->dragged_length; ++k)
        for (m = 0; m < k; ++m)
            REQUIRE(ui->dragged_squares[m] != ui->dragged_squares[k]);

    if (ui->number) {
        REQUIRE(ui->visible);
        REQUIRE(!is_clue(current_square(ui, state)));
    }
    return true;
}
#endif

/* return `true` iff something like `12_` is visible at cursor */
static bool is_entering_number(const struct game_ui *ui)
{
    return 0 < ui->number;
}

/* erase something like `12_` at cursor */
static void clear_number(game_ui *ui)
{
    ui->number = 0;
    assert(!is_entering_number(ui));
}

/* turn something like `12_` into `1_` at cursor */
static void pop_digit(game_ui *ui)
{
    ui->number /= 10;
}

/* turn something like `1_` into `12_` at cursor */
static bool push_digit(game_ui *ui,
                       const struct game_state *state,
                       int character)
{
    const int old_number = ui->number;
    const int new_number = ui->number * 10 + character - '0';

    if (new_number <= state->rules.w * state->rules.h)
        ui->number = new_number;

    return ui->number != old_number;
}

/* write `number1` (zero erases) into the square `(ui->x, ui->y)` */
static char *put_or_erase_number1(const struct game_ui *ui, int number1)
{
    return string(80, "W%d,%d,%d", ui->x, ui->y, number1);
}

/* return a move entering `ui->number` at cursor (unless redundant) */
static char *commit_number(struct game_ui *ui,
                           const struct game_state *state)
{
    const int number1 = ui->number;
    const square square = current_square(ui, state);

    assert(is_entering_number(ui));
    clear_number(ui);

    assert(!is_clue(square));
    if (is_user(square) && get_value1(square) == number1) {
        return MOVE_UI_UPDATE;
    } else {
        return put_or_erase_number1(ui, number1);
    }
}

/* return a move entering `ui->number` if any, else `MOVE_UI_UPDATE` */
static char *commit_number_if_any(struct game_ui *ui,
                                  const struct game_state *state)
{
    return is_entering_number(ui) ? commit_number(ui, state) : MOVE_UI_UPDATE;
}

/* return `true` iff a drag curve is active */
static bool is_dragging(const struct game_ui *ui) {
    return 0 < ui->dragged_length;
}

/* erase the whole drag curve */
static void clear_drag(struct game_ui *ui) {
    ui->dragged_length = 0;
    assert(!is_dragging(ui));
}

/* pop one square off the drag curve; move cursor to new end, if any */
static void pop_drag(game_ui *ui, const struct game_state *state) {
    assert(is_dragging(ui));
    --ui->dragged_length;

    if (is_dragging(ui)) {
        const int last_index = ui->dragged_squares[ui->dragged_length - 1];
        ui->x = last_index % state->rules.w;
        ui->y = last_index / state->rules.w;
    }
}

/* return true iff `index` is the `direction`'th neighbour of `x` and `y` */
static bool is_neighbour(const game_ui *ui,
                         const struct game_state *state,
                         int index,
                         int direction)
{
    const int nx = ui->x + neighbour_dxy[direction].dx;
    const int ny = ui->y + neighbour_dxy[direction].dy;
    const bool oob = out_of_bounds(nx, ny, state->rules.w, state->rules.h);
    return (!oob) && idx(nx, ny, state->rules.w) == index;
}

/* Let `index = idx(ui->x, ui->y, state->rules.w)`. Either:
 *
 * - Delete everything on the drag curve after `index` (if anything); or
 * - Add `index` to the drag curve if it's adjacent to the current end; or
 * - Do nothing.
 *
 * Return true iff `ui->dragged_length` has been modified.
 */
static bool push_drag(struct game_ui *ui, const struct game_state *state)
{
    const int index = idx(ui->x, ui->y, state->rules.w);
    int i;

    for (i = 0; i < ui->dragged_length; ++i) {
        if (ui->dragged_squares[i] == index) {
            ui->dragged_length = i + 1;
            assert(is_dragging(ui));
            return true;
        }
    }

    if (is_dragging(ui)) {
        const int last_index = ui->dragged_squares[ui->dragged_length - 1];
        for (i = 0; i < state->rules.neighbours; ++i) {
            if (is_neighbour(ui, state, last_index, i)) {
                ui->dragged_squares[ui->dragged_length++] = index;
                return true;
            }
        }
    }

    return false;
}

/* replace the last drag curve square with the current cursor square */
static void update_drag(struct game_ui *ui, const struct game_state *state) {
    assert(is_dragging(ui));
    if (ui->dragged_length == 1) {
        ui->dragged_squares[0] = idx(ui->x, ui->y, state->rules.w);
    } else {
        --ui->dragged_length;
        if (!push_drag(ui, state)) {
            ++ui->dragged_length;
        }
    }
}

/* calculate first drag number from an explicit number and its drag index */
static int first_drag_number1(const struct game_ui *ui, int value1, int index)
{
    return value1 + (ui->drag_increasing ? -index : index);
}

/* calculate the number which will go in square `ui->dragged_squares[0]` */
static int determine_first_drag_number1(const struct game_ui *ui,
                                        const struct game_state *state) {
    int k;

    if (is_entering_number(ui)) {
        return first_drag_number1(ui, ui->number, ui->dragged_length - 1);
    }

    for (k = 0; k < ui->dragged_length; ++k) {
        const square s = state->squares[ui->dragged_squares[k]];
        if (!is_empty(s)) {
            return first_drag_number1(ui, get_value1(s), k);
        }
    }

    return ui->drag_increasing ? 1 : state->rules.w * state->rules.h;
}

/* turn drag curve and explicit start into a move, eliminating redundancies */
static char *finish_drag_starting_at(struct game_ui *ui,
                                     const struct game_state *state,
                                     int first_number)
{
    const size_t initial_allocation = ui->dragged_length * 40;
    char *const move = snewn(initial_allocation, char);
    char *p = move;
    const int sign = ui->drag_increasing ? (+1) : (-1);
    const int n = state->rules.w * state->rules.h;
    int k;

    for (k = 0; k < ui->dragged_length; ++k) {
        const int index = ui->dragged_squares[k];
        const square square = state->squares[index];
        if (!is_clue(square)) {
            const int x = ui->dragged_squares[k] % state->rules.w;
            const int y = ui->dragged_squares[k] / state->rules.w;
            const int value1 = first_number + sign * k;
            if (1 <= value1 && value1 <= n && value1 != get_value1(square)) {
                p += sprintf(p, "W%d,%d,%d", x, y, value1);
            }
        }
    }

    clear_number(ui);

    if (p == move) {
        clear_drag(ui);
        sfree(move);
        return MOVE_UI_UPDATE;
    }

    assert(!*p);
    assert(p + 1 - move < initial_allocation);
    return srealloc(move, p + 1 - move);
}

/* return a move made from `ui->dragged_squares`, eliminating redundancies */
static char *finish_drag(game_ui *ui, const struct game_state *state) {
    const int first_number = determine_first_drag_number1(ui, state);
    return finish_drag_starting_at(ui, state, first_number);
}

/* `start_drag_in_current_direction(ui, index)`: set up single-square drag */
static void start_drag_in_current_direction(game_ui *ui, int index)
{
    /* 'current direction' means `ui->drag_increasing` remains unchanged */
    ui->dragged_length = 1;
    ui->dragged_squares[0] = index;
    assert(is_dragging(ui));
}

/* set up a single-square drag in the indicated direction */
static void start_drag_in_direction(game_ui *ui, int index, bool increasing)
{
    ui->drag_increasing = increasing;
    start_drag_in_current_direction(ui, index);
}

/* start typing a number, or extend the current number.  Commit the number
 * (i.e. return a move string) if it cannot be extended further.  Do nothing
 * if no number can be entered in the cursor square. */
static char *handle_digit(struct game_ui *ui,
                          const struct game_state *state,
                          int button)
{
    const bool cannot_enter_number_right_now
        = ui->is_mouse_dragging
        || is_clue(current_square(ui, state));

    if (cannot_enter_number_right_now) {
        return MOVE_NO_EFFECT;
    } else {
        const bool updated = push_digit(ui, state, button);
        if (updated) {
            ui->click_canceled = true;
            ui->visible = true;
        }
        if (ui->number * 10 <= state->rules.w * state->rules.h) {
            return updated ? MOVE_UI_UPDATE : MOVE_NO_EFFECT;
        } else if (is_dragging(ui)) {
            return finish_drag(ui, state);
        } else {
            return commit_number(ui, state);
        }
    }
}

/* start, reverse or finish keyboard drag, or finish entering a number */
static char *handle_enter_and_space(struct game_ui *ui,
                                    const struct game_state *state,
                                    bool increasing)
{
    if (is_dragging(ui)) {
        if (ui->drag_increasing == increasing) {
            return finish_drag(ui, state);
        } else {
            ui->drag_increasing = increasing;
            return MOVE_UI_UPDATE;
        }
    } else if (is_entering_number(ui)) {
        return commit_number(ui, state);
    } else {
        const int index = idx(ui->x, ui->y, state->rules.w);
        start_drag_in_direction(ui, index, increasing);
        ui->is_mouse_dragging = false;
        return MOVE_UI_UPDATE;
    }
}

/* Either:
 *  - Delete one digit from the number being entered; or
 *  - Remove one square from the drag curve; or
 *  - Delete the number in the cursor square; or
 *  - Do nothing
 */
static char *handle_backspace(struct game_ui *ui,
                              const struct game_state *state)
{
    if (is_entering_number(ui)) {
        pop_digit(ui);
        return MOVE_UI_UPDATE;
    } else if (is_dragging(ui)) {
        pop_drag(ui, state);
        return MOVE_UI_UPDATE;
    } else if (is_user(current_square(ui, state))) {
        return put_or_erase_number1(ui, 0);
    } else {
        return MOVE_NO_EFFECT;
    }
}

/* erase number being entered, or cancel drag operation, or do nothing */
static char *handle_escape(struct game_ui *ui,
                           const struct game_state *state)
{
    if (is_entering_number(ui)) {
        clear_number(ui);
        return MOVE_UI_UPDATE;
    } else if (is_dragging(ui)) {
        clear_drag(ui);
        ui->click_canceled = true;
        return MOVE_UI_UPDATE;
    } else {
        return MOVE_NO_EFFECT;
    }
}

/* return the cursor delta-x from a cursor motion keystroke */
static int x_signum(int button)
{
    switch (button & ~MOD_MASK) {
    case '1':
    case '4':
    case '7':
    case CURSOR_LEFT:
        return -1;
    case '3':
    case '6':
    case '9':
    case CURSOR_RIGHT:
        return +1;
    }
    return 0;
}

/* return the cursor delta-y from a cursor motion keystroke */
static int y_signum(int button)
{
    switch (button & ~MOD_MASK) {
    case '1':
    case '2':
    case '3':
    case '5':
    case CURSOR_DOWN:
        return +1;
    case '7':
    case '8':
    case '9':
    case CURSOR_UP:
        return -1;
    }
    return 0;
}

/* Move keyboard cursor (but not out of bounds, just make it visible).
 * If ctrl/shift and not dragging: start and finish a two-square drag.
 * If dragging and entering number: finish number and drag.
 * If dragging and ctrl: don't move into already dragged squares.
 * If dragging: extend or update drag curve.
 */
static char *handle_arrow(struct game_ui *ui,
                          const struct game_state *state,
                          int button)
{
    const int w = state->rules.w;
    const int h = state->rules.h;
    const int new_x = ui->x + x_signum(button);
    const int new_y = ui->y + y_signum(button);
    const bool oob = out_of_bounds(new_x, new_y, w, h);
    const bool noop = oob || (new_x == ui->x && new_y == ui->y);
    const bool was_visible = ui->visible;

    ui->visible = true;

    if (noop) return was_visible ? MOVE_NO_EFFECT : MOVE_UI_UPDATE;

    if (!is_dragging(ui)) {
        char *move;

        if (button & (MOD_CTRL | MOD_SHFT)) {
            ui->dragged_length = 2;
            ui->dragged_squares[0] = idx(ui->x, ui->y, w);
            ui->dragged_squares[1] = idx(new_x, new_y, w);
            ui->drag_increasing = !!(button & MOD_CTRL);
            move
                = is_entering_number(ui)
                ? finish_drag_starting_at(ui, state, ui->number)
                : finish_drag(ui, state);
        } else {
            move = commit_number_if_any(ui, state);
        }

        ui->x = new_x;
        ui->y = new_y;
        return move;
    }

    if (button & MOD_CTRL) {
        const int index = idx(new_x, new_y, w);
        int i;
        for (i = 0; i < ui->dragged_length; ++i)
            if (ui->dragged_squares[i] == index)
                break;
        if (i < ui->dragged_length)
            return MOVE_NO_EFFECT;
    }

    ui->x = new_x;
    ui->y = new_y;

    if (is_entering_number(ui))
        return finish_drag(ui, state);

    if (button & MOD_SHFT)
        update_drag(ui, state);
    else
        push_drag(ui, state);

    return MOVE_UI_UPDATE;
}

/* convert a canvas coordinate to a grid coordinate; return -1 if the click is
 * either out of bounds or outside the active click area centered within the
 * clicked square and whose size is given by `numerator` and `denominator`. */
static int canvas_to_grid(int x,
                          int w,
                          int tilesize,
                          int numerator,
                          int denominator)
{
    const int tile_start = first_tile_pixel_offset(tilesize);
    const int tile_width = tile_size(tilesize);
    const int tile_delta = next_tile_pixel_gap(tilesize);
    const int tile_relative_position = (x - tile_start) % tile_delta;
    const int margin_numerator = denominator - numerator;
    const int margin_denominator = 2 * denominator;
    const int begin = tile_width * margin_numerator / margin_denominator;
    const int end = tile_width - begin;

    if (x < tile_start || tile_start + w * tile_delta <= x)
        return -1;
    if (tile_relative_position < begin || end < tile_relative_position)
        return -1;

    return (x - tile_start) / tile_delta;
}

/* Either cancel a keyboard-based input action (out of bounds); or start,
 * extend or finish a mouse drag curve. */
static char *handle_mouse_event(struct game_ui *ui,
                                const game_state *state,
                                int event,
                                int x,
                                int y,
                                int tilesize)
{
    const int w = state->rules.w, h = state->rules.h;
    const int neighbours = state->rules.neighbours;
    const bool small_active_zone = (neighbours == 8 && IS_MOUSE_DRAG(event));
    const int numerator = small_active_zone ? 3 : 5;
    const int event_x = canvas_to_grid(x, w, tilesize, numerator, 5);
    const int event_y = canvas_to_grid(y, h, tilesize, numerator, 5);
    const bool no_grid_coordinate = event_x == -1 || event_y == -1;

    if (no_grid_coordinate) {
        if (IS_MOUSE_DOWN(event) && ui->visible) {
            ui->visible = false;
            clear_drag(ui);
            clear_number(ui);
            return MOVE_UI_UPDATE;
        }
        return MOVE_NO_EFFECT;
    }

    if (IS_MOUSE_DOWN(event)) {
        char *move = commit_number_if_any(ui, state);
        ui->drag_increasing = (event == LEFT_BUTTON);
        ui->x = ui->last_mouse_x = event_x;
        ui->y = ui->last_mouse_y = event_y;
        ui->visible = true;
        ui->click_canceled = false;
        return move;
    } else if (IS_MOUSE_DRAG(event)) {
        char *move;
        if (ui->click_canceled || (event_x == ui->x && event_y == ui->y))
            return MOVE_NO_EFFECT;

        move = commit_number_if_any(ui, state);

        ui->visible = false;
        ui->is_mouse_dragging = true;

        if (!is_dragging(ui)) {
            const int index = idx(ui->last_mouse_x, ui->last_mouse_y, w);
            start_drag_in_current_direction(ui, index);
        }
        ui->x = ui->last_mouse_x = event_x;
        ui->y = ui->last_mouse_y = event_y;
        push_drag(ui, state);
        return move;
    } else if (IS_MOUSE_RELEASE(event)) {
        ui->is_mouse_dragging = false;
        if (is_dragging(ui)) {
            ui->visible = false;
            return finish_drag(ui, state);
        }
        return MOVE_NO_EFFECT;
    } else {
        return MOVE_UNUSED; /* unreachable last time I checked */
    }
}

/* really translate input event into a move (etc.) */
static char *really_interpret_move(const struct game_state *state,
                                   struct game_ui *ui,
                                   const struct game_drawstate *ds,
                                   int x,
                                   int y,
                                   int button)
{
    const int event = STRIP_BUTTON_MODIFIERS(button);

    assert(ui_invariant(ui, state));

    if (LEFT_BUTTON <= event && event <= RIGHT_RELEASE) {
        return handle_mouse_event(ui, state, event, x, y, ds->tilesize);
    } else if ('0' <= button && button <= '9') {
        return handle_digit(ui, state, button);
    } else if (event == CURSOR_SELECT) {
        return handle_enter_and_space(ui, state, true);
    } else if (event == CURSOR_SELECT2 || button == ('0' | MOD_NUM_KEYPAD)) {
        return handle_enter_and_space(ui, state, false);
    } else if (event == 8) {
        return handle_backspace(ui, state);
    } else if (event == 27) {
        return handle_escape(ui, state);
    } else if (IS_CURSOR_MOVE(event) || (button & MOD_NUM_KEYPAD)) {
	return handle_arrow(ui, state, button);
    }

    return MOVE_UNUSED;
}

#if defined(TRACE_UI)
/* return `"true"` or `"false"` */
static const char *stringify_bool(bool b)
{
    return b ? "true" : "false";
}
#endif

/* print all the fields on the `ui` object, plus the `button` event */
static void trace_ui(const game_ui *ui,
                     int w,
                     int x,
                     int y,
                     int button,
                     char *move)
{
#if defined(TRACE_UI)
    printf("button=%d, x=%d, y=%d; move=\"%s\";"
           " ui: x=%d, y=%d, last_mouse_x=%d, last_mouse_y=%d,"
           " number=%d, visible=%s, drag_increasing=%s,"
           " is_mouse_dragging=%s, click_canceled=%s,"
           " dragged_length=%d; dragged_squares:",
           button,
           x,
           y,
           move,
           ui->x,
           ui->y,
           ui->last_mouse_x,
           ui->last_mouse_y,
           ui->number,
           stringify_bool(ui->visible),
           stringify_bool(ui->drag_increasing),
           stringify_bool(ui->is_mouse_dragging),
           stringify_bool(ui->click_canceled),
           ui->dragged_length);

    if (!is_dragging(ui)) {
        puts(" <none>");
    } else {
        int i;
        for (i = 0; i < ui->dragged_length; ++i)
            printf(" (%d,%d)",
                   ui->dragged_squares[i] / w + 1,
                   ui->dragged_squares[i] % w + 1);
        puts("");
    }
#endif
}

/* translate input event into a move, or `MOVE_{UI_UPDATE,NO_EFFECT}` etc. */
static char *interpret_move(const struct game_state *state,
                            struct game_ui *ui,
                            const struct game_drawstate *ds,
                            int x,
                            int y,
                            int button)
{
    char *move = really_interpret_move(state, ui, ds, x, y, button);
    trace_ui(ui, state->rules.w, x, y, button, move);
    assert(ui_invariant(ui, state));
    return move;
}

/* If drawflags is `NULL`: return `true` if solved, else `false`.
 *
 * Otherwise (i.e. `drawflags` is not `NULL`):
 *
 *  - Indicate line segments between `x` and `x+1` with `1 << direction`
 *
 *  - Mark each `x` as `diminished` if it's adjacent to `x-1` and `x+1`
 *    - Diminish `1` and `n` when adjacent to just one of {`x-1`, `x+1`}
 *
 *  - Highlight these errors:
 *    - Any number occurring twice (or more)
 *    - A pair of squares containing `x` and `x+1` if they are not adjacent
 *    - Any square containing `x` with no empty neighbours if:
 *      - `1 < x` and `x-1` is not placed anywhere
 *      - `x < n` and `x+1` is not placed anywhere
 */
static bool find_errors_and_set_drawflags(const struct rules *rules,
                                          const square *squares,
                                          drawflag *drawflags)
{
    const int w = rules->w, h = rules->h, n = w * h;
    const int neighbours = rules->neighbours;
    int x, y, index, value0, *location = snewn(n, int);
    bool solved = false;

    for (value0 = 0; value0 < n; ++value0)
        location[value0] = none;

    /* `location[value0] := index` where `get_value0(squares[index]) = value0`
     * or the constant `multiple` if multiple copies of `value0` occur. */
    for (index = 0; index < n; ++index) {
        if (is_empty(squares[index])) {
            if (!drawflags)
                goto done;
            continue;
        }
        value0 = get_value0(squares[index]);

        if (location[value0] == none)
            location[value0] = index;
        else {
            if (!drawflags)
                goto done;
            if (location[value0] != multiple) {
                drawflags[location[value0]] |= highlight_error;
                location[value0] = multiple;
            }
            drawflags[index] |= highlight_error;
        }
    }

    /* adjacency checking (everything but duplicated numbers) */
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const int index = idx(x, y, w);
            const int value0 = get_value0(squares[index]);
            bool found_predecessor = false, found_successor = false;
            bool surrounded = true;
            int linked_neighbours = 0, direction;
            if (is_empty(squares[index]))
                continue;
            for (direction = 0; direction < neighbours; ++direction) {
                const int neighbour_x = x + neighbour_dxy[direction].dx;
                const int neighbour_y = y + neighbour_dxy[direction].dy;
                const int neighbour_index = idx(neighbour_x, neighbour_y, w);
                bool found_neighbour = false;
                int neighbour_value0;

                if (out_of_bounds(neighbour_x, neighbour_y, w, h))
                    continue;
                if (is_empty(squares[neighbour_index])) {
                    surrounded = false;
                    continue;
                }

                neighbour_value0 = get_value0(squares[neighbour_index]);
                if (neighbour_value0 == value0 - 1) {
                    found_predecessor = found_neighbour = true;
                } else if (neighbour_value0 == value0 + 1) {
                    found_successor = found_neighbour = true;
                }
                if (drawflags && found_neighbour) {
                    drawflags[index] |= 1 << direction;
                    ++linked_neighbours;
                }
            }
            if ((0 < value0 && value0 < n - 1) < linked_neighbours) {
                assert(drawflags);
                drawflags[index] |= diminished;
            }
            if ((0 < value0 && !found_predecessor &&
                 (surrounded || location[value0 - 1] != none)) ||
                (value0 < n - 1 && !found_successor &&
                 (surrounded || location[value0 + 1] != none)))
            {
                if (!drawflags)
                    goto done;
                drawflags[index] |= highlight_error;
            }
        }
    }

    solved = true;
done:
    sfree(location);
    return solved;
}

/* `is_completed(state)`: `true` iff `state` is in a solved state */
static bool is_completed(const struct game_state *s)
{
    return find_errors_and_set_drawflags(&s->rules, s->squares, NULL);
}

/* `0..k` of `"W${x},${y},${value1}"` (0=erase) and `"S${desc}"` (solve) */
static struct game_state *execute_move(const struct game_state *prev,
                                       const char *move)
{
    struct game_state *state = dup_game(prev);

    while (*move) {
        int x, y, value1, nchars;
        if (sscanf(move, "W%d,%d,%d%n", &x, &y, &value1, &nchars) == 3) {
            const int index = idx(x, y, state->rules.w);
            if (out_of_bounds(x, y, state->rules.w, state->rules.h))
                goto fail;
            if (is_clue(state->squares[index]))
                goto fail;
            if (value1 < 0 || state->rules.w * state->rules.h < value1)
                goto fail;
            state->squares[index] = (value1 ? mk_user1(value1) : mk_empty());
            move += nchars;
        } else if (*move == 'S') {
            const int n = state->rules.w * state->rules.h;
            int index;
            state->used_solver = true;
            for (index = 0; index < n; ++index) {
                char separator;
                int value1, nchars;
                if (sscanf(move, "%c%d%n", &separator, &value1, &nchars) != 2)
                    goto fail;
                if (separator != (index ? '_' : 'S'))
                    goto fail;
                if (is_clue(state->squares[index])) {
                    if (value1 != get_value1(state->squares[index]))
                        goto fail;
                } else if (1 <= value1 && value1 <= n)
                    state->squares[index] = mk_user1(value1);
                else
                    goto fail;

                move += nchars;
            }
        } else {
            goto fail;
        }
    }

    state->completed = state->completed || is_completed(state);

    if (state->used_solver)
        assert(state->completed);

    return state;

fail:
    free_game(state);
    return NULL;
}

/* clear transient ui state and verify UI invariants before and after */
static void game_changed_state(struct game_ui *ui,
                               const struct game_state *oldstate,
                               const struct game_state *newstate)
{
    assert(ui_invariant(ui, oldstate));
    assert(ui_invariant(ui, newstate));
    clear_number(ui);
    clear_drag(ui);
    ui->is_mouse_dragging = false;
    if (newstate->completed && !newstate->used_solver && !oldstate->completed)
        ui->visible = false;
    trace_ui(ui, oldstate->rules.w, -1, -2, -3, "<changed state>");
    assert(ui_invariant(ui, newstate));
#if defined(TRACE_UI)
    {
        /* using this from a blank puzzle (e.g. './hidoku 20x10:tttttttttt')
         * is a workable 'edit puzzle' mode, if a bit on the simple side. */
        char *encoded_state = encode_game(newstate);
        printf("changed state to: %s\n", encoded_state);
        sfree(encoded_state);
    }
#endif
}

/* ----------------------------------------------------------------------
 * Colours
 * ---------------------------------------------------------------------- */

/* write an `(r, g, b)` triple consecutively into colour array */
static void colour(float *array, int index, float r, float g, float b)
{
    array[3 * index + 0] = r;
    array[3 * index + 1] = g;
    array[3 * index + 2] = b;
}

/* linear interpolation: `lerp(a, 0.0, b) = a` and `lerp(a, 1.0, b) = b` */
static float lerp(float a, float t, float b)
{
    return a + t*(b-a);
}

/* write all the colours into `colours`, some based on `game_mkhighlight` */
static void initialize_colours(float *colours)
{
    const float background_r = colours[3*COL_BACKGROUND+0];
    const float background_g = colours[3*COL_BACKGROUND+1];
    const float background_b = colours[3*COL_BACKGROUND+2];

    const float error_r = 1.0;
    const float error_g = 0.0;
    const float error_b = 0.0;

    const float clue_r = 0.0;
    const float clue_g = 0.0;
    const float clue_b = 0.0;

    const float user_r = 0.0;
    const float user_g = 0.6 * background_g;
    const float user_b = 0.0;

    const float entering_r = 0.0;
    const float entering_g = 0.0;
    const float entering_b = background_b;

    const float dim_clue_r = lerp(clue_r, 0.6, background_r);
    const float dim_clue_g = lerp(clue_g, 0.6, background_g);
    const float dim_clue_b = lerp(clue_b, 0.6, background_b);

    const float dim_user_r = lerp(user_r, 0.6, background_r);
    const float dim_user_g = lerp(user_g, 0.6, background_g);
    const float dim_user_b = lerp(user_b, 0.6, background_b);

    colour(colours, COL_GRID,          clue_r, clue_g, clue_b);
    colour(colours, COL_CLUE,          clue_r, clue_g, clue_b);
    colour(colours, COL_USER,          user_r, user_g, user_b);
    colour(colours, COL_ENTERING,      entering_r, entering_g, entering_b);
    colour(colours, COL_ERROR,         error_r, error_g, error_b);
    colour(colours, COL_DIMINISH_CLUE, dim_clue_r, dim_clue_g, dim_clue_b);
    colour(colours, COL_DIMINISH_USER, dim_user_r, dim_user_g, dim_user_b);
}

/* allocate and initialize the game's colours */
static float *game_colours(frontend *fe, int *ncolours)
{
    float *colours = snewn(3 * NCOLOURS, float);
    game_mkhighlight(fe, colours, COL_BACKGROUND, COL_ERROR, COL_CURSOR);
    initialize_colours(colours);
    *ncolours = NCOLOURS;
    return colours;
}

/* ----------------------------------------------------------------------
 * Drawing routines
 * ---------------------------------------------------------------------- */

/* Write numbers into `overlay_squares` by combining `ui->dragged_squares` and
 * `state` to represent what the grid would contain if the current drag curve
 * was committed.  Mark dragged squares with the drawflag `dragging`. */
static void overlay_pending_move(const struct game_state *state,
                                 const struct game_ui *ui,
                                 square *overlay_squares,
                                 drawflag *immediate_flags)
{
    /* Do not overlay `ui->number` as the UX is not good. */
    const int n = state->rules.w * state->rules.h;
    const int sign = ui->drag_increasing ? (+1) : (-1);
    const int first_drag_number1 = determine_first_drag_number1(ui, state);
    int i;

    COPY(overlay_squares, (const square *) state->squares, n);

    for (i = 0; i < ui->dragged_length; ++i) {
        const int value1 = first_drag_number1 + sign * i;
        if (1 <= value1 && value1 <= n) {
            const int index = ui->dragged_squares[i];
            immediate_flags[index] |= dragging;
            if (!is_clue(state->squares[index]))
                overlay_squares[index] = mk_clue1(value1);
        }
    }
}

/* draw a tile in canvas at `(x, y)`, showing `number`, tweaked by `flags` */
static void draw_tile(drawing *dr,
                      int x,
                      int y,
                      int tilesize,
                      drawflag flags,
                      int number)
{
    const int centre_x = x + tilesize / 2;
    const int centre_y = y + tilesize / 2;
    int direction;

    assert(!((flags & clue) && (flags & user)));
    assert(!((flags & clue) && (flags & entering)));
    assert((flags & cursor) || !(flags & entering)); /* entering => cursor */

    clip(dr, x, y, tilesize, tilesize);

    draw_rect(dr, x, y, tilesize, tilesize,
              (flags & cursor) ? COL_CURSOR : COL_BACKGROUND);

    if ((flags & clue) && (flags & highlight_error)) {
        draw_rect_outline(dr, x, y, tilesize, tilesize, COL_ERROR);
    } else if ((flags & clue) && (flags & dragging)) {
        draw_rect_outline(dr, x, y, tilesize, tilesize, COL_ENTERING);
    }

    for (direction = 0; direction < 8; ++direction)
        if (flags & (1 << direction))
            draw_line(dr,
                      centre_x + neighbour_dxy[direction].dx * tilesize / 4,
                      centre_y + neighbour_dxy[direction].dy * tilesize / 4,
                      centre_x + neighbour_dxy[direction].dx * tilesize / 2,
                      centre_y + neighbour_dxy[direction].dy * tilesize / 2,
                      COL_GRID);

    if (flags & (clue | user | entering | dragging)) {
        const int colour = (flags & clue
                            ? flags & diminished      ? COL_DIMINISH_CLUE
                                                      : COL_CLUE
                            : flags & highlight_error ? COL_ERROR
                            : flags & entering        ? COL_ENTERING
                            : flags & dragging        ? COL_ENTERING
                            : flags & diminished      ? COL_DIMINISH_USER
                                                      : COL_USER);
        char buf[80];
        const char *format = (flags & entering) ? "%d_" : "%d";

        if (lenof(buf) <= sprintf(buf, format, number))
            assert(false);

        draw_text(dr,
                  centre_x,
                  centre_y,
                  FONT_VARIABLE,
                  tilesize / 3,
                  ALIGN_VCENTRE | ALIGN_HCENTRE,
                  colour,
                  buf);
    }

    unclip(dr);
    draw_update(dr, x, y, tilesize, tilesize);
}

/* (re)draw the (changed parts of the) game state */
static void game_redraw(drawing *dr,
                        struct game_drawstate *ds,
                        const struct game_state *oldstate,
                        const struct game_state *state,
                        int dir,
                        const struct game_ui *ui,
                        float animtime,
                        float flashtime)
{
    const int w = state->rules.w;
    const int h = state->rules.h;
    const int n = w * h;
    const int tilesize = tile_size(ds->tilesize);
    const int tile_start = first_tile_pixel_offset(ds->tilesize);
    const int tile_delta = next_tile_pixel_gap(ds->tilesize);
    const int flash_sequence_index = (int) (5 * (flashtime / flash_duration));
    int i, x, y;

    if (!ds->retained_flags) {
        /* drawstate hasn't yet been initialized */
        const int margin = margin_size(ds->tilesize);
        const int internal_border = internal_border_size(ds->tilesize);
        const int external_border = external_border_size(ds->tilesize);
        const int canvas_width = canvas_dimension(ds->tilesize, w);
        const int canvas_height = canvas_dimension(ds->tilesize, h);
        const int external_border_width = canvas_width - 2 * margin;
        const int external_border_height = canvas_height - 2 * margin;

        SNEWA(ds->retained_flags, n);
        SNEWA(ds->retained_numbers, n);
        SNEWA(ds->immediate_flags, n);
        SNEWA(ds->overlay_squares, n);

        /* draw background */
        draw_rect(dr, 0, 0, canvas_width, canvas_height, COL_BACKGROUND);

        /* draw top external border */
        draw_rect(dr,
                  margin,
                  margin,
                  external_border_width,
                  external_border,
                  COL_GRID);

        /* draw horizontal internal borders */
        for (y = 1; y < h; ++y) {
            const int bx = tile_start;
            const int by = tile_start - internal_border + y * tile_delta;
            const int bw = canvas_width - 2 * tile_start;
            const int bh = internal_border;
            draw_rect(dr, bx, by, bw, bh, COL_GRID);
        }

        /* draw bottom external border */
        draw_rect(dr,
                  margin,
                  canvas_height - tile_start,
                  external_border_width,
                  external_border,
                  COL_GRID);

        /* draw left external border */
        draw_rect(dr,
                  margin,
                  margin,
                  external_border,
                  external_border_height,
                  COL_GRID);

        /* draw vertical internal borders */
        for (x = 1; x < w; ++x) {
            const int bx = tile_start - internal_border + x * tile_delta;
            const int by = tile_start;
            const int bw = internal_border;
            const int bh = canvas_height - 2 * tile_start;
            draw_rect(dr, bx, by, bw, bh, COL_GRID);
        }

        /* draw right external border */
        draw_rect(dr,
                  canvas_width - tile_start,
                  margin,
                  external_border,
                  external_border_height,
                  COL_GRID);

        draw_update(dr, 0, 0, canvas_width, canvas_height);

        for (i = 0; i < n; ++i)
            ds->retained_flags[i] = ds->retained_numbers[i] = 0;
    }

    for (i = 0; i < n; ++i)
        ds->immediate_flags[i] = 0;

    overlay_pending_move(state, ui, ds->overlay_squares, ds->immediate_flags);
    find_errors_and_set_drawflags(&state->rules,
                                  ds->overlay_squares,
                                  ds->immediate_flags);

    if (ui->visible && is_entering_number(ui)) {
        const int index = idx(ui->x, ui->y, state->rules.w);
        ds->overlay_squares[index] = mk_clue1(ui->number);
        /* mk_clue1 vs. mk_user1 doesn't matter: origin-bit is unread */
    }

    for (y = 0; y < state->rules.h; ++y) {
        for (x = 0; x < state->rules.w; ++x) {
            const int index = idx(x, y, state->rules.w);
            const square square = state->squares[index];
            const int value1 = get_value1(ds->overlay_squares[index]);

            if (is_clue(square))
                ds->immediate_flags[index] |= clue;

            if (is_user(square))
                ds->immediate_flags[index] |= user;

            if (flash_sequence_index % 2)
                ds->immediate_flags[index] |= cursor;

            if (ui->visible && x == ui->x && y == ui->y) {
                ds->immediate_flags[index] |= cursor;
                if (is_entering_number(ui))
                    ds->immediate_flags[index] |= entering;
            }

            if (ds->retained_flags[index] != ds->immediate_flags[index] ||
                ds->retained_numbers[index] != value1)
            {
                ds->retained_flags[index] = ds->immediate_flags[index];
                ds->retained_numbers[index] = value1;
                draw_tile(dr,
                          tile_start + x * tile_delta,
                          tile_start + y * tile_delta,
                          tilesize,
                          ds->retained_flags[index],
                          ds->retained_numbers[index]);
            }
        }
    }
}

/* always zero since no moves are animated */
static float game_anim_length(const struct game_state *oldstate,
                              const struct game_state *newstate,
                              int dir, game_ui *ui)
{
    return 0.0F;
}

/* flash the first time game is solved, only if solver was not used */
static float game_flash_length(const struct game_state *oldstate,
                               const struct game_state *newstate,
                               int dir, game_ui *ui)
{
    if (!oldstate->completed && newstate->completed && !newstate->used_solver)
        return flash_duration;
    return 0.0F;
}

/* extract pixel coordinates of cursor square */
static void game_get_cursor_location(const game_ui *ui,
                                     const game_drawstate *ds,
                                     const game_state *state,
                                     const game_params *params,
                                     int *x,
                                     int *y,
                                     int *w,
                                     int *h)
{
    if (ui->visible) {
        const int tile_start = first_tile_pixel_offset(ds->tilesize);
        const int tile_delta = next_tile_pixel_gap(ds->tilesize);
        const int tilesize = tile_size(ds->tilesize);

        *x = tile_start + ui->x * tile_delta;
        *y = tile_start + ui->y * tile_delta;
        *w = tilesize;
        *h = tilesize;
    }
}

/* return `+1` if the game was ever solved, `0` otherwise and `-1` never */
static int game_status(const struct game_state *state)
{
    return state->completed ? (+1) : 0;
}

/* never called, we don't have a timer */
static bool game_timing_state(const struct game_state *state, game_ui *ui)
{
    assert(false);
    return false;
}

/* ----------------------------------------------------------------------
 * Printing
 * ---------------------------------------------------------------------- */

/* output preferred printed size in millimetres */
static void game_print_size(const struct game_params *params,
                            const struct game_ui *ui,
                            float *x, float *y)
{
    *x = 6.0 * params->rules.w;
    *y = 6.0 * params->rules.h;
}

/* draw game for printing */
static void game_print(drawing *dr,
                       const struct game_state *state,
                       const struct game_ui *ui,
                       int tilesize)
{
    const int w = state->rules.w, h = state->rules.h;
    const int black = print_mono_colour(dr, 0);
    int x, y;

    /* horizontal grid lines */
    for (y = 0; y <= h; ++y)
        draw_line(dr, 0, y * tilesize, w * tilesize, y * tilesize, black);

    /* vertical grid lines */
    for (x = 0; x <= w; ++x)
        draw_line(dr, x * tilesize, 0, x * tilesize, h * tilesize, black);

    /* numbers */
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const square square = state->squares[idx(x, y, w)];
            if (!is_empty(square)) {
                char buf[20];
                if (lenof(buf) <= sprintf(buf, "%d", get_value1(square)))
                    assert(false);
                draw_text(dr,
                          x * tilesize + tilesize / 2,
                          y * tilesize + tilesize / 2,
                          FONT_VARIABLE,
                          tilesize / 3,
                          ALIGN_HCENTRE | ALIGN_VCENTRE,
                          black,
                          buf);
            }
        }
    }

    draw_update(dr, 0, 0, w * tilesize, h * tilesize);
}

/* ----------------------------------------------------------------------
 * `thegame` structure
 * ---------------------------------------------------------------------- */

#if defined(COMBINED)
#define thegame hidoku
#endif

const struct game thegame = {
    "Hidoku", /* name */
    "games.hidoku", /* winhelp_topic */
    "hidoku", /* htmlhelp_topic */
    default_params,
    game_fetch_preset,
    NULL, /* preset_menu */
    decode_params,
    encode_params,
    free_params,
    dup_params,
    true, /* can_configure */
    game_configure,
    custom_params,
    validate_params,
    new_game_desc,
    validate_desc,
    new_game,
    NULL, /* set_public_desc */
    dup_game,
    free_game,
    true, /* can_solve */
    solve_game,
    true, /* can_format_as_text_ever */
    game_can_format_as_text_now,
    game_text_format,
    NULL, /* get_prefs */
    NULL, /* set_prefs */
    new_ui,
    free_ui,
    encode_ui,
    decode_ui,
    NULL, /* request_keys */
    game_changed_state,
    NULL, /* current_key_label */
    interpret_move,
    execute_move,
    48, /* preferred_tilesize */
    game_compute_size,
    game_set_size,
    game_colours,
    game_new_drawstate,
    game_free_drawstate,
    game_redraw,
    game_anim_length,
    game_flash_length,
    game_get_cursor_location,
    game_status,
    true, /* can_print */
    false, /* can_print_in_colour */
    game_print_size,
    game_print,
    false, /* wants_statusbar */
    false, /* is_timed */
    game_timing_state,
    REQUIRE_NUMPAD, /* flags */
};

/* ----------------------------------------------------------------------
 * Standalone solver
 * ---------------------------------------------------------------------- */

#if defined(STANDALONE_SOLVER)

enum {
    EXIT_UNIQUE_SOLUTION = 0,
    EXIT_NO_SOLUTIONS = 1,
    EXIT_MULTIPLE_SOLUTIONS = 2,
    EXIT_ARGUMENT_PARSE_ERROR,
    EXIT_TOO_MANY_ARGUMENTS,
    EXIT_NO_GAME_ID_ARGUMENT,
    EXIT_NO_COLON,
    EXIT_VALIDATE_PARAMS,
    EXIT_VALIDATE_DESC,
    EXIT_IMPOSSIBLE
};

/* run recursive solver silently, the retrace its steps verbosely */
static int verbose_solver(const game_params *params, const game_state *state)
{
    char *stringified_state = game_text_format(state);
    int *trace = NULL;
    struct solver *solver = new_recursive_solver(state);
    const int solution_count = solve_recursively(solver, false, NULL, &trace);
    char *second_solution
        = (solution_count == multiple)
        ? stringify_solver(solver) : NULL;
    int k;

    printf("parsed puzzle description:\n%s", stringified_state);
    sfree(stringified_state);

    if (solution_count == none) {
        char *error = solver_error_message(solution_count);
        printf("%s\n", error);
        free_solver(solver);
        return solution_count;
    }

    assert(trace);

    /* The previous run of the solver counted solutions and told us the path
     * down the search tree to one of them (in `trace`).  Now we run the
     * solver again, but actually print out the steps. */

    verbose_standalone_solver = true;
    initialize_solver(solver);
    learn_all_clues(solver, state);

    for (k = 0; k < trace[0]; ++k) {
        const int raw_entry = trace[k + 2];
        const bool negated = raw_entry < 0;
        const int entry = (negated ? -raw_entry : raw_entry) - 1;
        const int value0 = entry / solver->n;
        const int index = entry % solver->n;
        const int value1 = value0 + 1;
        const int y = index / solver->w + 1;
        const int x = index % solver->w + 1;

        const char *message
            = (!negated)
            ? "guessed %d at (%d, %d)\n"
            : (solution_count == multiple && k == trace[1])
            ? ("guessing %d at (%d, %d) => second solution;"
               " this solution eliminates it\n")
            : ("guessing %d at (%d, %d) => not solvable;"
               " eliminating it instead\n");

        if (last_output_was_print_solver)
            puts("");
        printf(message, value1, y, x);
        last_output_was_print_solver = false;
        /* retrace just one branch down the search tree: */
        (negated ? exclude : learn_clue)(solver, index, value0);
    }

    if (second_solution != NULL) {
        printf("\nHere's a second solution:\n%s", second_solution);
        sfree(second_solution);
    }

    free_solver(solver);
    sfree(trace);

    return solution_count;
}

/* collect inputs for standalone solver, then launch it */
int main(int argc, char **argv)
{
    const char *error;

    const char *progname = argv[0];
    const char *game_id_argument = NULL;
    bool verbose = false;

    game_params *params;
    game_state *state;

    int solution_count;

    char *desc;

    while (0 < --argc) {
        char *p = *++argv;
        if (!strcmp(p, "-v")) {
            verbose = true;
        } else if (*p == '-') {
            fprintf(stderr, "%s: unrecognised option `%s'\n", progname, p);
            exit(EXIT_ARGUMENT_PARSE_ERROR);
        } else if (game_id_argument) {
            fprintf(stderr, "%s: excess argument `%s'\n", progname, p);
            exit(EXIT_TOO_MANY_ARGUMENTS);
        } else {
            game_id_argument = p;
        }
    }

    if (!game_id_argument) {
        fprintf(stderr, "usage: %s [-v] <game_id>\n", progname);
        exit(EXIT_NO_GAME_ID_ARGUMENT);
    }

    desc = strchr(game_id_argument, ':');
    if (!desc) {
        fprintf(stderr, "%s: game id expects a colon in it\n", progname);
        exit(EXIT_NO_COLON);
    }
    *(desc++) = '\0';

    params = default_params();
    decode_params(params, game_id_argument);

    error = validate_params(params, false);
    if (error) {
        free_params(params);
        fprintf(stderr, "%s: %s\n", progname, error);
        return EXIT_VALIDATE_PARAMS;
    }

    error = validate_desc(params, desc);
    if (error) {
        free_params(params);
        fprintf(stderr, "%s: %s\n", progname, error);
        return EXIT_VALIDATE_DESC;
    }

    state = new_game(NULL, params, desc);

    if (verbose) {
        solution_count = verbose_solver(params, state);
    } else {
        struct solver *solver = new_recursive_solver(state);
        solution_count = solve_recursively(solver, false, NULL, NULL);
        error = solver_error_message(solution_count);

        if (solution_count == none) {
            fprintf(stderr, "%s: couldn't solve game: %s\n", progname, error);
        } else {
            print_solver(solver);
            if (solution_count == multiple) {
                fprintf(stderr, "%s: %s; here is one solution\n",
                        progname, error);
            }
        }

        free_solver(solver);
    }

    free_params(params);
    free_game(state);

    exit(solution_count == unique_solution ? EXIT_UNIQUE_SOLUTION    :
         solution_count == none            ? EXIT_NO_SOLUTIONS       :
         solution_count == multiple        ? EXIT_MULTIPLE_SOLUTIONS :
         /* otherwise */                     EXIT_IMPOSSIBLE);
}
#endif

/* ----------------------------------------------------------------------
 * Code ends here, the rest is comments
 * ----------------------------------------------------------------------
 *
 * Quick advice before jumping into this code base:
 *
 *  - Run `grep -A1 '[*] ----*$' | grep '[a-z]' | cut -c4-`
 *    to make a list of all the comment section headings.
 *
 *  - Note in particular the "Essay" sections at the bottom of the file,
 *    explaining the solver and generor and proving NP-completeness of the
 *    8-neighbour variant.
 *
 *  - The numbers in the grid are sometimes 0-based and sometimes 1-based.
 *    I've tried to be consistent in naming variables `value0` and `value1`,
 *    with a `1` at the end of function names where applicable.
 *
 *  - I've tried to be consistent about other variable names as well:
 *    - `x` and `y` refer to the coordinates of squares (column and row).
 *    - `index` refers to the index of a square (always 0-based), i.e. yw+x.
 *    - variables of type `square` store the contents of squares: either empty
 *      or a clue or user-supplied number.
 *    - `value0` and `value1` represent the numbers in the squares.
 *
 * If "future work" ever happens here's some stuff I would like:
 *
 *  - A proof that the Hamiltonian path algorithm always terminates.
 *    - Alternatively we can start with a default Hamiltonian path, zig-sag
 *      scan lines for examples, and "shuffle" it with Pósa rotations.
 *
 *  - NP-completeness proofs of all variants.
 *
 *  - New variant: some grid squares can be "holes", i.e. no number goes
 *    there.  This is NP-hard for the 4-neighbour variant.  Note: when we
 *    generate a game we currently assume all grids have Hamiltonian paths.
 *    We should ensure the grids with holes are Hamiltonian by construction,
 *    or else cope with them not being Hamiltonian.
 *
 *  - New variant: 6 neighbours ("Beehive Hidato").
 *
 *    - A square grid with four neighbours plus parallel diagonal edges, i.e.
 *      the first 6 elements of `neighbour_dxy`, has a hex grid topology.
 *    - You probably want non-rectangular grid shapes.  Those can be embedded
 *      inside a rectangle with holes; that's why I want the "holes" variant.
 *    - Displaying such grids hexagonally can be a purely UI-layer thing.
 *    - Designing a good keyboard interface is probably devilish.
 *    - A proof (or refutation) of NP-completeness would be nice.
 *
 *  - More deductions, at least for the recursive solver:
 *
 *    - https://en.wikipedia.org/wiki/Dulmage%E2%80%93Mendelsohn_decomposition
 *      lets us do set elimination in polynomial time; see specifically the
 *      parts about #The_fine_decomposition.
 *
 *    - We can do local set elimination: if we know the number x in a square,
 *      and x-1 and x+1 are each possible in just two squares, and it's the
 *      same two squares, then we can rule out everything except {x-1, x+1} in
 *      those two squares.
 *
 *    - Given two clues x and y with no in-between numbers on the board, one
 *      might be able to deduce that a given square s must contain some number
 *      z with x < z < y, i.e. all other numbers can be excluded.  This can be
 *      deduced if all paths from x to y either go through s or are too long.
 *
 *    - With 4 or 6 neighbours the grid graph is planar.  For all numbers x on
 *      the grid with either x-1 or x+1 _not_ on the grid, we might view x as
 *      a vertex and all the numbers up (down) to the next clue as forming an
 *      edge. Calculate a planar embedding of the whole graph, including the
 *      outer face; for each vertex construct a clockwise ordered adjacency
 *      list of neighbours.  Combine clockwise-of relationships with existing
 *      knowledge to rule out possibilities.  Note that we can add edges
 *      between (even diagonally) adjacent filled squares, so long as the
 *      solution plus the extra edges is guaranteed to be planar; adding a
 *      cycle to a 2x2 block of adjacent filled squares might constrain the
 *      planar embedding more, such that we learn more.
 *
 *    - If an empty square can be shown to lie on some particular face in all
 *      planar embeddings then it must be part of one of the edges of that
 *      face---there are _only_ 'edge numbers' left to fill the empty squares.
 *      In the empty square we can rule out all the numbers which cannot be
 *      part of the boundary of the face in question.
 *
 *    - Do fully general topological reasoning, see unfinished/path.c for
 *      inspiration.
 *
 *    - The current generator assumes there is exactly one difficulty level
 *      ("uniquely solvable"), which makes the acceptability of clue sets a
 *      monotonic function.  Fewer clues can obviously produce an unsolvable
 *      puzzle (e.g. give no clues); more clues can remove the need for the
 *      more advanced deductions.  With relatively few difficulty levels,
 *      maybe the spread between the levels is such that we can almost always
 *      remove more clues if we enable more deductions, but it's a thing to
 *      consider if we ever add difficulty levels.
 *
 *    - The standalone solver is fast enough for 10x10:35xyyx17 but becomes
 *      progressively slower as we increase 35, even just to 43.  This is
 *      because the solver enters an unsolvable state but does exponentially
 *      much work before discovering the unsolvability.  More deductions means
 *      better pruning which means faster solving.  One hopes.
 *
 * My go-to test cases for the recursive solver:
 *
 *  - 10x10:35xyyx17
 *  - 9x9:t1a63a45m77c23m9a19a31t
 *  - 9x9:j25_22a16a8_1b24e2k36e54k42e58b41_78a68a70_59j
 *  - 9x5:2vv
 *
 */

/* ----------------------------------------------------------------------
 * Essay: Debugging and developing this
 * ----------------------------------------------------------------------
 *
 * Some advice if you want to modify this file.
 *
 * Q: How do I debug this?
 * A: general advice applies, see questions below for more specific advice.
 *    - Make the bug reproducible:
 *      - Find a random seed which exhibits a bug in the generator
 *      - Find a game description which exhibits a bug in the solver
 *      - Find a sequence of UI inputs which exhibits a bug there
 *    - Reduce the amount of noise around the bug:
 *      - Try finding smaller and smaller puzzles which exhibit the bug
 *      - See if a generator bug happens without symmetry and clues 1 and n
 *      - Try eliminating steps from the UI input sequence
 *    - Find a span of code such that the bug happens after A and before B
 *      - Then repeatedly try to cut this 'interval' in half, approximately
 *      - Try to narrow it to specific functions, or rule functions out
 *    - Collect observations to reconcile your mental model with the bug
 *      - Define `TIMING`, `PRINT_FULL_SOLVER_STATE` and/or `TRACE_UI`.
 *      - Use `print_solver` and `puts(game_text_format(state))` if relevant.
 *      - After debugging remove the calls to `game_text_format` to not leak.
 *
 * Q: How do I debug the solver?
 * A: - Read "Essay: Solver" below to know what you're dealing with.
 *    - Define `CHECK_SOLVER_INVARIANT` to catch the multiple sources of truth
 *      disagreeing with each other.
 *    - Define `CHECK_SUFFICIENT_CLUES_INVARIANT` and set `GENERATOR_SOLVERS`
 *      to 2 to catch cases where `exclude` behaves differently depending on
 *      the order in which clues are learned.
 *    - Run the standalone solver in verbose mode and judge whether each step
 *      looks reasonable to you.
 *    - Does it only happen on unsolvable puzzles, like 9x5:2vv?
 *    - Does it only happen on multi-solution puzzles, like 9x5:1vv?
 *    - Does it only happen on single-solution puzzles which need recursion,
 *      like 9x9:t1a63a45m77c23m9a19a31t?
 *    - Does it happen on single-solution puzzles which need no recursion,
 *      like 9x5e0:b35c3b40c16_15i21k43h?
 *    - Does it depend on symmetry, clueing 1 and n, parity of w, h, n?
 *
 * Q: How do I debug the generator?
 * A: - Read "Essay: Generator" below to know what you're dealing with.
 *    - Rule out problems in the solver; to help with this, define macros
 *      `CHECK_SOLVER_INVARIANT` and `CHECK_SUFFICIENT_CLUES_INVARIANT`.
 *    - Define `CHECK_PICK_MINIMAL_CLUE_SET_POSTCONDITION` to automatically
 *      catch invalid results at the end of the generation process.
 *    - Disable recursion by setting `GENERATOR_SOLVERS` to 2, to rule out
 *      problems in the recursion logic.
 *    - Set `GENERATOR_SOLVERS` to 3 to do just a single level of recursion,
 *      if you think the problem is in the recursion logic.  More recursion
 *      will just make the traces more confusing if one level is enough.
 *    - Alternatively set `GENERATOR_SOLVERS` very high and generate a large
 *      puzzle, if exhibiting the bug requires a rare recursion corner case.
 *    - Add any newly added local state to the `TIMING` trace.
 *    - Does it depend on symmetry, clueing 1 and n, parity of w, h, n?
 *
 * Q: Where should I add global deductions to the solver?
 * A: The answer for the generator and the recursive solver probably differ:
 *    - For the recursive solver, at the top of every recursive call.  This
 *      will automatically include one call after learning all the clues.
 *      This should be before we check the solution count; the search for a
 *      split requires us to return early if the puzzle is not ambiguous.
 *    - For the generator it's tricky.  The good performance comes in part
 *      from the solver being reasonably fast when run incrementally.
 *      - At a minimum when concluding that a clue is necessary you must know
 *        that it actually _is_ necessary, e.g. by doing the global deduction.
 *        (If it isn't then you can make progress by discarding the clue.)
 *      - Exception: necessary clues learned when a recursive call returns
 *        have already been proven necessary; you don't need to re-check.
 *        (This might be a good macro-conditional assertion, though.)
 *      - Depending on the cost/learning ratio you could consider doing a run
 *        at the top of every recursive call to `shrink_clue_set`.
 *      - If it's cheap you could do one for every learned clue, or every time
 *        we learn a necessary clue, or when we shrink `clues->sufficient`.
 */

/* ----------------------------------------------------------------------
 * Essay: Solver
 * ----------------------------------------------------------------------
 *
 * Like many other puzzles, the Hidoku solver consists of one part which runs
 * a bunch of deductions to a fixed point, plus a recursive part which picks a
 * predicate ("square i contains value v") and runs two timelines, one where
 * it's true and one where it's false.  The law of excluded middle shows the
 * search to be exhaustive.
 *
 * The fixpointer is `exclude`, the guesser is `solve_recursively`.
 *
 * We fixpoint the following deductions, for each square i and value v:
 *
 *  - "Naked singles": if we have ruled out {1..n} \ {v} at i
 *    then rule out {v} at {0..n-1} \ {i}.
 *
 *  - "Hidden singles": if we have ruled out v at {0..n-1} \ {i}
 *    then rule out {1..n} \ {v} at square i.
 *
 *  - "Adjacency down": if 1 < v and we have ruled out v-1 in i's neighbours
 *    then rule out v at i.
 *
 *  - "Adjacency up": if v < n and we have ruled out v+1 in i's neighbours
 *    then rule out v at i.
 *
 *  - "Adjacency overlap": if v-1 and v+1 are each possible in a single
 *    neighbour of i but it's the same neighbour then rule out v at i.
 *
 * Note that these can all be stated as dual Horn clauses, i.e. implications.
 * (Technically some deductions imply the impossibility of several placements
 * so those are really conjunctions of dual Horn clauses).  One can determine
 * the satisfiability of a (dual) Horn formula, i.e. a conjuction of (dual)
 * Horn clauses, in linear time using the marking algorithm: initialize all
 * bits to "uncertain"; mark all the premise-free conclusions (clues) as
 * "certain"; when the premises of an implication are certain then mark the
 * conclusion as certain as well.
 *
 * Concretely, a zero at `solver->possible[_]` means "certainly impossible"
 * and a non-zero value means "not refuted yet, i.e. uncertain".
 *
 * If you like you can think of `solver->possible` progressing through an
 * increasing sequence of elements of a semilattice which orders non-zero
 * before zero---until reaching a deduction fixpoint.  This makes many proofs
 * easy, e.g. that we can test deductions and execute conclusions in any order
 * we like; we'll reach the same fixpoint either way.
 *
 * Detecting that all bits in a premise are marked "certain" can be done by
 * counting how many bits are uncertain and reacting when this reaches zero.
 * This is exactly what the neighbour counting in `solver->possible` does.
 *
 * In `.possible_values_in_square` and `.possible_squares_for_value` we do
 * _approximately_ this for the singles deductions.  Ruling out all but (e.g.)
 * 41 and all but 42 in some square i is almost the same condition; instead of
 * having one counter per value per square we just have one per square.  When
 * it reaches "only one possible value left" we find out _which_ value and go
 * execute the conclusion.  [And one counter per value for hidden singles.]
 *
 * Except there's a wrinkle: when we execute a 'singles' conclusion we might
 * find that _all_ squares have been ruled out for our value `v`, which can
 * make us not trigger the deduction's conclusion when we "should".  This is
 * not a problem: if a value `v` becomes impossible everywhere the adjacency
 * rules will make all values impossible everywhere, as designed.  We might
 * say the adjacency rules cover for the hidden singles rule in this case.
 *
 * There is no corresponding cover if we rule out all values in a square `i`.
 * This is acceptable: by calling `learn_contents_of_square` before calling
 * `learn_location_of_value` we make sure there are no recursive calls to
 * `exclude` before `learn_contents_of_square` finds its value `v` and rules
 * out `v` everywhere else.  If `v` is also ruled out at `i`, i.e. all values
 * are ruled out at `i`, then `v` gets ruled out everywhere and eventually all
 * values get ruled out everywhere, as designed.
 *
 * The subtle part here is that we _must_ call `learn_contents_of_square`
 * before calling `learn_location_of_value`.  Note that this only matters on
 * puzzles with no solutions, and finding examples which trigger the bug if
 * the two functions are called in the wrong order is not completely trivial.
 * One recipe is w odd, h odd, 9 <= w, 5 <= h, a '2' in the top left corner,
 * with 4 neighbours.  (Set elimination would notice the wrong parity.)
 *
 * One additional subtlety: `.possible_squares_for_value[_]` is decremented
 * _after_ the recursive calls made in `learn_contents_of_square` and may thus
 * overstate reality.  It eventually catches up; when there are no `exclude`
 * calls on the stack the data is correct.
 *
 * The `assert(value0 < solver->n)` in `learn_contents_of_square` will most
 * likely catch you if you reorder the calls.  So will both `solver_invariant`
 * and the `assert(score);` in `pick_ambiguity` in the recursive solver.
 *
 * Note that where latin squares do many small overlapping bipartite matchings
 * between numbers and squares, hidoku does one large matching.  For example,
 * grids with 200 squares need 40 kilobytes for `solver->possible`.  This is
 * relatively large, compared to other puzzles and to common L1 cache sizes.
 * This makes an elaborate and unconventional algorithm worth it; we won't get
 * nearly as good performance otherwise---I tried and I measured.
 */

/* ----------------------------------------------------------------------
 * Essay: Generator
 * ----------------------------------------------------------------------
 *
 * Hidoku has important features in common with e.g. Kurodoko (range):
 *
 *  1. The clues come from a set of candidates determined by the solution
 *  2. Each can be given as a clue or not, independently of the others
 *  3. Giving all candidates as clues ensures a unique solution
 *  4. Monotonicity: adding more clues never reduces solvability
 *
 * (Puzzles with multiple difficulty levels are often non-monotonic: adding
 * clues can make the puzzle too easy.  Some puzzles, e.g. Pattern, have a
 * fixed structure of clues without free choices.  Some puzzles, e.g. Pearl,
 * have solutions such that giving all candidate clues is insufficient.)
 *
 * Thanks to these characteristics, starting from a solution with all clues
 * given we could erase one square at a time in a random order and verify that
 * unique solvability is preserved, putting the number back if not.
 *
 * This works, but for performance reasons we instead do this:
 *
 *  1. Add clues to an empty grid until there's only one solution.
 *
 *  2. Discard all the candidate clues we haven't added yet.  We're deciding
 *     to never give any of those clues.
 *
 *  3. The last clue we added is necessary, given the decision in step 2:
 *
 *     - all the other given clues are not enough, or the loop in step 1 would
 *       have stopped one iteration earlier.
 *
 *     - the last clue is the only remaining piece of infomation we can add,
 *       since we threw out everything else in step 2.
 *
 *  4. If the necessary clues are not enough to determine a unique solution
 *     then repeat from step 1.  Add all necessary clues before adding any
 *     uncertain clues.  (Then each iteration learns one necessary clue.)
 *
 * Upon termination the set of surviving clues are all necessary; together
 * they are sufficient to uniquely determine the given solution.
 *
 * With a solver where one can add clues but not remove them, "erase one
 * clue" really means "add n-1 clues, then test for solvability".  If we can
 * quickly test for solvability after adding each clue then our method gives
 * performance gains: we can stop after adding significantly fewer than n-1
 * clues, and when the first iteration stops early we can discard much more
 * than just a single clue; this drastically cuts down the iteration count.
 *
 * The above is a simplification; we include more performance enhancements:
 *
 *  1. We use one solver which learns only the necessary clues, then copy it
 *     into a second scratch solver which finds the next necessary clue.  (It
 *     is faster to copy a solver than it is to re-learn the necessary clues.)
 *
 *  2. When considering a candidate clue we check if the solver has already
 *     deduced the contents of the candidate clue square.  If so then giving
 *     it as a clue would not add information; the clue cannot be necessary,
 *     so discard it immediately.  In case of symmetry, all squares in the
 *     symmetry class must be solved for us to discard it.  (Paying for the
 *     check is faster than repeatedly looping to learn a no-op clue.)
 *
 *  3. If we have given all clues except the final candidate we don't have to
 *     give that clue and ask the solver whether the solution is unique: in
 *     the first iteration we know that giving clues in all squares makes the
 *     solution unique; in later iterations we know from the previous round
 *     that giving all _survining_ candidate clues is sufficient.  (Testing
 *     whether there's only one clue left is faster than running the solver.
 *     The last clue in particular tends to trigger a lot of deduction work.)
 *
 *  4. When we have given half of the uncertain clues we freeze-frame the
 *     current state of the solver and make a copy of it.  Using the copy we
 *     determine which of the _remaining_ candidate clues are necessary,
 *     keeping all the freeze-framed clues on the board at all times.
 *
 *     Once the copy is finished we mark the suffix of previously uncertain
 *     clues as necessary, then resume the freeze-framed solver's loop with
 *     the frozen prefix of uncertain clues now thawed and up for grabs.
 *
 *     (By copying a snapshot in which ~half the clues are already learned we
 *     save the effort of re-learning them in every iteration, which outweighs
 *     the cost of additional copying back and forth.)
 *
 *  5. We apply the above technique recursively, freeze-framing the base
 *     solver with half of the uncertain clues on the board, freeze-framing
 *     the copy with three quarters of the uncertain clues on the board,
 *     freeze-framing the copy of the copy with 7/8ths of the uncertain clues
 *     on the board, and so on.  (If it's good once it's good n times.)
 *
 *  6. In order to freeze-frame at a sensible point in time when all we know
 *     is that the set of all clues is sufficient, we calculate an estimate of
 *     how many clues will produce the first solved board; this estimate is
 *     used to determine when to freeze-frame and recurse.  (Splitting half
 *     way to sufficiency is ~best; the size of the previous sufficient set
 *     only gives a _good_ approximation of "half way" once we've thrown out a
 *     non-trivial set of clues, so in the first iteration we have to make a
 *     guess; we base it on observed averages, which beats "n I guess".)
 *
 * (The parentheses give my guesses at why the tweaks result in observable
 * performance improvements.)
 *
 * One way of understanding the recursive pattern: if we put the candidate
 * clues into a binary tree and calculated a solver state at each subtree
 * including all clues below it and all deductions from them, we could test
 * the removal of a single clue by: combining a logarithmic number of subtree
 * solver states; running deductions; and seeing if the instance is solved.
 * The recursion does approximately that, always testing the rightmost leaf
 * and moving the leaves around such that a new clue becomes the rightmost
 * leaf, in a way that minimizes recalculation.  Instead of combining solver
 * states for each leaf, we pass state down the tree and mutate it, restarting
 * from the correct place when we move clues from right to left.
 *
 * The generator keeps track of the necessary clues by moving them into a
 * prefix of `clues->classes`.  It keeps track of the smallest set of clues
 * known to be sufficient in a longer prefix of `clues->classes`, whose length
 * is given by `clues->sufficient`.
 *
 * In case of symmetry, replace "clue(s)" with "clue class(es)" in the above.
 *
 * Define the `TIMING` macro and run `hidoku --generate 1` to see a trace of
 * the generator working.  Set GENERATOR_SOLVERS to 2 to disable the freeze
 * frame logic (4-6).  Set it to 3 to do just one layer of freeze frames.
 */

/* ----------------------------------------------------------------------
 * Essay: Computational complexity
 * ----------------------------------------------------------------------
 *
 * The four-neighbour variant with holes is NP-complete, (thm 2.1, cor 1) in
 * Hamilton Paths in Grid Graphs, by Itai, Papadimitriou and Szwarcfiter,
 * https://www.researchgate.net/publication/220616693_Hamilton_Paths_in_Grid_Graphs.
 *
 * This lets us prove the 8-neighbour variant without holes is NP-complete,
 * see https://cs.stackexchange.com/questions/11330/is-hidoku-np-complete.  We
 * adapt this proof to show NP-completeness even for four-way symmetric clues
 * and even if we have to clue 1 and n.  We don't require the hidoku grids
 * produced by the reduction to be square.
 *
 * The idea is to take a hamiltonian circuit problem on grid graphs and turn
 * it 45 degrees, embedding it in an 8-neighbour puzzle. Put clues in the dead
 * space, i.e. all squares except those of the input graph.  For a graph with
 * k vertices we fill dead space with 1..n-k, except we insert a jump of size
 * k next to an outer edge of the given graph.  To give clues 1 and n and to
 * keep clues symmetric, we make four copies of this construction.
 *
 * Let's look at an example.
 *
 * The input graph has 14 vertices (indicated with 'x'):
 *
 * xxxx
 * x.xx
 * x.xx
 * xxxx
 *
 * Here's a game description:
 *
 * 1_2_41_42_43_44_71_72_3_4_40a45_46_69_70_5_6a39a47_67_68_7a38a48a65_66a8_36_37a49a64_9a34_35_50a62_63_24_25a33a51_60_61_26_27_32a52_53_58_59_28_29_30_31_54_55_56_57
 *
 * Here's the grid (notice the jump from 9 to 24):
 *
 * +----+----+----+----+----+----+----+----+
 * |  1 |  2 | 41 | 42 | 43 | 44 | 71 | 72 |
 * +----+----+----+----+----+----+----+----+
 * |  3 |  4 | 40 |    | 45 | 46 | 69 | 70 |
 * +----+----+----+----+----+----+----+----+
 * |  5 |  6 |    | 39 |    | 47 | 67 | 68 |
 * +----+----+----+----+----+----+----+----+
 * |  7 |    | 38 |    | 48 |    | 65 | 66 |
 * +----+----+----+----+----+----+----+----+
 * |    |  8 | 36 | 37 |    | 49 |    | 64 |
 * +----+----+----+----+----+----+----+----+
 * |  9 |    | 34 | 35 | 50 |    | 62 | 63 |
 * +----+----+----+----+----+----+----+----+
 * | 24 | 25 |    | 33 |    | 51 | 60 | 61 |
 * +----+----+----+----+----+----+----+----+
 * | 26 | 27 | 32 |    | 52 | 53 | 58 | 59 |
 * +----+----+----+----+----+----+----+----+
 * | 28 | 29 | 30 | 31 | 54 | 55 | 56 | 57 |
 * +----+----+----+----+----+----+----+----+
 *
 * The general idea is to add a padding row above and below the 45 degree
 * rotated input plus an optional padding column if the number of columns is
 * odd.  Then traverse the columns in pairs, alternating top-down vs. bottom
 * up, putting clues 1..n (except for the blanks) in squares as we visit them.
 * Note that the blanks only have other blanks as _diagonal_ neighbours, so
 * our traversal is never blocked.
 *
 * Pick an edge (u, v) on the outside of the input graph, such that one vertex
 * is adjacent to only one square of the traversal and a neighbouring square
 * is adjacent to at most {u, v} but no other vertices.  Insert a jump there;
 * in our example from 9 to 24.  The delta is k+1.
 *
 * To convert this into a four-way symmetric puzzle, add a padding column if
 * there isn't already one.  Then add a horizontally mirrored copy of the
 * puzzle on the right (clues 73 to 144).  Then add a vertically mirrored copy
 * of these two copies (clues 145 to 288), in our case on top; add the copy on
 * the side where the largest-so-far clue was placed.
 *
 * Each quarter of the puzzle is solvable iff the input graph is hamiltonian,
 * and thus the whole puzzle is solvable iff the input graph is hamiltonian.
 */

/* ----------------------------------------------------------------------
 * Essay: Correctness of instance generation and `validate_params_full`
 * ----------------------------------------------------------------------
 *
 * We want `new_game_desc` to generate good boards, by which we mean it has at
 * least one empty square; exactly one solution is consistent with the clues;
 * and the clues are chosen in according with the parameters (that is, they
 * satisify the symmetry requirements and `clue_1_and_n`).
 *
 * The task of `validate_params_full` is to produce an error message for those
 * parameters which do not admit any good boards.
 *
 * Some parameters are such that no clue removal is consistent with the
 * requested constraints, i.e. only already-solved puzzles are allowed.  We
 * want to error on those and we do.  (Those are `clue_1_and_n && n < 3`.)
 *
 * Other parameters are such that removing any symmetric set of clues from a
 * full board is guaranteed to create an ambiguous puzzle.  We want to error
 * on those as well.  This is a heterogeneous but finite and small class, and
 * we'll show that `validate_params_full` catches exactly those.
 *
 * A few parameters are such that for some hamiltonian paths a good board can
 * be found, but other hamiltonian paths no not admit any good board.  We'll
 * show that `may_need_rejection_sampling` catches exactly those.  To prove
 * that a set of parameters is in this class, we'll exhibit one hamiltonian
 * path which admits of a good board and one which doesn't.
 *
 * The remaining parameters are such that all hamiltonian paths admit at least
 * one good board.  We'll constructively show a set of squares which can be
 * emptied to produce an unambiguous puzzle.  That work is the bulk of this
 * comment.
 *
 * Note that the actual generation in `new_game_desc` is very likely to
 * produce a different set of empty squares; that's okay, we want at least one
 * non-empty set of squares to exist, so long as `new_game_desc` could produce
 * that set or a superset of it.  To prove the existence of such a set we'll
 * define a solver algorithm weaker than the one used by `new_game_desc`.  If
 * it can solve a given set of empty squares then the real solver can also
 * solve that set of empty square, and then `new_game_desc` could output that
 * set or a superset thereof.
 *
 * Let's define some terms then go over the algorithm.
 *  - by 'clue' we mean a value in any non-empty square
 *    (even values added by the algorithm during the solving process)
 *  - a number `x` is 'missing' if 1 <= x <= n and x is not a clue
 *  - A clue `x` is 'relevant' if either x-1 or x+1 is missing
 *  - A relevant clue is 'ambiguous' if it is adjacent to 2+ empty squares
 *  - A relevant clue is 'forcing' if it is not ambiguous
 *  - An empty square is 'tangled' if it's adjacent to an ambiguous clue or at
 *    least two empty squares
 *  - An empty square is 'isolated' if it is not tangled
 *  - Let a set of squares S be given and let T be the set of squares
 *    { s | s is symmetric to some square in S } for a contextually given
 *    symmetry.  We say that S is 'self-symmetric' if S = T, and that S is a
 *    'symmetry class' if no non-empty proper subset of S is self-symmetric.
 *    Note: symmetry classes are equivalence classes of the 'is-symmetric-to'
 *    relation (with respect to the contextually given symmetry).
 *  - We sometimes identify numbers with the squares containing them,
 *    e.g. "the n-1 square" is the square containing the number n-1.
 *
 * A short interlude analysing symmetry classes:
 *  - If there's no symmetry then every symmetry class is a singleton
 *  - If w and h are both odd the center square is a singleton symmetry class
 *  - If w is odd the (other) squares in the centre column get paired up
 *  - If h is odd the (other) squares in the centr row get paired up.
 *  - For k-fold symmetry, no symmetry class has more than k members, and only
 *    the above exceptions have fewer than k members.
 *
 * Here's the simple solver algorithm:
 *  - If n=1 then put 1 in the only square; else do the remaining steps
 *  - Let x be any forcing clue
 *  - Either x-1 or x+1 is missing, so x is adjacent to an empty square
 *  - At most one is missing since x is adjacent to only one empty square
 *  - Put the missing number in the empty square adjacent to x
 *  - Repeat
 *
 * This solver is weaker than the real one (i.e. `exclude`): when the simple
 * solver places y next to x (with y in {x-1, x+1}) the adjacency deductions
 * of the real solver would rule out y in all squares except the empty square
 * next to x, de facto placing y there.  Thus all deductions performed by the
 * simple algorithm can also be performed by the real algorithm.
 *
 * The simple algorithm will of course not work for all puzzles, but we will
 * show that it works for those used to prove `validate_params_full` correct.
 * It can fail if there are no forcing clues initially, or if filling in an
 * empty square creates a situation with no forcing clues.  For example:
 *
 * +---+---+---+
 * | 1 |   |   |
 * +---+---+---+ (4 neighbours)
 * | 2 |   |   |
 * +---+---+---+
 *
 * Here 2 forces the placement of 3 (lower middle), but 3 is ambiguous and
 * there are no forcing clues when 3 has been placed.
 *
 * Proposition:
 *   If the puzzle has a solution and solver algorithm fills up empty squares
 *   until all squares are full, the algorithm will produce the only solution.
 * Proof:
 *   When the algorithm places a number y in an empty square next to a forcing
 *   clue x, if y was placed anywhere else it would violate the rule that y
 *   has to be a neighbour of x; so y must go where the algorithm places it in
 *   all solutions.  If the algorithm fills the board then all solutions must
 *   be equal to the one found by the algorithm, i.e. there's just the one.
 *   []
 *
 * Proposition:
 *   All boards with both empty and non-empty squares have at least one
 *   relevant clue (assuming the board is solvable).
 * Proof:
 *   If 1 is missing then let y be the smallest clue.
 *   Then y is a relevant clue since 1..y-1 are all missing.
 *   Otherwise, let z be the smallest missing number and let w = z-1.
 *   Then w is a relevant clue since w+1 is missing.
 *   (y-1 and w+1 must be adjacent to y and w since the board is solvable)
 *   []
 *
 * Proposition:
 *   If all empty squares are isolated and there's at least one non-empty
 *   square and the puzzle has a solution, the solver algorithm will find the
 *   solution.
 * Proof:
 *   If no square is empty the puzzle is already solved.
 *   If there's at least one empty square then there's at least one relevant
 *   clue (there's a non-empty square by assumption).  Since all empty squares
 *   are isolated the clue is unambiguous and thus forcing.
 *   Thus the algorithm will fill at least one square.
 *   After doing so, all remaining empty squares (if any) are isolated:
 *   The square we filled did not become an ambiguous clue because it had at
 *   most one empty neighbour; filling squares does not add empty neighbours
 *   to empty squares or clues, so all other clues remain unambiguous and all
 *   remaining empty squares still have at most one empty neighbour.
 *   Thus the algorithm will keep filling squares until the puzzle is solved.
 *   []
 *
 * Proposition:
 *   If there's exactly one empty square then all empty squares are isolated.
 * Proof:
 *   It can't have any empty neighbours since it's the only empty square.
 *   No clue is ambiguous since that requires two empty squares.
 *   (If there's only one square it has no neighbours and is thus isolated)
 *   []
 *
 * Proposition:
 *   If there's exactly one empty square and the puzzle has a solution, the
 *   solver algorithm will find the solution.
 * Proof:
 *   Combine the previous two propositions.  (Corner case: if there's only one
 *   square and it's empty the solver finds the solution by hard-coding it.)
 *   []
 *
 * Proposition:
 *   If neighbours=4 and n > 4 and the puzzle has a solution and there are at
 *   most two empty squares and neither 1 nor n are missing, the algorithm
 *   will solve the puzzle.
 * Proof:
 *   If there are fewer than two empty squares we have already shown that the
 *   solver algorithm will solve the puzzle, so assume two empty squares.
 *   Since n > 2 there are also non-empty squares and thus a relevant clue.
 *   If the two empty squares are neighbours they have no common neighbours
 *   and thus no clue is ambigous, so the clue is forcing.  Thus the algorithm
 *   will fill one square; with only one empty square left the algorithm will
 *   fill the second square as well.
 *
 *   Assume the empty squares are not neighbours and they contain x and y with
 *   1 < x < y < n.  Since they are not neighbours it's not possible for y to
 *   be the successor of x (else the puzzle is unsolvable).  So x + 2 <= y,
 *   and thus 1 <= x-1 < x+1 <= y-1 < y+1 <= n, so at least three relevant
 *   clues have been given.  The x and y squares have at most two common
 *   neighbours, i.e. at most two out of the three clues are ambiguous, so at
 *   least one clue is forcing.  Hence the algorithm will fill at least one
 *   square, and the second iteration will fill in the other empty square.
 *   []
 *
 * Proposition:
 *   If 4 <= h and 4 <= w and there are clues in all squares except the
 *   corners then the empty squares are all isolated.
 * Proof:
 *   The corners are not neighbours of one another, and the neighbours of the
 *   corners are all adjacent to at most one corner (visualize this), hence no
 *   clue is ambiguous.
 *   []
 *
 * Proposition:
 *   If neighbours=4 and [h == 2 and 4 <= w or w == 2 and 4 <= h] and there
 *   are clues in all squares except the corners then the empty squares are
 *   all isolated.
 * Proof:
 *   Each corner is a neighbour of exactly one other corner. No clue is
 *   ambiguous, since no square is a neighbour of two or more corners.
 *   (This is not true if (w, h) in {(2, 3}, (3, 2)})
 *   []
 *
 * ----------------------------------------------------------------------
 *
 * With this background, we can begin proving that `new_game_desc` and friends
 * do what we want them to: `validate_params_full` rejects all the impossible
 * and always-ambiguous clues and passes all the other ones through, and that
 * `may_need_rejection_sampling` recognizes exactly those parameters where
 * some but not all hamiltonian paths can yield a good board.
 *
 * During this analysis we will assume WLOG that h <= w.  If w < h then flip
 * the board through a 45 degree line and swap horizontal/vertical symmetry.
 * You may want to check that when `validate_params_full` returns an error
 * message for a set of parameters with h < w it returns a related message for
 * the flipped board.
 *
 * In case analyses we'll often make use of the following symmetries: given a
 * hamiltonian path we can (a) "run it backwards": replace x with n + 1 - x
 * everywhere; (b) mirror the board through a horizontal line; and (c) mirror
 * the board through a vertical line.  [This justifies many useful WLOGs.]
 *
 * Note that the non-full `validate_params` ensures that 1 <= h <= w.
 *
 * Let the case analyses begin.
 *
 * ----------------------------------------------------------------------
 *
 * If h == 1:
 *   If clue_1_and_n:
 *     This is impossible and `validate_params_full` returns an error
 *   If w == 1 (and thus n == 1):
 *     All symmetries are satisfied if we empty the one and only square
 *   If w == n == 2:
 *     If we must give either all clues or no clues and n > 1, it is not
 *     possible to erase a symmetry class without rendering the puzzle
 *     ambigous, and `validate_params_full` catches exactly those cases.
 *     In case of a no-op symmetry we simply empty one square.
 *   If 2 < w:
 *     Note that the two corners contain 1 and n in all solutions
 *     Give the corners as clues and erase everything else.
 *     This satisfies all symmetries and is trivially solvable.
 *     (The leftmost empty square is always next to a forcing clue.)
 *
 * (Now we know 2 <= h <= w and thus 4 <= n.)
 *
 * If there are no symmetry requirement:
 *   All symmetry classes are singletons.
 *   Since 3 <= n there's at least one square containing neither 1 nor n.
 *   Empty that one square. [One empty square => solvable.]
 *
 * (Now we know 2 <= h <= w and symmetry != none, and still 4 <= n.)
 *
 * If neighbours == 4:
 *   If w == h == 2:
 *     We reject rotational and fourway symmetry
 *     Thus the symmetry is either horizontal or vertical
 *     WLOG assume 1 is in the top left corner; there are two solutions
 *     They encode as `2x2:1_2_4_3` and `2x2:1_4_2_3`.
 *     Note that the board divides into two symmetry classes (rows/columns)
 *     If `!clue_1_and_n`:
 *       Empty any symmetry class. All empty squares are isolated.
 *     If clue_1_and_n:
 *       ONLY SOME HAMILTONIAN PATHS CAN BECOME PUZZLES.
 *       If symmetry == horizontal:
 *         One solution has 1 and n in different rows, the other in the same.
 *         In the latter case, erase the row containing 2 and 3.
 *         All empty squares are isolated.
 *       Else:
 *         The symmetry is vertical.  Transpose the horizontal analysis.
 *
 *   If symmetry != fourway:
 *     5 <= n and each symmetry class contains at most two squares.
 *     Hence there are at least three symmetry classes.
 *     At least one symmetry class contains neither 1 nor n.
 *     Empty that symmetry class.  [At most two empty squares => solvable]
 *
 *   If `clue_1_and_n` is false:
 *     Empty a symmetry class based on w and h:
 *       - 3x2: empty the middle column
 *       - Wx2 with 4 <= W: empty the corners
 *       - 3x3: empty the middle square
 *       - Wx3: empty the left- and rightmost squares in the middle row
 *       - Otherwise, i.e. 4 <= h <= w: empty the corners
 *     Either way, all empty squares are isolated.
 *
 *   So `clue_1_and_n` is true and symmetry is four-way.
 *
 *   If 4 <= h (implying 4 <= w by assumption):
 *     Here are five symmetry classes:
 *       - The corners
 *       - The left- and rightmost squares in the one or two middle row(s),
 *         depending on whether h is odd or even.
 *       - The top and bottom squares in the one or two middle column(s),
 *         depending on whether w is odd or even.
 *     At least three exist on any particular board, depending on the parity
 *     of w and h.  At least one class contains neither 1 nor n.
 *     Empty one of those.  All empty squares are isolated.
 *
 *   If h == 3:
 *     If w is odd:
 *       n = 3 * w so n is odd
 *       The board as a whole contains one more odd number than even numbers.
 *       No row contains the same number of even and odd numbers (w odd).
 *       So the middle row must start and end with even numbers.
 *       Empty the left- and rightmost squares in the middle row.
 *       (Neither 1 nor n are even, so `clue_1_and_n` is satisfied)
 *       [At most two empty squares => solvable]
 *     If w is even:
 *       Consider the left- and rightmost squares in the middle row.
 *       If they contain neither 1 nor n then empty them.
 *       Otherwise, WLOG say 1 is in the leftmost square of the middle row.
 *       Call its vertical neighbours x and y, with x < y.
 *       If y < n then {x-1, x+1, y-1, y+1} go in the three squares adjacent
 *       that neighbour either x or y.
 *       But x+1 != 1 != y+1, so x-1 and y-1 must both go in the square which
 *       contains 1; i.e. x-1 = y-1 = 1 so x = y = 2, but x < y.
 *       So n is a vertical neighbour of 1 as y < n leads to a contradiction.
 *       Empty the two middle squares of the middle row.
 *       (They contain neither 1 nor n: those are in the leftmost column.)
 *       [At most two empty squares => solvable]
 *
 *   If h == 2:
 *     We already ruled out w == 2.
 *
 *     If neither 1 nor n is in a corner:
 *       If w == 3 then there's no hamiltonian path with both 1 and n in the
 *       middle column, so w != 3.  Thus 4 <= w.
 *       Empty the corners.  All empty squares are isolated.
 *       (The middle column clues would be ambiguous if w == 3.)
 *
 *     So assume WLOG that 1 is in the top left corner.
 *
 *     If w == 3 or w == 4:
 *       There are two symmetry classes: the corners, and everything else.
 *       It is possible for 1 and n to be in different symmetry classes.
 *       ONLY SOME HAMILTONIAN PATHS CAN BECOME PUZZLES.
 *       The solution admits a symmetry class which can be erased if e.g. n is
 *       a vertical neighbour of 1, but not if n is a horizontal neighbour.
 *       So `3x2:1_2_3__6_5_4` and `4x2:1_2_3_4__8_7_6_5` are good,
 *       but `3x2:1_6_5__2_3_4` and `4x2:1_8_7_6__2_3_4_5` are bad.
 *       In what follows, assume we're dealing with a good board.
 *       Empty the non-corner symmetry class.
 *     Else (i.e. 5 <= w):
 *       There are at least three symmetry classes, since 10 <= n and each
 *       symmetry class has at most four squares.  At least one contains
 *       neither 1 nor n.  Empty an arbitrary one of those.
 *
 *     With 1 and n given as clues the solution is unique, even if we were not
 *     given any other clues. (Recall that 1 is in the top left corner.)
 *
 *     If 1 and n are both in the left column, the second column must contain
 *     2 and n-1, the third column 3 and n-2, and so on.  In other words, the
 *     top row contains [1..(n/2)] and the bottom row [n..(n/2)+1].
 *
 *     Otherwise consider this example:
 *     +---+---+---+---+---+
 *     | 1 | c | d |...|...|
 *     +---+---+---+---+---+
 *     | a | b | n |...|...|
 *     +---+---+---+---+---+
 *     Since square `a` contains some number x with x != n, it must be
 *     adjacent to x-1 and x+1.  Since 1 != x+1 it follows that x-1 = 1,
 *     giving us a = 2 and also b = 3.  By a similar logic, let y be the
 *     smallest relevant clue (now 3).  If the other square in y's column is
 *     empty then it must contain y+1, and the horizontal neighbour of y+1
 *     must contain y+2.  Repeat this deduction until we place some y+2 above
 *     or below n.
 *
 *     Consider the sub-puzzle that has d and n in its leftmost column.  By
 *     the same logic as when 1 and n are in the leftmost column, the top row
 *     contains the smallest half of the numbers and the bottom row contains
 *     the largest half of the numbers, deduced one column at a time.
 *     (It can happen that this sub-puzzle only has one column, if n is in the
 *     rightmost column.  Then the puzzle is solved when we get here.)
 *
 * This concludes the neighbours=4 section, so let's consider neighbours=8.
 *
 * Recall that we have already handled the no-symmetry and h=1 cases, as well
 * as the 2x2 case.  We'll analyse the remaining cases one symmetry at a time.
 *
 * (We'll often assume that `clue_1_and_n` is true.  Sets of emptied squares
 * which satisify this requirement also work when `clue_1_and_n` is false.)
 *
 * If symmetry == vertical:
 *   If h == 2:
 *     All symmetry classes are columns.  All empty columns are ambiguous.
 *     `validate_params_full` correctly returns an error in these cases.
 *   If h is odd:
 *     Observe that 3 <= h <= w.
 *     At least one square in the middle row contains neither 1 nor n.
 *     Empty that square.  [Only one empty square => solvable]
 *   So h is even, and thus 4 <= h <= w.
 *
 *   Find a column containing neither 1 nor n.  Empty the top and bottom
 *   squares in that column.  All empty squares are isolated.
 *
 * If symmetry == horizontal:
 *   If 3 <= h:
 *     If w is odd:
 *       Find a square in the middle column containing neither 1 nor n.
 *       Empty that square.  [Only one empty square => solvable]
 *     Otherwise 4 <= w.  Find a row containing neither 1 nor n.
 *     Empty the left- and rightmost squares in that row.
 *     All empty squares are isolated.
 *
 *   So h == 2 and 3 <= w.
 *
 *   If w is odd:
 *     No hamiltonian path has both 1 and n in the middle column.
 *     Empty one square in the middle column.
 *     [Only one empty square => solvable]
 *
 *   If 6 <= w:
 *     Here are three symmetry classes:
 *       - the top two corners
 *       - the bottom two corners
 *       - the horizontal neighbour of the top left corner, plus partner
 *     At least one contains neither 1 nor n.  Empty that one.
 *     All empty squares are isolated.
 *
 *   So w == 4.
 *
 *   If the bottom corners contain neither 1 nor n then empty them.
 *   Else, If the top corners contain neither 1 nor n then empty them.
 *   If we empty a pair of corners then the empty squares are isolated.
 *
 *   Otherwise, WLOG 1 is in the top left corner and n is in a bottom corner.
 *
 *   If n is in the bottom left corner then:
 *     - column 2 contains 2 and 7 (=n-1)
 *     - column 3 contains 3 and 6 (=n-1)
 *     - column 4 contains 4 and 5 (=n-1)
 *   Empty the middle two squares in any row.
 *   Either 1 or n will be a forcing clue.
 *
 *   Otherwise n is in the bottom right corner.
 *
 *   If 2 is in column 2 and is given as a clue then we can deduce that 3 goes
 *   in the bottom left and 4 goes above/below 2.  So if 2 is in column 2 then
 *   empty the middle two squares in the row not containing 2.  This makes 3 a
 *   forcing clue.  Do likewise if 7 is in column 3.
 *
 *   Otherwise column 1 is [1, 2] and column 4 is [7, 8].
 *   Empty the two middle squares in a row containing either 3 or 6 or both.
 *   Then either 2 or 7 or both will be forcing clues.
 *   After deducing either 3 or 6 there's only one empty square.
 *
 * If symmetry == rotational:
 *   If 4 <= h (and by assumption 4 <= w):
 *     Find a symmetry class contained in the top and bottom rows which
 *     contains neither 1 nor n (at most 2 out of 4 get ruled out).
 *     Empty those squares.  All empty squares are isolated.
 *
 *   If h == 3:
 *     If 4 <= w:
 *       Find a symmetry class contained in the left and right columns which
 *       contains neither 1 nor n (at most 2 out of 3 get ruled out).
 *       Empty those squares.  All empty squares are isolated.
 *
 *     So w == 3.  Here are three symmetry classes:
 *      - One pair of opposing corners
 *      - The other pair of opposing corners
 *      - The middle square
 *     At least one symmetry class contains neither 1 nor n.  Empty that one.
 *     Emptying the middle square is trivially solvable.
 *     If we empty two corners then the middle square is an ambiguous clue.
 *     Say x and y are missing with 1 < x < x+1 < y < n.
 *     [x+1 < y because x and y are not neighbours.]
 *     Then 1 <= x-1 < x+1 <= y-1 < y+1 <= n.
 *     Thus {x-1, x+1, y+1} are three distinct relevant clues.
 *     At most one is ambiguous, so at least one clue is forcing.
 *     With one missing number deduced the other will be trivial.
 *
 *   So only h == 2 remains.
 *
 *   If 5 <= w:
 *     Here are three symmetry classes:
 *      - One pair of opposing corners
 *      - The other pair of opposing corners
 *      - Column 2 row 1, plus partner ("c2r1")
 *     At least one symmetry class contains neither 1 nor n.  Empty that one.
 *     Empty corners are isolated.  So is c2r1 if 6 <= w.
 *
 *     If w == 5 then emptying c2r1 leaves two ambiguous clues in the middle
 *     column.  But we have three relevant clues among {x-1, x+1, y+1}, like
 *     the h == 3 case, so at least one is forcing.
 *
 *   If w == 4:
 *     Each symmetry class contains one square in the left half of the board
 *     and one square in the right half.
 *
 *     Swap each square in the right half with its vertical neighbour.
 *     Use the analysis of 4x2 with horizontal symmetry to empty two squares.
 *
 *     They are not rotationally symmetric.  (Un)swap each square in the right
 *     half with its vertical neighbour.  Now they're symmetric.
 *
 *     The horizontal symmetry analysis shows why this is solvable.
 *
 *   So only (w, h) == (3, 2) remains.
 *
 *   There are three symmetry classes:
 *     - The top left and bottom right corners
 *     - The bottom left and top right corners
 *     - The middle column
 *
 *   Emptying the middle column creates an ambiguous puzzle, so we must empty
 *   a pair of corners.
 *
 *   No hamiltonian path has 1 and n in the middle column, so assume WLOG that
 *   1 is in the top left corner.
 *
 *   If neither the bottom left corner nor top right corner contains n, then
 *   empty those two corners.  If x and y are missing with 1 < x < x+1 < y < n
 *   then {x-1, x+1, y+1} are three relevant clues.  Only the two squares in
 *   the middle column are ambiguous, so at least one clue is forcing.
 *
 *   So assume n is in a corner symmetry class not containing 1.
 *
 *   If `clue_1_and_n` is true:
 *     ONLY SOME HAMILTONIAN PATHS CAN BECOME PUZZLES.
 *     There is no symmetry class we can empty if we haven't found one yet.
 *     This can happen for e.g. `3x2:1_2_3__6_5_4`.
 *     However, for `3x2:1_3_5__2_4_6` we can erase 2 and 5.
 *
 *   So `clue_1_and_n` is false.  Erase 1 and x with x < n (since n is in one
 *   of the two corners that's not opposite to 1, else we're done already).
 *
 *   Note that x != 2 since x is not adjacent to 1.
 *
 *   If x-1 and x+1 do not both go in the middle column:
 *     One of them is placed above x and is a forcing clue.
 *   Else if x != 3:
 *     2 must be in the bottom left and is thus a forcing clue.
 *     (Note: x > 3 thus x-1 > 2 so 2 is not in the middle column, but 2 must
 *     be adjacent to 1 in the top left corner.)
 *   Else:
 *     The middle column contains 2 and 4.
 *     Then 5 and 6 go in non-neighbouring corners (the other four squares
 *     have all been accounted for), but that's not a solution.
 *
 * The only cases left are neighbours = 8 and symmetry == fourway.  Recall
 * that 2 <= h <= w and 4 < n since we have already handled the other cases.
 *
 * If h == 2:
 *   Squares are emptied one or more columns at a time, but empty columns are
 *   always ambiguous.  Note that `validate_params_full` correctly errors.
 *
 * If h == 3:
 *   If w == 3:
 *     If the center contains neither 1 nor n then empty the center.
 *     [One empty square => solvable.]
 *
 *     Otherwise, assume WLOG the centre contains 1.  If neither of the other
 *     two squares in the middle row contain n then empty them; otherwise
 *     empty the other two squares in the middle column.  Either way, three
 *     clues are ambiguous, either the middle row or middle column.
 *
 *     Let x and y be the missing numbers with 1 < x < x+1 < y < n.
 *     Then {x-1, x+1, y+1} are three distinct relevant clues.
 *
 *     If x+1 < y-1 then y-1 is a fourth clue, one of which must be forcing.
 *     Otherwise y = x+2.  If {x-1, x+1, y+1} are not all adjacent to both
 *     empty squares then one is a forcing clue.
 *
 *     If all three are ambiguous then 1 is the smallest of them, 1 = x-1.
 *     So x=2, y=4 and the middle row/column has 1, 3, 5.  But then 6..9 go in
 *     the four corners and are not adjacent, i.e. this is no valid solution.
 *
 *     So all solutions can be turned into solvable instances.
 *
 *   Now consider h == 3 and 4 <= w.
 *
 *   Try emptying the left- and rightmost squares in the middle row (they
 *   would be isolated).  If they contain neither 1 nor n we're done.
 *
 *   Otherwise WLOG assume 1 is leftmost in the middle row.
 *
 *   If n is not in a corner then empty the corners.  For each pair of corners
 *   x and y in the same column they have two shared neighbours, which cannot
 *   contain all of {x-1, x+1, y+1} so one of those is forcing.  This lets us
 *   fill one leftmost corner and one rightmost corner, isolating the last two
 *   corners [i.e. this produces a solvable puzzle].
 *
 *   Name the squares near 1 as indicated:
 *   +---+---+---+---+
 *   | x | u |   |...|
 *   +---+---+---+---+
 *   | 1 | z |   |...|
 *   +---+---+---+---+
 *   | y | v |   |...|
 *   +---+---+---+---+
 *
 *   If n is not a neighbour of 1 then either x != 2 and {u, z} = {x-1, x+1}
 *   or y != 2 and {z, v} = {y-1, y+1}.  Empty z and its symmetry partner,
 *   then either x or y will be a forcing clue, making the board solvable.
 *
 *   So assume n is a neighbour of 1, WLOG x = n.  If z = n-1 then empty z and
 *   its partner; n will be a forcing clue.
 *
 *   Otherwise u = n-1.  If y != 2 then {z, v} = {y-1, y+1}.  Empty z and its
 *   partners; y will be a forcing clue.
 *
 *   So assume y = 2.  If z = 3 then empty z and partner; 2 will be forcing.

 *   So v = 3, and we have this:
 *   +---+---+---+---+
 *   | n |n-1|   |...|
 *   +---+---+---+---+
 *   | 1 | z |   |...|
 *   +---+---+---+---+
 *   | 2 | 3 |   |...|
 *   +---+---+---+---+
 *
 *   Empty {3, n-1} and their two symmetric partners.  Then 2 and n will be
 *   forcing clues, leaving us with two empty squares to the right:
 *   +---+---+---+---+
 *   |...|   | d |   |
 *   +---+---+---+---+
 *   |...| a | b | c |
 *   +---+---+---+---+
 *   |...|   | e | k |
 *   +---+---+---+---+
 *
 *   Here d and e are empty, with 1 < d < d+1 < e < n, and just like before if
 *   {a, b, c} != {d-1, d+1, e+1} the latter contains a forcing clue.
 *
 *   So assume {a, b, c} = {d-1, d+1, e+1}.  Note that 1 < k < n, and thus we
 *   must find {k-1, k+1} in {b, c, e}. But d+1 != k+1 != e+1 so k+1 = d-1.
 *   Then k-1 = d-3 (note: 3 < d since 1..3 are in the leftmost two columns)
 *   but then k-1 < d-1 < d+1 < e < e+1 so k-1 cannot be in {b, c, e}.
 *
 *   Thus {a, b, c} != {d-1, d+1, e+1} for any hamiltonian path, i.e. we must
 *   have hit one of the earlier cases and created a solvable board.
 *
 *   This concludes h == 3.
 *
 * So now we know neighbours = 8 and symmetry == fourway and 4 <= h <= w.
 *
 * If `clue_1_and_n` is false we can empty the corners; then all empty squares
 * are isolated and the board is solvable.  So assume `clue_1_and_n` is true.
 *
 * If (w, h) == (4, 4):
 *   ONLY SOME HAMILTONIAN PATHS CAN BECOME PUZZLES.
 *
 *   We can empty the middle four squares if this is the solution:
 *   (`4x4e1c1s4:S7_6_2_1_8_5_4_3_9_11_13_15_10_12_14_16`)
 *   +----+----+----+----+
 *   |  7 |  6 |  2 |  1 |
 *   +----+----+----+----+
 *   |  8 |  5 |  4 |  3 |
 *   +----+----+----+----+
 *   |  9 | 11 | 13 | 15 |
 *   +----+----+----+----+
 *   | 10 | 12 | 14 | 16 |
 *   +----+----+----+----+
 *   Then 10, 12, 3 and 4 are fourcing clues (in that order).
 *
 *   But there is no symmetry class we can empty for:
 *   (`4x4e1c1s4:16_14_11_10_15_12_13_9_1_3_6_8_2_5_4_7`)
 *   +----+----+----+----+
 *   | 16 | 14 | 11 | 10 |
 *   +----+----+----+----+
 *   | 15 | 12 | 13 |  9 |
 *   +----+----+----+----+
 *   |  1 |  3 |  6 |  8 |
 *   +----+----+----+----+
 *   |  2 |  5 |  4 |  7 |
 *   +----+----+----+----+
 *   Not the corners because 16(=n) and not {1,8,9,15} because 1;
 *   not {4,5,11,14} because neither 3 nor 6 would force 4 or 5
 *   and not {3,6,12,13} because although 2 and 7 would force 3 and 6, neither
 *   11 nor 14 would force 12 or 13.
 *
 *   (Notice the similarity to the 4x2 case with two-fold symmetry.)
 *
 * If (w, h) == (6, 4): # parameters are `6x4e1c1s4`
 *   ONLY SOME HAMILTONIAN PATHS CAN BECOME PUZZLES.
 *
 *   We can empty {2, 6, 22, 23} if this is the solution:
 *   (`15_14_12_11_9_8_16_22_13_10_6_7_17_23_21_20_2_5_24_18_19_3_4_1`)
 *   +----+----+----+----+----+----+
 *   | 15 | 14 | 12 | 11 |  9 |  8 |
 *   +----+----+----+----+----+----+
 *   | 16 | 22 | 13 | 10 |  6 |  7 |
 *   +----+----+----+----+----+----+
 *   | 17 | 23 | 21 | 20 |  2 |  5 |
 *   +----+----+----+----+----+----+
 *   | 24 | 18 | 19 |  3 |  4 |  1 |
 *   +----+----+----+----+----+----+
 *   Then 1, 5, 24, 21 are forcing clues (in that order).
 *
 *   But there is no symmetry class we can empty for:
 *   (`14_15_18_17_24_23_11_13_16_19_21_22_12_10_5_6_20_2_9_8_7_4_3_1`)
 *   +----+----+----+----+----+----+
 *   | 14 | 15 | 18 | 17 | 24 | 23 |
 *   +----+----+----+----+----+----+
 *   | 11 | 13 | 16 | 19 | 21 | 22 |
 *   +----+----+----+----+----+----+
 *   | 12 | 10 |  5 |  6 | 20 |  2 |
 *   +----+----+----+----+----+----+
 *   |  9 |  8 |  7 |  4 |  3 |  1 |
 *   +----+----+----+----+----+----+
 *   - Not the corners because 1 is there
 *   - Not {3, 8, 15, 24} because n=24
 *   - Not {2, 11, 12, 22} because {10, 13} do not force {11, 12}
 *   - Not {10, 13, 20, 21} because {19, 22} do not force {20, 22}
 *   - Not {4, 7, 17, 18} because {16, 19} do not force {17, 18}
 *   - Not {5, 6, 16, 19} because {4, 7} do not force {5, 6}.
 *
 * So assume (h, w) is not in {(4, 4), (4, 6)}
 *
 * Consider the following symmetry classes:
 *   - The corners (i.e. row 1 column 1 and its three symmetric partners)
 *   - If h is odd: the left- and rightmost squares in the middle row
 *   - If w is odd: the top and bottom squares in the middle column
 *   - If 5 <= w: row 1 column 2 and its three partners
 *   - If 7 <= w: row 1 column 3 and its three partners
 *
 * If h is odd then 5 <= h <= w so three symmetry classes are available.
 * If w is odd then 5 <= w and three symmetry classes are available.
 * If w is even then 5 < 7 < w and three symmetry classes are available.
 *
 * Either way, three symmetry classes are available and least one contains
 * neither 1 nor n.  Empty that one.
 *
 * The corners are isolated, as are the outermost squares in the middle row
 * and column (if w and/or h are odd).
 *
 * If 5 < w then row 1 column 2 and its partners are isolated.  If w = 5 then
 * the two empty squares (x, y with 1 < x < x+1 < y < n) have two common
 * neighbours, but {x-1, x+1, y+1} are three relevant clues, at least one of
 * which is forcing.  Likewise for the two empty squares on the bottom row.
 * After filling two empty squares the remaining empty squares are isolated.
 *
 * Similarly if 7 < w then then row 1 column 3 and its partners are isolated,
 * and if w = 7 the situation is like w = 5 vs. row 1 column 2.
 *
 * This exhausts all the cases and completes the proof.
 */
