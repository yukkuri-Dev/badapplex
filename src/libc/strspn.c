#include <string.h>

unsigned long strspn(const char *s, const char *accept)
{
	unsigned long count = 0;
	while (*s != '\0' && strchr(accept, *s) != 0) {
		s++;
		count++;
	}
	return count;
}
