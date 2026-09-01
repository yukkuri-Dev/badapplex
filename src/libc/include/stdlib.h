
#ifndef _STDLIB_H
#define _STDLIB_H

#include "../memmgr.h"

#define NULL 0


#define malloc memmgr_alloc
#define realloc memmgr_realloc
#define free memmgr_free

int rand(void);
void srand(unsigned int seed);

int atoi(const char *s);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
int abs(int j);

void _Exit(int status);
int atexit(void (*function)(void));
void exit(int status);
void abort(void);

#endif // _STDLIB_H
