void alias_func(int *a, int *b) {
    *a = 10;
    *b = 20;
    int val1 = *a; // 첫 번째 load
    int val2 = *b; // 두 번째 load
    int res = val1 + val2;
}