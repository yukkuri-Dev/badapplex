
#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include "../logger.h"

typedef struct _file FILE;
#define SEEK_SET 0
#define SEEK_END 2
#define SEEK_CUR 1
#define EOF (-1)
#define BUFSIZ 512

// この環境には標準入出力ストリームの概念が無いため常にNULL
#define stdin  ((FILE *)0)
#define stdout ((FILE *)0)
#define stderr ((FILE *)0)

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *fp);
unsigned long fread(void *ptr, unsigned long size, unsigned long nmemb, FILE *stream);
unsigned long fwrite(const void *ptr, unsigned long size, unsigned long nmemb, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
int fileno(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int getc(FILE *stream);

#define fflush(x)
#define printf log_write
int sprintf(char *str, const char *format, ...);
int vsprintf(char *str, const char *format, va_list args);

#endif // _STDIO_H
