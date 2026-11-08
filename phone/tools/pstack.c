/*
 * pstack PID: every thread's pc/lr and frame-pointer backtrace, plus the
 * shared cache slide, for symbolication on the Mac (tools/flame-pstack.sh
 * does both). Needs task_for_pid (boot policy allows it; signed with
 * task_for_pid-allow).
 * pstack -w NAME [COUNT [SECS]]: for launchd jobs (no shell on hotdogb): wait
 * for each new process named exactly NAME, then sample it COUNT times (default
 * 6), SECS apart (default 5), the first one SECS after it appeared. Runs until
 * killed; each sample starts with "=== NAME PID sample I".
 */
#include <mach/mach.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

static int rd(task_t t, uint64_t a, void *out, size_t n)
{
	vm_size_t got = 0;
	return vm_read_overwrite(t, (vm_address_t)a, n, (vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n ? 0 : -1;
}

static int dump(int pid)
{
	task_t t;
	kern_return_t kr = task_for_pid(mach_task_self(), pid, &t);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	struct task_dyld_info di;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	if (task_info(t, TASK_DYLD_INFO, (task_info_t)&di, &cnt) == KERN_SUCCESS) {
		struct dyld_all_image_infos ai;
		if (rd(t, di.all_image_info_addr, &ai, sizeof(ai)) == 0)
			printf("shared cache slide 0x%lx\n", (unsigned long)ai.sharedCacheSlide);
	}
	thread_act_array_t th;
	mach_msg_type_number_t nth;
	if (task_threads(t, &th, &nth) != KERN_SUCCESS)
		return 1;
	for (unsigned i = 0; i < nth; i++) {
		arm_thread_state64_t s;
		mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&s, &sc) != KERN_SUCCESS)
			continue;
		uint64_t pc = arm_thread_state64_get_pc(s), lr = arm_thread_state64_get_lr(s), fp = arm_thread_state64_get_fp(s);
		printf("thread %u: pc 0x%llx lr 0x%llx\n  bt", i, pc, lr);
		for (int d = 0; d < 32 && fp; d++) {
			uint64_t fr[2];
			if (rd(t, fp, fr, sizeof(fr)))
				break;
			if (!fr[1])
				break;
			printf(" 0x%llx", fr[1] & 0x0000000fffffffffULL);
			fp = fr[0];
		}
		printf("\n");
	}
	for (unsigned i = 0; i < nth; i++)
		mach_port_deallocate(mach_task_self(), th[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(th[0]));
	mach_port_deallocate(mach_task_self(), t);
	return 0;
}

/* pid of the newest process named exactly name, or 0 */
static int find(const char *name)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL }, best = 0;
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0)
		return 0;
	len += len / 4;
	struct kinfo_proc *p = malloc(len);
	if (p && sysctl(mib, 3, p, &len, NULL, 0) == 0)
		for (size_t i = 0; i < len / sizeof(*p); i++)
			if (!strcmp(p[i].kp_proc.p_comm, name) && p[i].kp_proc.p_pid > best)
				best = p[i].kp_proc.p_pid;
	free(p);
	return best;
}

int main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "-w")) {
		const char *name = argv[2];
		int count = argc > 3 ? atoi(argv[3]) : 6, secs = argc > 4 ? atoi(argv[4]) : 5, done = 0;
		setvbuf(stdout, NULL, _IOLBF, 0);
		printf("pstack: waiting for %s\n", name);
		for (;;) {
			int pid = find(name);
			if (pid && pid != done) {
				done = pid;
				for (int i = 0; i < count && find(name) == pid; i++) {
					sleep(secs);
					printf("=== %s %d sample %d\n", name, pid, i);
					dump(pid);
					fflush(stdout);
				}
			}
			usleep(200000);
		}
	}
	if (argc < 2) {
		fprintf(stderr, "usage: pstack PID | pstack -w NAME [COUNT [SECS]]\n");
		return 2;
	}
	return dump(atoi(argv[1]));
}
