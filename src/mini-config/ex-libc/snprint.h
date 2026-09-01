#ifndef SNPRINT_H
#define SNPRINT_H

#include <stdarg.h>
#include <stddef.h>

int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list args);
char *itoa(int value, char *str, int base);

#endif
