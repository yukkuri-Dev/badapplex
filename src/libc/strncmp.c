#include <string.h>

int strncmp(const char *s1, const char *s2, unsigned long n)
{
	while (n > 0) {
		unsigned char c1 = *(const unsigned char *)s1;
		unsigned char c2 = *(const unsigned char *)s2;
		if (c1 != c2)
			return (int)c1 - (int)c2;
		if (c1 == '\0')
			return 0;
		s1++;
		s2++;
		n--;
	}
	return 0;
}
