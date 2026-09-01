#ifndef _ERRNO_H
#define _ERRNO_H

// errnoコード体系を持たない環境向けの簡易スタブ。
// fopen等の失敗はNULL/負値の戻り値で判定するため、errnoの値自体は
// 常に0(エラー無し)として扱う。
extern int errno;

#define EDOM   1
#define ERANGE 2

#endif /* _ERRNO_H */
