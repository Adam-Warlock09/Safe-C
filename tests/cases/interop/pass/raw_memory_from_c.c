/* The C side of raw_memory_from_c.safec. Returns the number of the first
   check that fails, 0 if all pass. */
#include <stddef.h>

struct Point { int x; int y; };
static struct Point table[3] = { { 5, 500 }, { 250, 7 }, { 99, 101 } };
static char *names[3] = { "banana", "cherry", NULL };

struct Point *c_entity(int id) { return &table[id]; }
int c_entity_count(void) { return 3; }
char *c_find_name(int id) { return names[id]; }
void c_fill(int *out, int n) { for (int i = 0; i < n; i++) out[i] = i * 10; }

int clamp_all(int max);
int names_with(int id, char wanted);
int sum_filled(void);

int main(void) {
    if (clamp_all(100) != 3) return 1;
    if (table[0].x != 5 || table[0].y != 100) return 2;    /* SafeC wrote C's memory */
    if (table[1].x != 100 || table[1].y != 7) return 3;
    if (table[2].x != 99 || table[2].y != 100) return 4;
    if (names_with(0, 'a') != 3) return 5;                 /* "banana" */
    if (names_with(1, 'r') != 2) return 6;                 /* "cherry" */
    if (names_with(2, 'x') != -1) return 7;                /* NULL from C */
    if (sum_filled() != 100) return 8;                     /* 0+10+20+30+40 */
    return 0;
}
