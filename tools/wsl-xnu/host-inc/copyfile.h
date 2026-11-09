/* fcopyfile(COPYFILE_DATA) only, for SETUP/installfile on Linux. */
#include "darwin-compat.h"
#define COPYFILE_DATA (1 << 3)
typedef void *copyfile_state_t;
static inline int fcopyfile(int from, int to, copyfile_state_t s, unsigned flags) {
	char buf[65536]; ssize_t n; (void)s; (void)flags;
	if (lseek(from, 0, SEEK_SET) < 0) return -1;
	while ((n = read(from, buf, sizeof buf)) > 0) { char *p = buf; while (n > 0) { ssize_t w = write(to, p, n); if (w < 0) return -1; p += w; n -= w; } }
	return n < 0 ? -1 : 0;
}
/* BSD setmode/getmode, octal modes only (xnu passes -m 0444 / 0755 style). */
static inline void *setmode(const char *s) {
	char *e; unsigned long m = strtoul(s, &e, 8); mode_t *p;
	if (!*s || *e || m > 07777) return NULL;
	p = malloc(sizeof *p); if (p) *p = (mode_t)m; return p;
}
static inline mode_t getmode(const void *set, mode_t omode) { (void)omode; return *(const mode_t *)set; }
