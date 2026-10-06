// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * kshim_printf.c - a small freestanding vsnprintf with the Linux printk
 * conventions fs/ntfs relies on (%pV, %pd, ll/z/t modifiers), so formatting
 * does not depend on the NT run-time library's dialect.
 */
#include <kshim.h>

struct pbuf { char *p; size_t n, len; };

static void put(struct pbuf *b, char c)
{
	if (b->len + 1 < b->n)
		b->p[b->len] = c;
	b->len++;
}

static void puts_pad(struct pbuf *b, const char *s, int slen, int width, int left)
{
	int pad = width > slen ? width - slen : 0;
	if (!left) while (pad-- > 0) put(b, ' ');
	while (slen-- > 0) put(b, *s++);
	if (left) while (pad-- > 0) put(b, ' ');
}

static void put_num(struct pbuf *b, unsigned long long v, int neg, int base, int upper,
		int width, int prec, int zero, int left, int plus, int space, int alt)
{
	char tmp[24];
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0, len, pad;
	char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
	const char *pfx = (alt && base == 16 && v) ? (upper ? "0X" : "0x") : (alt && base == 8) ? "0" : "";
	int pfxlen = (int)strlen(pfx);

	do { tmp[n++] = dig[v % base]; v /= base; } while (v);
	if (prec >= 0 && prec == 0 && n == 1 && tmp[0] == '0')
		n = 0;
	len = n > prec ? n : prec;
	len += (sign ? 1 : 0) + pfxlen;
	pad = width > len ? width - len : 0;
	if (!left && !zero) while (pad-- > 0) put(b, ' ');
	if (sign) put(b, sign);
	while (*pfx) put(b, *pfx++);
	if (!left && zero) while (pad-- > 0) put(b, '0');
	for (int i = n; i < prec; i++) put(b, '0');
	while (n) put(b, tmp[--n]);
	if (left) while (pad-- > 0) put(b, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct pbuf b = { buf, size, 0 };

	for (; *fmt; fmt++) {
		int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0;
		if (*fmt != '%') { put(&b, *fmt); continue; }
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') alt = 1;
			else break;
		}
		if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
		else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++; prec = 0;
			if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
			else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
		}
		for (;; fmt++) {
			if (*fmt == 'l') lng++;
			else if (*fmt == 'h') ;
			else if (*fmt == 'z' || *fmt == 't') lng = (sizeof(size_t) == 8) ? 2 : 1;
			else if (*fmt == 'j' || *fmt == 'L' || *fmt == 'q') lng = 2;
			else break;
		}
		switch (*fmt) {
		case 'd': case 'i': {
			long long v = lng >= 2 ? va_arg(ap, long long) : lng ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
			put_num(&b, v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 10, 0, width, prec, zero, left, plus, space, 0);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': {
			unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned int);
			int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
			put_num(&b, v, 0, base, *fmt == 'X', width, prec, zero, left, 0, 0, alt);
			break;
		}
		case 'c': { char c = (char)va_arg(ap, int); puts_pad(&b, &c, 1, width, left); break; }
		case 's': {
			const char *s = va_arg(ap, const char *);
			int l = 0;
			if (!s) s = "(null)";
			while (s[l] && (prec < 0 || l < prec)) l++;
			puts_pad(&b, s, l, width, left);
			break;
		}
		case 'p': {
			void *p = va_arg(ap, void *);
			if (fmt[1] == 'V') {
				struct va_format *vaf = p;
				va_list cp;
				char *rest = buf ? buf + (b.len < b.n ? b.len : b.n) : NULL;
				size_t room = b.len < b.n ? b.n - b.len : 0;
				fmt++;
				va_copy(cp, *vaf->va);
				b.len += vsnprintf(rest, room, vaf->fmt, cp);
				va_end(cp);
				break;
			}
			if (fmt[1] == 'd') {
				const struct dentry *d = p;
				fmt++;
				if (d) puts_pad(&b, (const char *)d->d_name.name, d->d_name.len, width, left);
				break;
			}
			put_num(&b, (uintptr_t)p, 0, 16, 0, width, prec, zero, left, 0, 0, 1);
			break;
		}
		case '%': put(&b, '%'); break;
		case 0: fmt--; break;
		default: put(&b, '%'); put(&b, *fmt); break;
		}
	}
	if (size)
		buf[b.len < size ? b.len : size - 1] = 0;
	return (int)b.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap; int r;
	va_start(ap, fmt); r = vsnprintf(buf, n, fmt, ap); va_end(ap);
	return r;
}

int scnprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap; int r;
	va_start(ap, fmt); r = vsnprintf(buf, n, fmt, ap); va_end(ap);
	if (!n) return 0;
	return r >= (int)n ? (int)n - 1 : r;
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap; int r;
	va_start(ap, fmt); r = vsnprintf(buf, 4096, fmt, ap); va_end(ap);
	return r;
}
