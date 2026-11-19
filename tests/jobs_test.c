/* SPDX-License-Identifier: GPL-2.0-only
 * Exercise the real detached-worker locks and latest request on synthetic input.
 * The only child allowed to stand in for sudo is this test executable.
 */
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int job_test_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                          const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    if (!strcmp(path, "/usr/bin/sudo")) {
        if (strcmp(argv[1], "-n") || strcmp(argv[2], "--") ||
            strcmp(argv[3], "/Library/PrivilegedHelperTools/fanctl") || argv[5])
            abort();
        char *args[] = {getenv("JOB_TEST_BINARY"), "fixture", argv[4], NULL};
        return posix_spawn(pid, args[0], actions, attr, args, envp);
    }
    return posix_spawn(pid, path, actions, attr, argv, envp);
}
#define posix_spawn job_test_spawn
#define main fanctl_command_main
#include "../fanctl.c"
#undef main
#undef posix_spawn

static void require(bool ok) {
    if (!ok) exit(3);
}

int main(int argc, char **argv) {
    require(argc >= 2);
    if (!strcmp(argv[1], "--apply-pending")) return work_jobs(3, 4);
    if (!strcmp(argv[1], "fixture")) {
        if (!strcmp(argv[2], "3300")) {
            close(open("started", O_CREAT | O_WRONLY, 0600));
            for (int i = 0; access("proceed", F_OK) && i < 400; i++)
                usleep(10000);
            require(!access("proceed", F_OK));
            return 0;
        }
        fputs("simulated fan control failure\n", stderr);
        return 1;
    }
    require(argc == 2 && !chdir(argv[1]));
    int dir = open(".", O_RDONLY | O_DIRECTORY);
    int gate = job_lock(dir, "gate", false);
    int lock = job_lock(dir, "worker", true);
    require(lock >= 0);
    job_store(dir, "3300");
    start_worker(dir, lock);
    close(lock);
    close(gate);
    for (int i = 0; access("started", F_OK) && i < 400; i++)
        usleep(10000);
    require(!access("started", F_OK));
    gate = job_lock(dir, "gate", false);
    require(job_lock(dir, "worker", true) < 0);
    job_store(dir, "auto");
    job_store(dir, "3310");
    close(gate);
    close(open("proceed", O_CREAT | O_WRONLY, 0600));
    for (int i = 0; !access("pending", F_OK) && i < 400; i++)
        usleep(10000);
    require(access("pending", F_OK) < 0 && errno == ENOENT);
    char message[256];
    require(job_read(dir, "error", message, sizeof(message)) &&
            strstr(message, "simulated fan control failure") && strstr(message, "3310"));
    require(job_lock(dir, "worker", true) >= 0);
    puts("Detached control: latest target, confirmed failure, worker exit verified.");
}
