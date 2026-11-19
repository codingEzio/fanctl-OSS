/* SPDX-License-Identifier: GPL-2.0-only
 * Short-lived, unprivileged worker for the optional passwordless installation.
 * All terminals of one account share one worker and one latest requested target.
 */
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <syslog.h>

extern char **environ;

static int job_directory(bool create) {
    char path[PATH_MAX];
    size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path));
    if (!size || size > sizeof(path) ||
        strlcat(path, "/fanctl-control", sizeof(path)) >= sizeof(path))
        fail("Cannot locate the per-user control directory");
    if (create && mkdir(path, 0700) < 0 && errno != EEXIST) fail("Cannot create control directory");
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0 && !create && errno == ENOENT) return -1;
    struct stat st;
    if (dir < 0 || fstat(dir, &st) || st.st_uid != getuid() || (st.st_mode & 0077))
        fail("Cannot use the private control directory");
    return dir;
}

static int job_lock(int dir, const char *name, bool try_only) {
    int fd = openat(dir, name, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) fail("Cannot open the control lock");
    int rc;
    do {
        rc = flock(fd, LOCK_EX | (try_only ? LOCK_NB : 0));
    } while (rc && errno == EINTR);
    if (!rc) return fd;
    if (!try_only || errno != EWOULDBLOCK) fail("Cannot lock fan control");
    close(fd);
    return -1;
}

static bool job_read(int dir, const char *name, char *text, size_t length) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) return false;
    if (fd < 0) fail("Cannot open fan request status");
    ssize_t n;
    do {
        n = read(fd, text, length - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0) fail("Cannot read fan request status");
    text[n] = '\0';
    return true;
}

static void job_store(int dir, const char *command) {
    int fd = openat(dir, "next.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, command, strlen(command)) != (ssize_t)strlen(command) || close(fd) ||
        renameat(dir, "next.tmp", dir, "pending"))
        fail("Cannot save the requested fan setting");
}

static bool previous_job_failed(int dir) {
    char message[1024];
    if (!job_read(dir, "error", message, sizeof(message))) return false;
    fprintf(stderr, "fanctl: last adjustment failed; %s\n", message);
    return true;
}

static int job_result(void) {
    int dir = job_directory(false);
    if (dir < 0) return 0;
    int gate = job_lock(dir, "gate", false);
    char message[1024];
    int result = 0;
    if (job_read(dir, "pending", message, sizeof(message))) {
        int lock = job_lock(dir, "worker", true);
        if (lock >= 0) {
            close(lock);
            fputs("fanctl: previous adjustment was interrupted; repeat the command\n", stderr);
            result = 1;
        }
    } else if (previous_job_failed(dir)) {
        result = 1;
    }
    close(gate);
    close(dir);
    return result;
}

static int apply_job(int dir, const char *command) {
    int log =
        openat(dir, "applying.log", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log < 0) fail("Cannot open the fan adjustment log");
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) ||
        posix_spawn_file_actions_adddup2(&actions, log, 2))
        fail("Cannot prepare the fan adjustment");
    char *args[] = {"/usr/bin/sudo", "-n", "--", FANCTL_SYSTEM_TOOL, (char *)command, NULL};
    pid_t child;
    int rc = posix_spawn(&child, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    if (!rc) {
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) rc = EIO;
    }
    if (rc) dprintf(log, "Could not apply %s. Repeat the command or use fanctl auto.", command);
    close(log);
    return rc;
}

static int work_jobs(int dir, int lock) {
    char current[32], next[32];
    int gate = job_lock(dir, "gate", false);
    while (job_read(dir, "pending", current, sizeof(current))) {
        close(gate);
        int rc = apply_job(dir, current);
        gate = job_lock(dir, "gate", false);
        if (rc) syslog(LOG_ERR, "fanctl could not apply %s", current);
        if (!job_read(dir, "pending", next, sizeof(next)) || strcmp(current, next)) continue;
        if (rc) {
            if (renameat(dir, "applying.log", dir, "error")) fail("Cannot record control failure");
        } else {
            unlinkat(dir, "error", 0);
            unlinkat(dir, "applying.log", 0);
        }
        if (unlinkat(dir, "pending", 0)) fail("Cannot complete the fan request");
        break;
    }
    close(lock); /* Release while the gate is held, so a new request always gets a worker. */
    close(gate);
    close(dir);
    return 0;
}

static void start_worker(int dir, int lock) {
    char path[PATH_MAX];
    uint32_t length = sizeof(path);
    if (_NSGetExecutablePath(path, &length)) fail("Cannot locate the fan worker");
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    /* Duplicate above the reserved descriptors before mapping them, even if stdin was closed. */
    int d = fcntl(dir, F_DUPFD_CLOEXEC, 5), l = fcntl(lock, F_DUPFD_CLOEXEC, 5);
    if (d < 0 || l < 0 || posix_spawn_file_actions_init(&actions) || posix_spawnattr_init(&attr) ||
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT) ||
        posix_spawn_file_actions_adddup2(&actions, d, 3) ||
        posix_spawn_file_actions_adddup2(&actions, l, 4) ||
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDWR, 0) ||
        posix_spawn_file_actions_adddup2(&actions, 0, 1) ||
        posix_spawn_file_actions_adddup2(&actions, 0, 2))
        fail("Cannot prepare the background fan worker");
    pid_t pid;
    char *args[] = {path, "--apply-pending", NULL};
    int rc = posix_spawn(&pid, path, &actions, &attr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(d);
    close(l);
    if (rc) fail("Cannot start the background fan worker");
}

/* The inspection and submission share the gate so another terminal cannot lose a newer target. */
static void submit_job(io_connect_t connection, const char *command, bool maximum, bool automatic,
                       int rpm) {
    int dir = job_directory(true);
    int gate = job_lock(dir, "gate", false);
    int lock = job_lock(dir, "worker", true);
    FanState fans[10];
    int count = all_fans(connection, fans);
    if (!automatic && !maximum) {
        for (int i = 0; i < count; i++) {
            if (rpm < fans[i].minimum || rpm > fans[i].maximum)
                fail("RPM is outside a fan's allowed range");
        }
    }
    if (lock >= 0) (void)previous_job_failed(dir);
    if (lock < 0 || automatic || !already_applied(fans, count, maximum, rpm)) {
        char number[16];
        if (!maximum && !automatic) {
            snprintf(number, sizeof(number), "%d", rpm);
            command = number;
        }
        job_store(dir, command);
        if (lock >= 0) start_worker(dir, lock);
    } else {
        unlinkat(dir, "pending", 0);
        unlinkat(dir, "error", 0);
    }
    if (lock >= 0) close(lock);
    close(gate);
    close(dir);
    print_rpm(fans, count);
}
