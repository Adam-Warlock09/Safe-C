/* Demo 9: the C side. Plain C, compiled and linked together with
   09_calling_c.safec by safec. */
struct Point { int x; int y; };

struct Point midpoint(struct Point a, struct Point b) {
    struct Point m = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
    return m;
}

int requests_served = 41;

static int fake_register;          /* stands in for a memory-mapped device */
int *device_register(void) { return &fake_register; }
