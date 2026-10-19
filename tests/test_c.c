#include <stdio.h>

void analyze_me(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) {
            sum += i;
        } else {
            sum *= 2;
        }
    }
    printf("Result: %d\n", sum);
}

int main() {
    analyze_me(10);
    return 0;
}