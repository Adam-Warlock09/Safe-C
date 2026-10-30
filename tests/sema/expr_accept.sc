struct Node { int val; };
int main() {
    int x = 10;
    int y = x + 5;
    bool z = x == y;
    own int* p = new int;
    free(p);
    return 0;
}