/*
 * Copyright (C) 2013  Brian Johnson
 * Author: Brian Johnson <brijohn@gmail.com>
 */
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>

static void (*atexit_cb)(void) = 0;

static unsigned int lastrandom = 0x12345678;
	
void srand(unsigned int seed)
{
	lastrandom = seed;
}

int rand(void)
{
	lastrandom = 0x41C64E6D*lastrandom + 0x3039;
	return lastrandom >> 16;
}

void exit(int status) {
	if (atexit_cb)
		atexit_cb();
	_exit(-2);
}

void abort(void)
{
	_exit(-2);
}

int abs(int j)
{
	return (j < 0) ? -j : j;
}

/* Simple atexit function only supports registering one callback
 *  (All thats needed for gnuboy EX)
 */
int atexit(void (*function)(void))
{
	atexit_cb = function;
	return 0;
}

int atoi(const char *s)
{
	int a = 0;
	if (*s == '0')
	{
		s++;
		if (*s == 'x' || *s == 'X')
		{
			s++;
			while (*s)
			{
				if (isdigit(*s))
					a = (a<<4) + *s - '0';
				else if (strchr("ABCDEF", *s))
					a = (a<<4) + *s - 'A' + 10;
				else if (strchr("abcdef", *s))
					a = (a<<4) + *s - 'a' + 10;
				else return a;
				s++;
			}
			return a;
		}
		while (*s)
		{
			if (strchr("01234567", *s))
				a = (a<<3) + *s - '0';
			else return a;
			s++;
		}
		return a;
	}
	if (*s == '-')
	{
		s++;
		for (;;)
		{
			if (isdigit(*s))
				a = (a*10) + *s - '0';
			else return -a;
			s++;
		}
	}
	while (*s)
	{
		if (isdigit(*s))
			a = (a*10) + *s - '0';
		else return a;
		s++;
	}
	return a;
}

/* 簡易実装: 10進数の"[-+]digits[.digits][(e|E)[-+]digits]"のみ対応。
 * 16進浮動小数点(0x1.8p3等)やinf/nanは扱わない(Luaのlua_str2numberは
 * それらを自前でチェックしてから渡すため、通常の数値リテラルには十分)。
 */
double strtod(const char *nptr, char **endptr)
{
	const char *s = nptr;
	int neg = 0;
	double result = 0.0;

	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;

	if (*s == '-') { neg = 1; s++; }
	else if (*s == '+') { s++; }

	const char *digits_start = s;

	while (isdigit(*s)) {
		result = result * 10.0 + (double)(*s - '0');
		s++;
	}

	if (*s == '.') {
		s++;
		double frac = 0.1;
		while (isdigit(*s)) {
			result += (double)(*s - '0') * frac;
			frac *= 0.1;
			s++;
		}
	}

	if (s == digits_start || (s == digits_start + 1 && *digits_start == '.')) {
		if (endptr != 0)
			*endptr = (char *)nptr;
		return 0.0;
	}

	if (*s == 'e' || *s == 'E') {
		const char *exp_start = s;
		s++;
		int exp_neg = 0;
		if (*s == '-') { exp_neg = 1; s++; }
		else if (*s == '+') { s++; }

		if (!isdigit(*s)) {
			s = exp_start; /* 'e'の後に数字が無ければ指数部は無視 */
		} else {
			int exp_val = 0;
			while (isdigit(*s)) {
				exp_val = exp_val * 10 + (*s - '0');
				s++;
			}
			double scale = 1.0;
			for (int i = 0; i < exp_val; i++)
				scale *= 10.0;
			result = exp_neg ? (result / scale) : (result * scale);
		}
	}

	if (endptr != 0)
		*endptr = (char *)s;
	return neg ? -result : result;
}

float strtof(const char *nptr, char **endptr)
{
	return (float)strtod(nptr, endptr);
}
