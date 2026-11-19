/* Non-privileged CLI authorization contract. No SMC call may run here. */
/* Modified on 2026-11-15: cached authorization and presentation argument transfer. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
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
static int fake_open_tty(const char *path, int flags, ...) {
    (void)path;
    return getenv("FANCTL_TEST_NO_TTY") ? -1 : open("/dev/null", flags);
}
#if FANCTL_USE_ADJACENT_AUTHORIZER
static int fake_isatty(int fd) { (void)fd; return 1; }
#endif
static int fake_lstat(const char *path, struct stat *st) {
    (void)path;
    if (!getenv("FANCTL_TEST_INSTALLED")) return -1;
    *st = (struct stat){.st_mode = S_IFREG | 0755};
    return 0;
}
#if FANCTL_USE_ADJACENT_AUTHORIZER
static int fake_access(const char *path, int mode) {
    return mode == X_OK && !strcmp(path, "/fixture/bin/fanctl_askpass") ? 0 : -1;
}
#endif
static int fake_execv(const char *path, char *const arguments[]) {
    if (getenv("FANCTL_TEST_EXEC_FAIL")) { errno = ENOENT; return -1; }
    bool no_prompt = arguments[1] && strcmp(arguments[1], "-n") == 0;
    bool askpass = arguments[1] && strcmp(arguments[1], "-A") == 0;
    int sep = no_prompt || askpass ? 2 : 1;
    if (strcmp(arguments[sep], "--") || arguments[sep+3] || strcmp(path, "/usr/bin/sudo")) _Exit(81);
    if (askpass && strcmp(getenv("SUDO_ASKPASS"), "/fixture/bin/fanctl_askpass")) _Exit(81);
    printf("%s %s %s\n", no_prompt ? "no-prompt" : askpass ? "local" : "terminal",
        arguments[sep+1], arguments[sep+2]);
    fflush(stdout);
    _Exit(0);
}

static kern_return_t denied_status(mach_port_t port, CFDictionaryRef matching,
    io_iterator_t *iterator) {
    (void)port; (void)matching; (void)iterator;
    return getenv("FANCTL_TEST_IO_ERROR") ? kIOReturnError : kIOReturnNotPrivileged;
}
#define main fanctl_command_main
#define geteuid fake_geteuid
#define _NSGetExecutablePath fake_executable
#define realpath fake_realpath
#define execv fake_execv
#if FANCTL_USE_ADJACENT_AUTHORIZER
#define access fake_access
#endif
#define open fake_open_tty
#define lstat fake_lstat
#if FANCTL_USE_ADJACENT_AUTHORIZER
#define isatty fake_isatty
#endif
#define IOServiceGetMatchingServices denied_status
#ifndef FANCTL_SOURCE_INCLUDE
#define FANCTL_SOURCE_INCLUDE "../Bin/fanctl.c"
#endif
#include FANCTL_SOURCE_INCLUDE
#undef main
int main(int argc, char *argv[]) {
    return fanctl_command_main(argc, argv);
}
