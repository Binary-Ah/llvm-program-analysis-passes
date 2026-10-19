#include <stdio.h>

// 전역 변수 (Global Value)
int global_var = 10;

int compute(int a, int b) {
    int result = 0;
    
    // 조건문 (Branch instruction 유도)
    if (a > b) {
        result = a * b + global_var; // mul, add 연산
    } else {
        result = (a + b) * 2;
    }
    
    return result;
}

int main() {
    int x = 5;
    int y = 10;
    int sum = 0;

    // 반복문 (Phi node, Branch 유도)
    for (int i = 0; i < 5; i++) {
        sum += compute(x, i);
    }

    printf("Total Sum: %d\n", sum);

    return 0;
}