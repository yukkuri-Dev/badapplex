#ifndef _MATH_H
#define _MATH_H

// 最小構成のmath.h。
// floor/ceil/fmod/fabs/modf/frexp/ldexpはIEEE754倍精度のビット操作で
// 実装しているため標準ライブラリ非依存で正確に動く。
// 三角関数・対数・pow・sqrtはこの環境では未実装で、呼ぶと0を返す
// スタブとしている(Luaのmath.sin等を使うスクリプトは正しい結果を得られない)。

#define HUGE_VAL (__builtin_huge_val())

double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double fabs(double x);
double modf(double x, double *iptr);
double frexp(double x, int *exp);
double ldexp(double x, int exp);

// 未実装のスタブ(常に0を返す)
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sqrt(double x);

#endif /* _MATH_H */
