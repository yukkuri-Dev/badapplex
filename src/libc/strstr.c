#include <string.h>

char *strstr(const char *haystack, const char *needle)
{
	if (*needle == '\0')
		return (char *)haystack;

	for (; *haystack != '\0'; haystack++) {
		const char *h = haystack;
		const char *n = needle;
		while (*h != '\0' && *n != '\0' && *h == *n) {
			h++;
			n++;
		}
		if (*n == '\0')
			return (char *)haystack;
	}
	return NULL;
}
