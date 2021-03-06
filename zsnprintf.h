/* 
 * File:   zsnprintf.h
 * Author: Michael Sandstedt
 *
 * An snprintf that is fast, capable, and safe.
 *
 * Created on January 1, 2017, 11:38 AM
 */

#ifndef ZSNPRINTF_H
#define	ZSNPRINTF_H

#include <stdarg.h>

size_t zvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
#ifdef __GNUC__
size_t zsnprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format (printf, 3, 4)));
#else
size_t zsnprintf(char* buf, size_t n, const char* fmt, ...);
#endif

#endif	/* ZSNPRINTF_H */

