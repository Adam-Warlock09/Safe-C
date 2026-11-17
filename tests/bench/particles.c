#include <stdio.h>
struct Vec2 { float x, y; };
struct Particle { struct Vec2 pos, vel; };
struct Particle ps[4096];
struct Vec2 add(struct Vec2 a, struct Vec2 b) { return (struct Vec2){ a.x + b.x, a.y + b.y }; }
struct Vec2 scale(struct Vec2 a, float k) { return (struct Vec2){ a.x * k, a.y * k }; }
void step(struct Particle *p, struct Vec2 gravity, float dt) {
    p->vel = add(p->vel, scale(gravity, dt));
    p->pos = add(p->pos, scale(p->vel, dt));
    if (p->pos.y < 0.0f) { p->pos.y = -p->pos.y; p->vel.y = -p->vel.y * 0.5f; }
}
int main(void) {
    for (int i = 0; i < 4096; i++) {
        ps[i].pos = (struct Vec2){ i * 0.25f, 100.0f };
        ps[i].vel = (struct Vec2){ 1.0f, 0.0f };
    }
    struct Vec2 gravity = { 0.0f, -9.8f };
    for (int t = 0; t < 50000; t++)
        for (int i = 0; i < 4096; i++) step(&ps[i], gravity, 0.001f);
    float check = 0.0f;
    for (int i = 0; i < 4096; i++) check = check + ps[i].pos.x + ps[i].pos.y;
    printf("%.1f\n", check);
    return 0;
}
