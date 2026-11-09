/* Darwin libc bits the XNU SETUP host tools use, for building them on Linux. */
#ifndef DARWIN_COMPAT_H
#define DARWIN_COMPAT_H
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <mach/mach_init.h>
#define __private_extern__ __attribute__((visibility("hidden")))
#define getprogname() (program_invocation_short_name)
#ifndef O_EXLOCK
#define O_EXLOCK 0
#endif
#ifndef O_SHLOCK
#define O_SHLOCK 0
#endif
#define st_mtimespec st_mtim
#define st_atimespec st_atim
static inline size_t compat_strlcpy(char *d, const char *s, size_t n) {
	size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
static inline size_t compat_strlcat(char *d, const char *s, size_t n) {
	size_t dl = strnlen(d, n); if (dl == n) return n + strlen(s); return dl + compat_strlcpy(d + dl, s, n - dl); }
#if !defined(__GLIBC__) || __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38)
#define strlcpy compat_strlcpy
#define strlcat compat_strlcat
#endif
/* Darwin's lseek on /dev/null returns the new offset; Linux returns 0, which
 * kextsymboltool (-output /dev/null in config/Makefile check_all_exports) reads as
 * a failed seek. */
static inline off_t compat_lseek(int fd, off_t off, int whence) {
	off_t r = lseek(fd, off, whence);
	struct stat st;
	if (r == 0 && whence == SEEK_SET && off != 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode))
		return off;
	return r;
}
#define lseek compat_lseek
#endif
