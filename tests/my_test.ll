; Sample IR: exercises add/sub/mul/sdiv/load/store/phi
define i32 @sample(i32 %a, i32 %b) {
entry:
  %x = alloca i32
  store i32 5, i32* %x          ; store positive constant
  %loaded = load i32, i32* %x   ; should be Positive
  %sum    = add  i32 %loaded, 3 ; Positive + Positive = Positive
  %diff   = sub  i32 %sum, %b   ; Positive - Top = Top
  %prod   = mul  i32 -2, -3     ; Negative * Negative = Positive
  %quot   = sdiv i32 %prod, 2   ; Positive / Positive = Positive
  ret i32 %quot
}
