// The C side of safec_calls_c.safec: functions and globals its main calls.
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct IntCharBool { int x; char c; bool b; };
struct ThreeInts   { int a, b, c; };
struct TwoFloats   { float x, y; };
struct ThreeFloats { float a, b, c; };
struct IntFloats   { int a; float b, c; };
struct CharArray5  { char s[5]; };
struct Big         { int a, b, c, d, e; };
struct Quad        { int a, b, c, d; };

struct IntCharBool c_bump_icb(struct IntCharBool v) { v.x++; v.c++; v.b = !v.b; return v; }
struct ThreeInts   c_bump_i3(struct ThreeInts v)    { v.a++; v.b++; v.c++; return v; }
struct TwoFloats   c_bump_f2(struct TwoFloats v)    { v.x += 1; v.y += 1; return v; }
struct ThreeFloats c_bump_f3(struct ThreeFloats v)  { v.a += 1; v.b += 1; v.c += 1; return v; }
struct IntFloats   c_bump_iff(struct IntFloats v)   { v.a++; v.b += 1; v.c += 1; return v; }
struct CharArray5  c_bump_ca5(struct CharArray5 v)  { for (int i = 0; i < 5; i++) v.s[i]++; return v; }
struct Big         c_bump_big(struct Big v)         { v.a++; v.b++; v.c++; v.d++; v.e++; return v; }

int c_quad_after_five(int a, int b, int c, int d, int e, struct Quad q, int n) {
    return a + b + c + d + e + q.a * 10 + q.b * 100 + q.c * 1000 + q.d * 10000 + n * 100000;
}
float c_vec_after_eight(float a, float b, float c, float d, float e, float f, float g, float h,
                        struct TwoFloats v, int i) {
    return a + b + c + d + e + f + g + h + v.x * 10 + v.y * 100 + i * 1000;
}

char c_next_char(char c) { return c + 1; }
bool c_not(bool b) { return !b; }
int c_add(int a, int b) { return a + b; }

int c_counter = 41;
float c_scale = 2.5f;
int c_get_counter(void) { return c_counter; }

int *c_make_counter(int start) {
    int *p = malloc(sizeof *p);
    *p = start;
    return p;
}
int c_destroyed;
void c_destroy(int *p) {
    c_destroyed = *p;
    free(p);
}

// Called as c_check_varargs(5, (char)-5, true, 2.5f, 7, "ok"): char and bool
// arrive as int, float as double. Returns 0, or which argument was wrong
int c_check_varargs(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int result = 0;
    if (count != 5) result = 1;
    else if (va_arg(ap, int) != -5) result = 1;
    else if (va_arg(ap, int) != 1) result = 2;
    else if (va_arg(ap, double) != 2.5) result = 3;
    else if (va_arg(ap, int) != 7) result = 4;
    else if (strcmp(va_arg(ap, char *), "ok") != 0) result = 5;
    va_end(ap);
    return result;
}
