#include "snprint.h"

static void append_char(char *buf, size_t size, size_t *pos, char ch)
{
    if (size > 0) {
        if (*pos + 1 < size) {
            buf[*pos] = ch;
        }
    }
    (*pos)++;
}

static void append_string(char *buf, size_t size, size_t *pos, const char *s)
{
    while (s != NULL && *s != '\0') {
        append_char(buf, size, pos, *s++);
    }
}

static void append_unsigned(char *buf, size_t size, size_t *pos, unsigned int value, int base)
{
    char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[33];
    int i = 0;

    if (base < 2 || base > 36) {
        return;
    }

    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value != 0) {
            tmp[i++] = digits[value % base];
            value /= base;
        }
    }

    while (i-- > 0) {
        append_char(buf, size, pos, tmp[i]);
    }
}

static void append_signed(char *buf, size_t size, size_t *pos, int value, int base)
{
    unsigned int uvalue;

    if (base == 10 && value < 0) {
        append_char(buf, size, pos, '-');
        uvalue = (unsigned int)(-value);
    } else {
        uvalue = (unsigned int)value;
    }

    append_unsigned(buf, size, pos, uvalue, base);
}

int vsnprintf(char *str, size_t size, const char *format, va_list args)
{
    size_t pos = 0;
    const char *fmt = format;

    if (str != NULL && size > 0) {
        str[0] = '\0';
    }

    while (*fmt != '\0') {
        if (*fmt != '%') {
            append_char(str, size, &pos, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '\0') {
            break;
        }

        if (*fmt == '%') {
            append_char(str, size, &pos, '%');
            fmt++;
            continue;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            append_string(str, size, &pos, s);
            break;
        }
        case 'd':
        case 'i': {
            int value = va_arg(args, int);
            append_signed(str, size, &pos, value, 10);
            break;
        }
        case 'u': {
            unsigned int value = va_arg(args, unsigned int);
            append_unsigned(str, size, &pos, value, 10);
            break;
        }
        case 'x': {
            unsigned int value = va_arg(args, unsigned int);
            append_unsigned(str, size, &pos, value, 16);
            break;
        }
        case 'X': {
            unsigned int value = va_arg(args, unsigned int);
            char digits[] = "0123456789ABCDEF";
            char tmp[33];
            int i = 0;
            unsigned int v = value;

            if (v == 0) {
                tmp[i++] = '0';
            } else {
                while (v != 0) {
                    tmp[i++] = digits[v % 16];
                    v /= 16;
                }
            }

            while (i-- > 0) {
                append_char(str, size, &pos, tmp[i]);
            }
            break;
        }
        case 'c': {
            int ch = va_arg(args, int);
            append_char(str, size, &pos, (char)ch);
            break;
        }
        default:
            append_char(str, size, &pos, *fmt);
            break;
        }

        fmt++;
    }

    if (str != NULL && size > 0) {
        if (pos >= size) {
            pos = size - 1;
        }
        str[pos] = '\0';
    }

    return (int)pos;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

char *itoa(int value, char *str, int base)
{
    char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[33];
    unsigned int uvalue;
    int sign = 0;
    int i = 0;
    int j = 0;

    if (str == NULL || base < 2 || base > 36) {
        return NULL;
    }

    if (base == 10 && value < 0) {
        sign = 1;
        uvalue = (unsigned int)(-value);
    } else {
        uvalue = (unsigned int)value;
    }

    if (uvalue == 0) {
        tmp[i++] = '0';
    } else {
        while (uvalue != 0) {
            tmp[i++] = digits[uvalue % base];
            uvalue /= base;
        }
    }

    if (sign) {
        tmp[i++] = '-';
    }

    while (i-- > 0) {
        str[j++] = tmp[i];
    }
    str[j] = '\0';

    return str;
}
