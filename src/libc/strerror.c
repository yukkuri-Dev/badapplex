#include <string.h>

// errno体系を持たない環境向けの簡易スタブ。エラーコードの意味付けは行わない。
char *strerror(int errnum)
{
	(void)errnum;
	return "unknown error";
}
