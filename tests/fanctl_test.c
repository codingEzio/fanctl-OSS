/* Non-privileged CLI authorization contract. No SMC call may run here. */
/* Modified on 2026-11-15: cached authorization and presentation argument transfer. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uid_t fake_geteuid(void) { return 501; }
static int fake_executable(char *buffer, uint32_t *size) {
    const char *path = "/fixture/bin/fanctl";
    if (*size <= strlen(path)) return -1;
    strcpy(buffer, path);
    return 0;
}
static char *fake_realpath(const char *path, char *resolved) {
    strcpy(resolved, path);
    return resolved;
}
#if FANCTL_USE_ADJACENT_AUTHORIZER
static pid_t fake_fork(void) {
    if (getenv("FANCTL_TEST_CACHE_PROBE_ERROR")) { errno = EAGAIN; return -1; }
    return 4242;
}
static pid_t fake_waitpid(pid_t child, int *status, int options) {
    (void)options;
    *status = getenv("FANCTL_TEST_CACHED") ? 0 : 256;
    return child;
}
#endif
static int fake_execv(const char *path, char *const arguments[]) {
    if (getenv("FANCTL_TEST_EXEC_FAIL")) { errno = ENOENT; return -1; }
    bool no_prompt = arguments[1] && strcmp(arguments[1], "-n") == 0;
    bool askpass = arguments[1] && strcmp(arguments[1], "-A") == 0;
    int executable = no_prompt || askpass ? 2 : 1;
    if (!arguments[0] || !arguments[executable] || !arguments[executable + 1] ||
        strcmp(arguments[0], path) || strcmp(arguments[executable], "/fixture/bin/fanctl")) _Exit(81);
    printf("authorize=%s executable=%s argument=%s", path, arguments[executable], arguments[executable + 1]);
    for (int index = executable + 2; arguments[index]; index++) {
        if (strcmp(arguments[index], "--no-color") && strcmp(arguments[index], "--non-interactive") &&
            (strcmp(arguments[executable + 1], "stat") || strcmp(arguments[index], "--verbose"))) _Exit(81);
        printf(" %s", arguments[index]);
    }
    if (askpass && (!getenv("SUDO_ASKPASS") ||
        strcmp(getenv("SUDO_ASKPASS"), "/fixture/bin/admin_authorize") ||
        !getenv("ADMIN_AUTHORIZE_ASKPASS_SCOPE") ||
        strcmp(getenv("ADMIN_AUTHORIZE_ASKPASS_SCOPE"), "fanctl"))) _Exit(81);
    printf("%s\n", no_prompt ? " noninteractive=1" : askpass ? " askpass=1" : "");
    fflush(stdout);
    _Exit(0);
}
static kern_return_t forbidden_smc(mach_port_t port, CFDictionaryRef matching,
    io_iterator_t *iterator) {
    (void)port; (void)matching; (void)iterator;
    fputs("SMC must not run during the authorization test\n", stderr);
    _Exit(80);
}
#define main fanctl_command_main
#define geteuid fake_geteuid
#define _NSGetExecutablePath fake_executable
#define realpath fake_realpath
#define execv fake_execv
#if FANCTL_USE_ADJACENT_AUTHORIZER
#define fork fake_fork
#define waitpid fake_waitpid
#endif
#define IOServiceGetMatchingServices forbidden_smc
#ifndef FANCTL_SOURCE_INCLUDE
#define FANCTL_SOURCE_INCLUDE "../Bin/fanctl.c"
#endif
#include FANCTL_SOURCE_INCLUDE
#undef main
int main(int argc, char *argv[]) { return fanctl_command_main(argc, argv); }
