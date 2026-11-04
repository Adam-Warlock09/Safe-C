struct Node { int v; own struct Node*? next; };

// nested check
int nested(struct Node*? p) {
    if (p != null) {
        if (p != null) return p->v;         
    }
    return 0;
}

// check after an early-return guard
int after_guard(struct Node*? p) {
    if (p == null) return 0;
    if (p != null) return p->v;             
    return 1;
}

// null on the left, and the '==' form
int reversed(struct Node*? p) {
    if (p == null) return 0;
    if (null == p) return 1;               
    return p->v;
}

// loop condition after a guard
int loop_cond(struct Node*? p) {
    if (p == null) return 0;
    int s = 0;
    while (p != null && s < 10) {           
        s = s + p->v;
    }
    return s;
}

int main() {
    return 0;
}
