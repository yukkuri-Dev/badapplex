int toupper(int c)
{
	if (c >= 'a' && c <= 'z')
		c = c - 0x20;
	return c;
}
