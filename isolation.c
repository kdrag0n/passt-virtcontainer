// SPDX-License-Identifier: AGPL-3.0-or-later

/* PASST - Plug A Simple Socket Transport
 *  for qemu/UNIX domain socket mode
 *
 * PASTA - Pack A Subtle Tap Abstraction
 *  for network namespace/tap device mode
 *
 * isolation.c - Self isolation helpers
 *
 * Copyright Red Hat
 * Author: Stefano Brivio <sbrivio@redhat.com>
 * Author: David Gibson <david@gibson.dropbear.id.au>
 */
/**
 * DOC: Theory of Operation
 *
 * For security the passt/pasta process performs a number of
 * self-isolations steps, dropping capabilities, setting namespaces
 * and otherwise minimising the impact we can have on the system at
 * large if we were compromised.
 *
 * Obviously we can't isolate ourselves from resources before we've
 * done anything we need to do with those resources, so we have
 * multiple stages of self-isolation.  In order these are:
 *
 * 1. isolate_initial()
 * ====================
 *
 * Executed immediately after startup, drops capabilities we don't
 * need at any point during execution (or which we gain back when we
 * need by joining other namespaces).
 *
 * 2. isolate_user()
 * =================
 *
 * Executed once we know what user and user namespace we want to
 * operate in.  Sets our final UID & GID, and enters the correct user
 * namespace.
 *
 * 3. isolate_prefork()
 * ====================
 *
 * Executed after all setup, but before daemonising (fork()ing into
 * the background).  Uses mount namespace and pivot_root() to remove
 * our access to the filesystem.
 *
 * 4. isolate_postfork()
 * =====================
 *
 * Executed immediately after daemonizing, but before entering the
 * actual packet forwarding phase of operation.  Or, if not
 * daemonizing, immediately after isolate_prefork().  Uses seccomp()
 * to restrict ourselves to the handful of syscalls we need during
 * runtime operation.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "util.h"
#include "seccomp.h"
#include "passt.h"
#include "log.h"
#include "isolation.h"

#define CAP_VERSION	_LINUX_CAPABILITY_VERSION_3
#define CAP_WORDS	_LINUX_CAPABILITY_U32S_3

/**
 * drop_caps_ep_except() - Drop capabilities from effective & permitted sets
 * @keep:	Capabilities to keep
 */
static void drop_caps_ep_except(uint64_t keep)
{
	struct __user_cap_header_struct hdr = {
		.version = CAP_VERSION,
		.pid = 0,
	};
	struct __user_cap_data_struct data[CAP_WORDS];
	int i;

	if (syscall(SYS_capget, &hdr, data)) {
		err("Couldn't get current capabilities: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < CAP_WORDS; i++) {
		uint32_t mask = keep >> (32 * i);

		data[i].effective &= mask;
		data[i].permitted &= mask;
	}

	if (syscall(SYS_capset, &hdr, data)) {
		err("Couldn't drop capabilities: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
}

/**
 * clamp_caps() - Prevent any children from gaining caps
 *
 * This drops all capabilities from both the inheritable and the
 * bounding set.  This means that any exec()ed processes can't gain
 * capabilities, even if they have file capabilities which would grant
 * them.  We shouldn't ever exec() in any case, but this provides an
 * additional layer of protection.  Executing this requires
 * CAP_SETPCAP, which we will have within our userns.
 *
 * Note that dropping capabilites from the bounding set limits
 * exec()ed processes, but does not remove them from the effective or
 * permitted sets, so it doesn't reduce our own capabilities.
 */
static void clamp_caps(void)
{
	struct __user_cap_data_struct data[CAP_WORDS];
	struct __user_cap_header_struct hdr = {
		.version = CAP_VERSION,
		.pid = 0,
	};
	int i;

	for (i = 0; i < 64; i++) {
		/* Some errors can be ignored:
		 * - EINVAL, we'll get this for all values in 0..63
		 *   that are not actually allocated caps
		 * - EPERM, we'll get this if we don't have
		 *   CAP_SETPCAP, which can happen if using
		 *   --netns-only.  We don't need CAP_SETPCAP for
		 *   normal operation, so carry on without it.
		 */
		if (prctl(PR_CAPBSET_DROP, i, 0, 0, 0) &&
		    errno != EINVAL && errno != EPERM) {
			err("Couldn't drop cap %i from bounding set: %s",
			    i, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	if (syscall(SYS_capget, &hdr, data)) {
		err("Couldn't get current capabilities: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < CAP_WORDS; i++)
		data[i].inheritable = 0;

	if (syscall(SYS_capset, &hdr, data)) {
		err("Couldn't drop inheritable capabilities: %s",
		    strerror(errno));
		exit(EXIT_FAILURE);
	}
}

/**
 * isolate_initial() - Early, config independent self isolation
 *
 * Should:
 *  - drop unneeded capabilities
 * Musn't:
 *  - remove filesytem access (we need to access files during setup)
 */
void isolate_initial(void)
{
	/* We want to keep CAP_NET_BIND_SERVICE in the initial
	 * namespace if we have it, so that we can forward low ports
	 * into the guest/namespace
	 *
	 * We have to keep CAP_SETUID and CAP_SETGID at this stage, so
	 * that we can switch user away from root.
	 *
	 * We have to keep some capabilities for the --netns-only case:
	 *  - CAP_SYS_ADMIN, so that we can setns() to the netns.
	 *  - Keep CAP_NET_ADMIN, so that we can configure interfaces
	 *
	 * It's debatable whether it's useful to drop caps when we
	 * retain SETUID and SYS_ADMIN, but we might as well.  We drop
	 * further capabilites in isolate_user() and
	 * isolate_prefork().
	 */
	drop_caps_ep_except(BIT(CAP_NET_BIND_SERVICE) |
			    BIT(CAP_SETUID) | BIT(CAP_SETGID) |
			    BIT(CAP_SYS_ADMIN) | BIT(CAP_NET_ADMIN));
}

/**
 * isolate_user() - Switch to final UID/GID and move into userns
 * @uid:	User ID to run as (in original userns)
 * @gid:	Group ID to run as (in original userns)
 * @use_userns:	Whether to join or create a userns
 * @userns:	userns path to enter, may be empty
 * @mode:	Mode (passt or pasta)
 *
 * Should:
 *  - set our final UID and GID
 *  - enter our final user namespace
 * Mustn't:
 *  - remove filesystem access (we need that for further setup)
 */
void isolate_user(uid_t uid, gid_t gid, bool use_userns, const char *userns,
		  enum passt_modes mode)
{
	uint64_t ns_caps = 0;

	/* First set our UID & GID in the original namespace */
	if (setgroups(0, NULL)) {
		/* If we don't have CAP_SETGID, this will EPERM */
		if (errno != EPERM) {
			err("Can't drop supplementary groups: %s",
			    strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	if (setgid(gid) != 0) {
		err("Can't set GID to %u: %s", gid, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (setuid(uid) != 0) {
		err("Can't set UID to %u: %s", uid, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (*userns) { /* If given a userns, join it */
		int ufd;

		ufd = open(userns, O_RDONLY | O_CLOEXEC);
		if (ufd < 0) {
			err("Couldn't open user namespace %s: %s",
			    userns, strerror(errno));
			exit(EXIT_FAILURE);
		}

		if (setns(ufd, CLONE_NEWUSER) != 0) {
			err("Couldn't enter user namespace %s: %s",
			    userns, strerror(errno));
			exit(EXIT_FAILURE);
		}

		close(ufd);

	} else if (use_userns) { /* Create and join a new userns */
		if (unshare(CLONE_NEWUSER) != 0) {
			err("Couldn't create user namespace: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	/* Joining a new userns gives us full capabilities; drop the
	 * ones we don't need.  With --netns-only we haven't changed
	 * userns but we can drop more capabilities now than at
	 * isolate_initial()
	 */
	/* Keep CAP_SYS_ADMIN, so we can unshare() further in
	 * isolate_prefork(), pasta also needs it to setns() into the
	 * netns
	 */
	ns_caps |= BIT(CAP_SYS_ADMIN);
	if (mode == MODE_PASTA) {
		/* Keep CAP_NET_ADMIN, so we can configure the if */
		ns_caps |= BIT(CAP_NET_ADMIN);
		/* Keep CAP_NET_BIND_SERVICE, so we can splice
		 * outbound connections to low port numbers
		 */
		ns_caps |= BIT(CAP_NET_BIND_SERVICE);
		/* Keep CAP_SYS_PTRACE to join the netns of an
		 * existing process */
		if (*userns || !use_userns)
			ns_caps |= BIT(CAP_SYS_PTRACE);
	}

	drop_caps_ep_except(ns_caps);
}

/**
 * isolate_prefork() - Self isolation before daemonizing
 * @c:		Execution context
 *
 * Return: negative error code on failure, zero on success
 *
 * Should:
 *  - Move us to our own IPC and UTS namespaces
 *  - Move us to a mount namespace with only an empty directory
 *  - Drop unneeded capabilities (in the new user namespace)
 * Mustn't:
 *  - Remove syscalls we need to daemonise
 */
int isolate_prefork(struct ctx *c)
{
	int flags = CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWUTS;
	uint64_t ns_caps = 0;

	/* If we run in foreground, we have no chance to actually move to a new
	 * PID namespace. For passt, use CLONE_NEWPID anyway, in case somebody
	 * ever gets around seccomp profiles -- there's no harm in passing it.
	 */
	if (!c->foreground || c->mode == MODE_PASST)
		flags |= CLONE_NEWPID;

	if (unshare(flags)) {
		perror("unshare");
		return -errno;
	}

	if (mount("", "/", "", MS_UNBINDABLE | MS_REC, NULL)) {
		perror("mount /");
		return -errno;
	}

	if (mount("", TMPDIR, "tmpfs",
		  MS_NODEV | MS_NOEXEC | MS_NOSUID | MS_RDONLY,
		  "nr_inodes=2,nr_blocks=0")) {
		perror("mount tmpfs");
		return -errno;
	}

	if (chdir(TMPDIR)) {
		perror("chdir");
		return -errno;
	}

	if (syscall(SYS_pivot_root, ".", ".")) {
		perror("pivot_root");
		return -errno;
	}

	if (umount2(".", MNT_DETACH | UMOUNT_NOFOLLOW)) {
		perror("umount2");
		return -errno;
	}

	/* Now that initialization is more-or-less complete, we can
	 * drop further capabilities
	 */
	if (c->mode == MODE_PASTA) {
		/* Keep CAP_SYS_ADMIN, so we can enter the netns */
		ns_caps |= BIT(CAP_SYS_ADMIN);
		/* Keep CAP_NET_BIND_SERVICE, so we can splice
		 * outbound connections to low port numbers
		 */
		ns_caps |= BIT(CAP_NET_BIND_SERVICE);
	}

	clamp_caps();
	drop_caps_ep_except(ns_caps);

	return 0;
}

/**
 * isolate_postfork() - Self isolation after daemonizing
 * @c:		Execution context
 *
 * Should:
 *  - disable core dumps
 *  - limit to a minimal set of syscalls
 */
void isolate_postfork(const struct ctx *c)
{
	struct sock_fprog prog;

	prctl(PR_SET_DUMPABLE, 0);

	if (c->mode == MODE_PASST) {
		prog.len = (unsigned short)ARRAY_SIZE(filter_passt);
		prog.filter = filter_passt;
	} else {
		prog.len = (unsigned short)ARRAY_SIZE(filter_pasta);
		prog.filter = filter_pasta;
	}

}
