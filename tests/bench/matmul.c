#include <stdio.h>
struct Mat { float m[64][64]; };
struct Mat a, b, c;
void mul(struct Mat *out, const struct Mat *x, const struct Mat *y) {
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 64; j++) {
            out->m[i][j] = 0.0f;
            for (int k = 0; k < 64; k++)
                out->m[i][j] = out->m[i][j] + x->m[i][k] * y->m[k][j];
        }
}
int main(void) {
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 64; j++) {
            a.m[i][j] = (i + j) % 7 * 0.125f;
            b.m[i][j] = (i * j) % 5 * 0.25f;
        }
    for (int r = 0; r < 1500; r++) mul(&c, &a, &b);
    float check = 0.0f;
    for (int i = 0; i < 64; i++) check = check + c.m[i][i];
    printf("%.3f\n", check);
    return 0;
}
