/*
 * Droidspaces v6 - PulseAudio Server and Socket Manager
 *
 * Manages the PulseAudio daemon lifecycle on Android (spawning, logging, and
 * stopping) and host-to-container socket bridging.
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "droidspace.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

/*
 * Resolve the Termux UID from packages.list.
 * Returns the UID on success, -1 on failure.
 * PulseAudio needs to run as the Termux app UID so Android's audio HAL
 * grants it access to OpenSL ES / AAudio.
 */
static int pa_resolve_termux_uid(void) {
  int uid = ds_resolve_termux_uid();
  if (uid < 0)
    return -1;

  if (access(TX11_PULSE_BIN, F_OK) != 0) {
    ds_warn("PulseAudio: binary not found at %s. Is pulseaudio installed in "
            "Termux?",
            TX11_PULSE_BIN);
    return -1;
  }
  return uid;
}

/* ---- daemon child ----------------------------------------------------- */

struct pulse_args {
  int uid;
};

/*
 * Set up the forked child, perform the SELinux dance, and exec PulseAudio.
 * ready_fd: O_CLOEXEC write-end; EOF on execv success, byte on failure.
 * This function never returns on success.
 */
static void pulse_child_wrapper(int ready_fd, void *user_data) {
  struct pulse_args *args = (struct pulse_args *)user_data;

  /* Ignore hangups, keyboard interrupts, and broken pipes to keep the daemon
   * alive through terminal disconnects. SIGTERM is our shutdown signal. */
  signal(SIGHUP, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);

  /* Make PulseAudio unkillable by the OOM killer.
   * Must be done while still root, before privilege drop. */
  ds_oom_protect();

  /* Set up the Termux environment before dropping privileges */
  setenv("TMPDIR", TX11_PREFIX "/tmp", 1);
  setenv("HOME", TX11_HOME, 1);
  setenv("PREFIX", TX11_PREFIX, 1);
  setenv("PULSE_SERVER", "unix:" TX11_PULSE_SOCKET, 1);

  /* Ensure the tmp directory exists (root creates it before priv drop) */
  mkdir_p(TX11_PREFIX "/tmp", 0755);

  /* Stay in droidspacesd (permissive) -- no domain transition needed.
   * untrusted_app_27 blocks execv of Termux binaries under enforcing. */
  ds_selinux_enter_domain();

  /* Drop root -> Termux UID. */
  if (ds_drop_privileges(args->uid) < 0) {
    perror("[PulseAudio] privilege drop failed");
    if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
    }
    _exit(1);
  }

  fprintf(stdout, "[PulseAudio] uid=%d socket=%s\n", (int)getuid(),
          TX11_PULSE_SOCKET);
  fflush(stdout);

  /* Launch PulseAudio in non-daemon foreground mode.
   * - module-native-protocol-unix: UNIX socket with anonymous auth
   * - module-aaudio-sink: Android AAudio low-latency audio output (default)
   * - --exit-idle-time=-1: never exit on idle (we manage lifecycle ourselves)
   * - --daemonize=no: stay in foreground so our log relay captures all output
   */
  char *argv[] = {
      TX11_PULSE_BIN,
      "--load=module-native-protocol-unix socket=" TX11_PULSE_SOCKET
      " auth-anonymous=1",
      "--load=module-aaudio-sink",
      "--exit-idle-time=-1",
      "--daemonize=no",
      "--log-target=stderr",
      NULL,
  };

  execv(argv[0], argv);
  perror("[PulseAudio] execv");
  if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
  }
  _exit(1);
}

/* ---- pactl post-start helper ------------------------------------------ */

/*
 * Run 'pactl set-default-sink AAudio_sink' once the socket is up.
 * Forked as a short-lived child; parent doesn't wait -- fire and forget.
 */
static void run_pactl_set_default(int uid) {
  pid_t p = fork();
  if (p < 0)
    return;
  if (p == 0) {
    /* Redirect to /dev/null -- we don't need pactl output in the log */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    /* Stay in droidspacesd (permissive) */
    ds_selinux_enter_domain();
    if (ds_drop_privileges(uid) < 0) {
      _exit(1);
    }

    setenv("PULSE_SERVER", "unix:" TX11_PULSE_SOCKET, 1);
    setenv("HOME", TX11_HOME, 1);

    char *argv[] = {TX11_PACTL_BIN, "set-default-sink", TX11_PULSE_DEFAULT_SINK,
                    NULL};
    execv(TX11_PACTL_BIN, argv);
    _exit(1);
  }
  /* Parent: reap to avoid zombie. We don't care about exit code. */
  waitpid(p, NULL, 0);
}

/* ---- spawn ------------------------------------------------------------ */

static pid_t spawn_pulse(int uid) {
  struct pulse_args args = {.uid = uid};
  return ds_spawn_daemon(pulse_child_wrapper, &args, "pulse.log", "PulseAudio",
                         "PulseAudio");
}

/* ---- public API ------------------------------------------------------- */

int ds_pulse_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->pulseaudio || !is_android())
    return -1;
  if (getuid() != 0) {
    ds_error("[PulseAudio] not running as root");
    return -1;
  }

  /* Reuse existing global daemon if still alive */
  pid_t existing = ds_daemon_read_pid("pulse.ppid");
  if (existing > 0) {
    ds_log("PulseAudio: daemon already running (PID %d)", (int)existing);
    cfg->pulse_pid = existing;
    return 1;
  }

  int uid = pa_resolve_termux_uid();
  if (uid < 0)
    return -1;

  /* Clean up stale socket from a previous crashed run */
  unlink(TX11_PULSE_SOCKET);

  ds_log("[PulseAudio] launching daemon (uid=%d)", uid);
  pid_t child = spawn_pulse(uid);
  if (child <= 0)
    return -1;

  cfg->pulse_pid = child;
  ds_daemon_write_pid("pulse.ppid", child);

  /* Wait for the UNIX socket to appear, then set the default sink.
   * If PA hasn't created the socket in 3s (or dies), pactl is skipped. */
  if (wait_for_socket_or_death(child, TX11_PULSE_SOCKET, 3000, 100000) == 0) {
    ds_log("[PulseAudio] socket appeared - setting default sink to %s",
           TX11_PULSE_DEFAULT_SINK);
    run_pactl_set_default(uid);
  } else {
    ds_warn("PulseAudio: socket did not appear in 3s - skipping pactl");
  }

  return 0;
}

void ds_pulse_daemon_stop(struct ds_config *cfg) {
  if (!cfg || !is_android())
    return;

  /* Keep the daemon alive if any other running container still needs it */
  if (check_pulse_needs() == 1) {
    ds_log("[PulseAudio] keeping global daemon running for other active "
           "containers");
    return;
  }

  pid_t pid =
      cfg->pulse_pid > 0 ? cfg->pulse_pid : ds_daemon_read_pid("pulse.ppid");
  if (pid > 0) {
    ds_log("[PulseAudio] terminating daemon (PID %d)...", (int)pid);
    kill(pid, SIGTERM);
    for (int i = 0; i < 10 && kill(pid, 0) == 0; i++)
      usleep(100000);
    if (kill(pid, 0) == 0) {
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
    }
    cfg->pulse_pid = 0;
  }

  ds_daemon_remove_pid("pulse.ppid");
  unlink(TX11_PULSE_SOCKET);
}

/* ---- socket bridge ---------------------------------------------------- */

int ds_setup_pulse_socket(struct ds_config *cfg) {
  if (!is_android() || !cfg->pulseaudio)
    return 0;

  /* Post-pivot_root: host filesystem is under /.old_root.
   * TX11_PULSE_SOCKET lives in TX11_PREFIX/tmp, accessed via
   * DS_TERMUX_TMP_OLDROOT, exactly like the VirGL socket. */
  char src[PATH_MAX];
  snprintf(src, sizeof(src), "%s/.pulse-socket", DS_TERMUX_TMP_OLDROOT);

  struct stat st;
  if (stat(src, &st) != 0) {
    ds_warn("PulseAudio: socket not found at %s - skipping socket bridge", src);
    return 0;
  }

  uid_t uid = st.st_uid;

  if (ds_bind_mount_socket(src, DS_PULSE_SOCKET, uid, "PulseAudio") < 0)
    return 0;

  ds_log("PulseAudio: socket bind-mounted into container");

  /* Inject PULSE_SERVER so all processes inside the container find the daemon
   */
  setenv("PULSE_SERVER", "unix:" DS_PULSE_SOCKET, 1);

  return 0;
}
