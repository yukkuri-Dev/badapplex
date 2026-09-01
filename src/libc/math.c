#include <math.h>

// IEEE754倍精度のビットパターンを直接操作して床/天井/剰余/絶対値を求める。
// FPUを使わない(-m4-nofpu)ビルドのため、コンパイラの浮動小数点組み込み関数
// (__builtin_floor等)はソフトエミュレーションのdouble演算にコンパイルされる。
// ここではビット演算のみで完結させ、余計なソフト浮動小数点ライブラリへの
// 依存を増やさないようにしている。

typedef union {
	double d;
	unsigned long long u;
} d_bits;

#define SIGN_MASK 0x8000000000000000ULL
#define EXP_MASK  0x7ff0000000000000ULL
#define FRAC_MASK 0x000fffffffffffffULL
#define EXP_BIAS  1023

double fabs(double x)
{
	d_bits b;
	b.d = x;
	b.u &= ~SIGN_MASK;
	return b.d;
}

double floor(double x)
{
	d_bits b;
	b.d = x;
	int exp = (int)((b.u & EXP_MASK) >> 52) - EXP_BIAS;

	if (exp >= 52 || x != x /* NaN */)
		return x;
	if (exp < 0) {
		// |x| < 1
		if (b.u & SIGN_MASK)
			return (x == 0.0) ? x : -1.0;
		return 0.0;
	}

	unsigned long long frac_bits_to_clear = 52 - exp;
	unsigned long long mask = (frac_bits_to_clear >= 64) ? ~0ULL : ((1ULL << frac_bits_to_clear) - 1);
	int had_frac = (b.u & mask) != 0;
	b.u &= ~mask;

	if (had_frac && (b.u & SIGN_MASK))
		b.d -= 1.0;
	return b.d;
}

double ceil(double x)
{
	return -floor(-x);
}

double modf(double x, double *iptr)
{
	double i = (x < 0.0) ? ceil(x) : floor(x);
	*iptr = i;
	return x - i;
}

double fmod(double x, double y)
{
	if (y == 0.0)
		return 0.0;
	double q = x / y;
	double iq = (q < 0.0) ? ceil(q) : floor(q);
	return x - iq * y;
}

double frexp(double x, int *exp)
{
	d_bits b;
	b.d = x;

	if (x == 0.0 || x != x) {
		*exp = 0;
		return x;
	}

	int e = (int)((b.u & EXP_MASK) >> 52) - EXP_BIAS;
	if (e == -EXP_BIAS) {
		// 非正規化数は未対応(0扱い)
		*exp = 0;
		return 0.0;
	}

	*exp = e + 1;
	// 指数部を、仮数を[0.5,1.0)に収める値へ書き換える
	b.u = (b.u & ~EXP_MASK) | ((unsigned long long)(EXP_BIAS - 1) << 52);
	return b.d;
}

double ldexp(double x, int exp)
{
	d_bits b;
	b.d = x;

	if (x == 0.0 || x != x)
		return x;

	int e = (int)((b.u & EXP_MASK) >> 52) - EXP_BIAS;
	e += exp;
	if (e >= 1024) {
		b.u = (b.u & SIGN_MASK) | EXP_MASK;
		return b.d; // overflow -> inf
	}
	if (e <= -1023) {
		b.u &= SIGN_MASK;
		return b.d; // underflow -> 0 (非正規化数は未対応)
	}

	b.u = (b.u & ~EXP_MASK) | ((unsigned long long)(e + EXP_BIAS) << 52);
	return b.d;
}

// 未実装スタブ: この環境では三角関数・対数・pow・sqrtの精度実装を用意していない
double sin(double x) { (void)x; return 0.0; }
double cos(double x) { (void)x; return 0.0; }
double tan(double x) { (void)x; return 0.0; }
double asin(double x) { (void)x; return 0.0; }
double acos(double x) { (void)x; return 0.0; }
double atan(double x) { (void)x; return 0.0; }
double atan2(double y, double x) { (void)y; (void)x; return 0.0; }
double exp(double x) { (void)x; return 0.0; }
double log(double x) { (void)x; return 0.0; }
double log2(double x) { (void)x; return 0.0; }
double log10(double x) { (void)x; return 0.0; }
double pow(double x, double y) { (void)x; (void)y; return 0.0; }
double sqrt(double x) { (void)x; return 0.0; }
