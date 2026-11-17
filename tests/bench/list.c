#include <stdio.h>
#include <stdlib.h>
struct Node { int value; struct Node *next; };
int sum(struct Node *head) {
    int s = 0;
    for (struct Node *cur = head; cur != NULL; cur = cur->next) s = (s + cur->value) % 1000003;
    return s;
}
void free_all(struct Node *cur) {
    while (cur != NULL) { struct Node *n = cur; cur = n->next; free(n); }
}
int main(void) {
    int total = 0;
    for (int round = 0; round < 6; round++) {
        struct Node *head = NULL;
        for (int i = 0; i < 500000; i++) {
            struct Node *n = malloc(sizeof *n);
            if (!n) abort();
            n->value = i;
            n->next = head;
            head = n;
        }
        for (int k = 0; k < 10; k++) total = (total + sum(head)) % 1000003;
        free_all(head);
    }
    printf("%d\n", total);
    return 0;
}
