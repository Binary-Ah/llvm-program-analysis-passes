#include <stdio.h>

// 1. 상수 전파 및 Lattice 분석용 함수
void constant_propagation_test() {
    int x = 10;
    int y = x + 5; // x가 상수로 전파되는지 확인
    printf("Value: %d\n", y);
}

// 2. 보안 체크 및 Alias 분석용 함수
void security_check(int *secret, int *public_val) {
    *secret = 999;
    *public_val = 1; 
    // 분석 포인트: secret과 public_val이 NoAlias인지 확인
}

int main() {
    int s, p;
    constant_propagation_test();
    security_check(&s, &p);
    return 0;
}