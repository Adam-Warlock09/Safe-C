/* Demo 10: the C side, an ordinary C program using a SafeC library. */
#include <stdio.h>

struct Stats { int count, total, min, max; };
struct Histogram { int bins[5]; };

/* implemented in SafeC (10_c_calls_safec.safec) */
struct Stats stats_add(struct Stats s, int value);
int stats_mean(struct Stats s);
void histogram_add(struct Histogram *h, int value);

int main(void) {
    int samples[] = { 12, 47, 33, 95, 61, 8, 74, 50 };
    struct Stats s = { 0, 0, 0, 0 };
    struct Histogram h = { { 0 } };
    for (int i = 0; i < 8; i++) {
        s = stats_add(s, samples[i]);
        histogram_add(&h, samples[i]);
    }
    printf("C calling SafeC:\n");
    printf("  count %d, min %d, max %d, mean %d\n", s.count, s.min, s.max, stats_mean(s));
    printf("  histogram:");
    for (int b = 0; b < 5; b++) printf(" [%d-%d]: %d", b * 20, b * 20 + 19, h.bins[b]);
    printf("\n");
    return 0;
}
