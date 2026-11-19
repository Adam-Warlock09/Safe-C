/* Demo 12: the C side. C owns the table; SafeC gets raw pointers into it. */
#include <stdio.h>

struct Point { int x; int y; };
static struct Point table[3] = { { 5, 500 }, { 250, 7 }, { 99, 101 } };

struct Point *c_entity(int id) { return &table[id]; }
int c_entity_count(void) { return 3; }
void c_print_table(void) {
    for (int i = 0; i < 3; i++) printf("  point %d: (%d, %d)\n", i, table[i].x, table[i].y);
}
void c_fill_squares(int *out, int n) { for (int i = 0; i < n; i++) out[i] = i * i; }
