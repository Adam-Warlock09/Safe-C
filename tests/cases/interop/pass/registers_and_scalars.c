// Calls the functions of registers_and_scalars.safec.
// Returns the number of the first check that fails, 0 if all pass.
#include <stdbool.h>

struct Pair { int a, b; };
struct Quad { int a, b, c, d; };
struct Vec2 { float x, y; };
struct Big  { int a, b, c, d, e; };

int quad_after_five(int a, int b, int c, int d, int e, struct Quad q, int n);
int pair_after_six(int a, int b, int c, int d, int e, int f, struct Pair p);
float vec_after_eight(float a, float b, float c, float d, float e, float f, float g, float h,
                      struct Vec2 v, int i);
struct Big big_after_five(int a, int b, int c, int d, int e, struct Quad q);
char char_minus_one(char c);
bool not(bool b);
int char_plus_bool(char c, bool b);

int main(void) {
    if (quad_after_five(1, 2, 3, 4, 5, (struct Quad){ 1, 2, 3, 4 }, 5) != 543225) return 1;
    if (pair_after_six(1, 1, 1, 1, 1, 1, (struct Pair){ 2, 3 }) != 3206) return 2;
    if (vec_after_eight(1, 1, 1, 1, 1, 1, 1, 1, (struct Vec2){ 2, 3 }, 4) != 4328.0f) return 3;

    struct Big b = big_after_five(1, 2, 3, 4, 5, (struct Quad){ 10, 20, 30, 40 });
    if (b.a != 11 || b.b != 22 || b.c != 33 || b.d != 44 || b.e != 5) return 4;

    if (char_minus_one(-100) != -101) return 5;
    if (char_minus_one(0) != -1) return 6;
    if (not(true) != false || not(false) != true) return 7;
    if (char_plus_bool(-5, true) != -4) return 8;
    return 0;
}
