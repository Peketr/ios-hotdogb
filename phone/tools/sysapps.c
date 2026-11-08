/*
 * sysapps: install iOS's removable factory apps (Weather, Notes, Calendar, Mail, Maps, ...) without installd.
 *
 * The IPSW ships them in /private/var/staged_system_apps, with a placeholder per app in
 * /System/Library/AppPlaceholders; on a real phone installd moves them in on first boot. Here installd cannot
 * (no clonefile on HFS+, no code signature validation), so, as on flame (HANDOFF "Stock apps on the home
 * screen"): each staged app is renamed into /Applications (same volume: no copy), its placeholder is moved to
 * /var/local/sysapps-placeholders, then `lsreg -r` rebuilds LaunchServices' databases.
 *
 *   sysapps            install whatever is still staged (nothing staged: does nothing, exit 0)
 *   sysapps -n         dry run: say what would move
 *   sysapps -u         undo: move the apps back to staged_system_apps and the placeholders back, then lsreg -r
 *
 * Runs as a one-shot launchd job (tools/hotdogb-sysapps-install.py). Log on stdout.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

#define STAGED "/private/var/staged_system_apps"
#define APPS "/Applications"
#define PH "/System/Library/AppPlaceholders"
#define PHBAK "/var/local/sysapps-placeholders"
#define LSREG "/var/local/bin/lsreg"

extern char **environ;

static int exists(const char *p)
{
	struct stat st;
	return lstat(p, &st) == 0;
}

static int mv(const char *from, const char *to, int dry)
{
	if (!exists(from)) {
		return 0;
	}
	if (exists(to)) {
		printf("  skip %s: %s exists\n", from, to);
		return 0;
	}
	if (dry) {
		printf("  would move %s -> %s\n", from, to);
		return 1;
	}
	if (rename(from, to) != 0) {
		printf("  move %s -> %s FAILED: %s\n", from, to, strerror(errno));
		return -1;
	}
	printf("  moved %s -> %s\n", from, to);
	return 1;
}

static int lsreg_rebuild(void)
{
	char *argv[] = {LSREG, "-r", NULL};
	pid_t pid;
	int st, e;
	printf("running %s -r\n", LSREG);
	fflush(stdout);
	if ((e = posix_spawn(&pid, LSREG, NULL, NULL, argv, environ)) != 0) {
		printf("spawn %s: %s\n", LSREG, strerror(e));
		return 1;
	}
	if (waitpid(pid, &st, 0) < 0) {
		printf("waitpid: %s\n", strerror(errno));
		return 1;
	}
	printf("lsreg -r exit %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(int argc, char **argv)
{
	int dry = argc > 1 && !strcmp(argv[1], "-n");
	int undo = argc > 1 && !strcmp(argv[1], "-u");
	const char *src = undo ? APPS : STAGED;
	char a[1024], b[1024];
	int moved = 0, failed = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("#### SYSAPPS %s (pid %d)\n", undo ? "UNDO" : dry ? "DRY RUN" : "INSTALL", getpid());
	if (!undo && !dry) {
		mkdir(PHBAK, 0755);
	}
	/* undo: the apps to move back are the ones whose placeholder sits in PHBAK */
	DIR *d = opendir(undo ? PHBAK : STAGED);
	if (!d) {
		printf("%s: %s - nothing to do\n", undo ? PHBAK : STAGED, strerror(errno));
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".app")) {
			continue;
		}
		int r;
		snprintf(a, sizeof a, "%s/%s", src, e->d_name);
		snprintf(b, sizeof b, "%s/%s", undo ? STAGED : APPS, e->d_name);
		r = mv(a, b, dry);
		if (r < 0) {
			failed++;
			continue;
		}
		moved += r;
		snprintf(a, sizeof a, "%s/%s", undo ? PHBAK : PH, e->d_name);
		snprintf(b, sizeof b, "%s/%s", undo ? PH : PHBAK, e->d_name);
		if (mv(a, b, dry) < 0) {
			failed++;
		}
	}
	closedir(d);
	printf("%d app(s) %s, %d failure(s)\n", moved, dry ? "to move" : "moved", failed);
	if (dry || moved == 0) {
		return failed != 0;
	}
	sync();
	return lsreg_rebuild() || failed != 0;
}
