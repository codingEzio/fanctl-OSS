// SPDX-License-Identifier: GPL-2.0-only
// Apple SMC access is based on mac-fans-cli and smcFanControl.
// Original SMC portions: Copyright (C) 2006 devnull; (C) 2013 Michael Wilber.
// Credits also retain Hendrik Holtmann and park-jemin; see THIRD_PARTY_NOTICES.md.
// Modified by fanctl contributors on 2026-11-07: preflight, diagnostic and
// best-effort automatic-mode recovery when a forced fan change fails.
// Modified on 2026-11-15: wait for thermal-manager handoff and recognize system mode.
// Modified on 2026-11-19: direct status reads and optional motor observation.

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char major;
    char minor;
    char build;
    char reserved;
    uint16_t release;
} SMCKeyDataVers;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpu_plimit;
    uint32_t gpu_plimit;
    uint32_t mem_plimit;
} SMCKeyDataPLimit;

typedef struct {
    uint32_t data_size;
    uint32_t data_type;
    uint8_t data_attributes;
} SMCKeyInfo;

typedef struct {
    uint32_t key;
    SMCKeyDataVers vers;
    SMCKeyDataPLimit plimit_data;
    SMCKeyInfo key_info;
    uint8_t result;
    uint8_t status;
    uint8_t data8;
    uint32_t data32;
    uint8_t bytes[32];
} SMCKeyData;

typedef struct {
    char key[5];
    char data_type[5];
    size_t size;
    uint8_t bytes[32];
} SMCValue;

typedef struct {
    int number;
    float current;
    float minimum;
    float maximum;
    float target;
    int mode;
} FanState;

_Static_assert(sizeof(SMCKeyData) == 80, "AppleSMC data layout changed");

enum {
    kKernelIndexSMC = 2,
    kSMCCommandReadBytes = 5,
    kSMCCommandWriteBytes = 6,
    kSMCCommandReadKeyInfo = 9,
    kSMCSuccess = 0,
    kSMCKeyNotFound = 132,
    kSMCWriteProtected = 130,
    kTargetTolerance = 25,
    /* Thermal-manager handoff is asynchronous; use the researched 30s bound. */
    kModeAttempts = 120,
    kModePollMicroseconds = 250000,
    kSpeedAttempts = 32, /* Observe motor response for at most eight seconds. */
};

typedef struct {
    bool armed;
    bool attempted;
    bool touched_force_bits;
    bool touched_test_mode;
    bool pending_test_mode;
    io_connect_t connection;
    int count;
} ForceRecovery;
static ForceRecovery force_recovery;
static volatile sig_atomic_t interrupted;
static bool terminal_output;
static bool color_output;
static bool progress_terminal;
static int terminal_columns;
static bool progress_visible;

static void progress_clear(void) {
    if (progress_visible) {
        fputs("\r\033[2K", stderr);
        fflush(stderr);
        progress_visible = false;
    }
}

static bool reduce_motion(void);

static void progress_sample(const char *phase, const FanState *fan) {
    if (!progress_terminal) return;
    static const char *last_phase;
    static bool still;
    static struct timespec started;
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    if (!last_phase) {
        still = reduce_motion();
        started = now;
    }
    if (still && last_phase && strcmp(last_phase, phase) == 0) return;
    last_phase = phase;
    double elapsed = difftime(now.tv_sec, started.tv_sec) +
        (now.tv_nsec - started.tv_nsec) / 1e9;
    static const char *rotor[] = {"◐", "◓", "◑", "◒"};
    const char *mark = still ? "•" : rotor[now.tv_nsec / 250000000L];
    fprintf(stderr, "\r\033[2K%s%s%s  %s", color_output ? "\033[33m" : "",
        mark, color_output ? "\033[0m" : "", phase);
    if (fan && !still && terminal_columns >= 56) {
        fprintf(stderr, "  Fan %d  %.0f → %.0f RPM", fan->number + 1, fan->current, fan->target);
    }
    if (!still && elapsed >= 1) fprintf(stderr, "  %.0fs", elapsed);
    fflush(stderr);
    progress_visible = true;
}

static void progress_note(const char *phase) { progress_sample(phase, NULL); }

static void handle_signal(int number) { interrupted = number; }

static bool reduce_motion(void) {
    const char *name = getenv("SUDO_USER");
    CFStringRef user = name ? CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8) : NULL;
    CFPropertyListRef value = CFPreferencesCopyValue(CFSTR("reduceMotion"),
        CFSTR("com.apple.universalaccess"), user ? user : kCFPreferencesCurrentUser,
        kCFPreferencesAnyHost);
    bool enabled = value && CFGetTypeID(value) == CFBooleanGetTypeID() &&
        CFBooleanGetValue((CFBooleanRef)value);
    if (value) CFRelease(value);
    if (user) CFRelease(user);
    return enabled;
}

static bool restore_fans_best_effort(io_connect_t connection, int count,
    bool require_force_bits, bool require_test_mode);
static void fail(const char *message) __attribute__((noreturn));

static void fail(const char *message) {
    progress_clear();
    fprintf(stderr, "fanctl: %s\n", message);
    if (force_recovery.armed && force_recovery.attempted) {
        force_recovery.armed = false;
        bool restored = restore_fans_best_effort(force_recovery.connection,
            force_recovery.count, force_recovery.touched_force_bits,
            force_recovery.touched_test_mode);
        progress_clear();
        if (restored) {
            fputs("fanctl: automatic mode verified after the failed change\n", stderr);
        } else {
            fputs("fanctl: automatic recovery incomplete; run fanctl auto and verify fanctl stat\n",
                stderr);
        }
    }
    exit(interrupted ? 128 + interrupted : EXIT_FAILURE);
}

static void check_interrupted(void) {
    if (interrupted) fail("Cancelled");
}

/* Only an unprivileged, read-only status command may retry an IOKit denial.
   All rows are buffered until the full sample succeeds, so a retry prints once. */
static int stat_argc;
static char **stat_argv;
static void run_elevated(int argc, char *argv[]) __attribute__((noreturn));

static void fail_code(const char *action, kern_return_t code) {
    if (stat_argv && (code == kIOReturnNotPrivileged || code == kIOReturnNotPermitted)) {
        run_elevated(stat_argc, stat_argv);
    }
    char message[160];
    snprintf(message, sizeof(message), "%s: 0x%08x", action, (unsigned int)code);
    fail(message);
}

static void fail_smc_result(const char *action, uint8_t result) {
    char message[160];
    snprintf(message, sizeof(message), "%s: SMC result 0x%02x", action,
        (unsigned int)result);
    fail(message);
}

static uint32_t key_code(const char key[4]) {
    return ((uint32_t)(uint8_t)key[0] << 24) |
           ((uint32_t)(uint8_t)key[1] << 16) |
           ((uint32_t)(uint8_t)key[2] << 8) |
           (uint8_t)key[3];
}

static void type_name(uint32_t type, char result[5]) {
    for (size_t index = 0; index < 4; index++) {
        result[index] = (char)((type >> ((3 - index) * 8)) & 0xff);
    }
    result[4] = '\0';
}

static kern_return_t smc_call(io_connect_t connection, SMCKeyData *input,
    SMCKeyData *output) {
    size_t output_size = sizeof(*output);
    kern_return_t result = IOConnectCallStructMethod(connection, kKernelIndexSMC, input,
        sizeof(*input), output, &output_size);
    if (result == KERN_SUCCESS && output_size != sizeof(*output)) return kIOReturnUnderrun;
    return result;
}

static bool has_format(const SMCValue *value, const char *type, size_t size) {
    return value->size == size && strcmp(value->data_type, type) == 0;
}

static io_connect_t open_smc(void) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("AppleSMC"), &iterator);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot find AppleSMC service", result);
    }

    io_object_t device = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (device == IO_OBJECT_NULL) {
        fail("No AppleSMC service found");
    }

    io_connect_t connection = IO_OBJECT_NULL;
    result = IOServiceOpen(device, mach_task_self(), 0, &connection);
    IOObjectRelease(device);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot open AppleSMC", result);
    }
    return connection;
}

static SMCKeyInfo get_key_info(io_connect_t connection, const char key[4]) {
    SMCKeyData input = {0};
    SMCKeyData output = {0};
    input.key = key_code(key);
    input.data8 = kSMCCommandReadKeyInfo;
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot read SMC key metadata", result);
    }
    if (output.result != kSMCSuccess) {
        fail_smc_result("Cannot read SMC key metadata", output.result);
    }
    return output.key_info;
}

static SMCValue read_key(io_connect_t connection, const char key[4]) {
    SMCKeyData input = {0};
    SMCKeyData output = {0};
    SMCValue value = {0};
    SMCKeyInfo key_info = get_key_info(connection, key);
    if (key_info.data_size == 0 || key_info.data_size > sizeof(value.bytes)) {
        fail("Invalid SMC key data size");
    }

    input.key = key_code(key);
    input.key_info.data_size = key_info.data_size;
    input.data8 = kSMCCommandReadBytes;
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot read SMC key", result);
    }
    if (output.result != kSMCSuccess) {
        fail_smc_result("Cannot read SMC key", output.result);
    }

    memcpy(value.key, key, 4);
    type_name(key_info.data_type, value.data_type);
    value.size = key_info.data_size;
    memcpy(value.bytes, output.bytes, value.size);
    return value;
}

static bool try_read_key(io_connect_t connection, const char key[4], SMCValue *value) {
    SMCKeyData input = {0};
    SMCKeyData output = {0};
    input.key = key_code(key);
    input.data8 = kSMCCommandReadKeyInfo;
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot read optional SMC key metadata", result);
    }
    if (output.result == kSMCKeyNotFound) {
        return false;
    }
    if (output.result != kSMCSuccess) {
        fail_smc_result("Cannot read optional SMC key metadata", output.result);
    }
    if (output.key_info.data_size == 0 || output.key_info.data_size > sizeof(value->bytes)) {
        fail("Invalid optional SMC key data size");
    }
    SMCKeyInfo key_info = output.key_info;

    input = (SMCKeyData){0};
    input.key = key_code(key);
    input.key_info.data_size = key_info.data_size;
    input.data8 = kSMCCommandReadBytes;
    result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot read optional SMC key", result);
    }
    if (output.result == kSMCKeyNotFound) {
        return false;
    }
    if (output.result != kSMCSuccess) {
        fail_smc_result("Cannot read optional SMC key", output.result);
    }

    memcpy(value->key, key, 4);
    type_name(key_info.data_type, value->data_type);
    value->size = key_info.data_size;
    memcpy(value->bytes, output.bytes, value->size);
    return true;
}

static uint8_t write_key_result(io_connect_t connection, const SMCValue *value) {
    SMCValue existing = read_key(connection, value->key);
    if (existing.size != value->size) {
        fail("SMC key data-size mismatch");
    }
    if (strcmp(existing.data_type, value->data_type) != 0) {
        fail("SMC key format changed before write");
    }

    SMCKeyData input = {0};
    SMCKeyData output = {0};
    input.key = key_code(value->key);
    input.key_info.data_size = (uint32_t)value->size;
    input.data8 = kSMCCommandWriteBytes;
    memcpy(input.bytes, value->bytes, value->size);
    if (force_recovery.armed) {
        /* A transport failure may occur after the controller applied the write. */
        force_recovery.attempted = true;
        if (memcmp(value->key, "FS! ", 4) == 0) force_recovery.touched_force_bits = true;
        if (memcmp(value->key, "Ftst", 4) == 0) force_recovery.touched_test_mode = true;
        if (memcmp(value->key, "Ftst", 4) == 0 && value->bytes[0] == 1) {
            force_recovery.pending_test_mode = true;
        }
    }
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot write SMC key", result);
    }
    return output.result;
}

static void write_key(io_connect_t connection, const SMCValue *value) {
    uint8_t result = write_key_result(connection, value);
    if (result != kSMCSuccess) fail_smc_result("Cannot write SMC key", result);
}

typedef enum { kRecoveryReadOK, kRecoveryReadMissing, kRecoveryReadError } RecoveryRead;

/* Recovery cannot call fail(): one broken key must not prevent the other resets. */
static RecoveryRead recovery_read_key(io_connect_t connection, const char key[4],
    SMCValue *value) {
    SMCKeyData input = {0};
    SMCKeyData output = {0};
    input.key = key_code(key);
    input.data8 = kSMCCommandReadKeyInfo;
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "fanctl: %.4s metadata transport error: 0x%08x\n", key,
            (unsigned int)result);
        return kRecoveryReadError;
    }
    if (output.result == kSMCKeyNotFound) return kRecoveryReadMissing;
    if (output.result != kSMCSuccess) {
        fprintf(stderr, "fanctl: %.4s metadata SMC result: 0x%02x\n", key,
            (unsigned int)output.result);
        return kRecoveryReadError;
    }
    if (output.key_info.data_size == 0 || output.key_info.data_size > sizeof(value->bytes)) {
        fprintf(stderr, "fanctl: %.4s has an invalid metadata size\n", key);
        return kRecoveryReadError;
    }
    SMCKeyInfo info = output.key_info;
    input = (SMCKeyData){0};
    output = (SMCKeyData){0};
    input.key = key_code(key);
    input.key_info.data_size = info.data_size;
    input.data8 = kSMCCommandReadBytes;
    result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "fanctl: %.4s read transport error: 0x%08x\n", key,
            (unsigned int)result);
        return kRecoveryReadError;
    }
    if (output.result == kSMCKeyNotFound) return kRecoveryReadMissing;
    if (output.result != kSMCSuccess) {
        fprintf(stderr, "fanctl: %.4s read SMC result: 0x%02x\n", key,
            (unsigned int)output.result);
        return kRecoveryReadError;
    }
    memcpy(value->key, key, 4);
    type_name(info.data_type, value->data_type);
    value->size = info.data_size;
    memcpy(value->bytes, output.bytes, value->size);
    return kRecoveryReadOK;
}

static bool recovery_write_key(io_connect_t connection, const SMCValue *value, bool fan_mode) {
    /* Match the established writer's second metadata/read before every write. */
    SMCValue existing = {0};
    if (recovery_read_key(connection, value->key, &existing) != kRecoveryReadOK ||
        existing.size != value->size || strcmp(existing.data_type, value->data_type) != 0) {
        fprintf(stderr, "fanctl: %.4s could not be checked before recovery write\n", value->key);
        return false;
    }
    SMCKeyData input = {0};
    SMCKeyData output = {0};
    input.key = key_code(value->key);
    input.key_info.data_size = (uint32_t)value->size;
    input.data8 = kSMCCommandWriteBytes;
    memcpy(input.bytes, value->bytes, value->size);
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "fanctl: %.4s write transport error: 0x%08x\n", value->key,
            (unsigned int)result);
        return false;
    }
    if (output.result != kSMCSuccess) {
        if (fan_mode && output.result == kSMCWriteProtected) {
            SMCValue readback = {0};
            if (recovery_read_key(connection, value->key, &readback) == kRecoveryReadOK &&
                has_format(&readback, "ui8 ", 1) &&
                (readback.bytes[0] == 0 || readback.bytes[0] == 3)) return true;
        }
        fprintf(stderr, "fanctl: %.4s write SMC result: 0x%02x\n", value->key,
            (unsigned int)output.result);
        return false;
    }
    return true;
}

typedef struct {
    char key[5];
    size_t zero_bytes;
    bool optional;
    bool required;
    bool seen;
    bool fan_mode;
} RecoveryKey;

static bool recovery_write_zero(io_connect_t connection, RecoveryKey *entry) {
    SMCValue value = {0};
    RecoveryRead status = recovery_read_key(connection, entry->key, &value);
    if (status == kRecoveryReadMissing && entry->optional && !entry->required) return true;
    if (status == kRecoveryReadOK) entry->seen = true;
    const char *type = entry->zero_bytes == 2 ? "ui16" : "ui8 ";
    if (status != kRecoveryReadOK || !has_format(&value, type, entry->zero_bytes)) {
        fprintf(stderr, "fanctl: cannot restore key %.4s\n", entry->key);
        return false;
    }
    /* Mode 3 already belongs to the system and rejects mode writes with 0x82.
       Global overrides are still cleared and verified before recovery succeeds. */
    if (entry->fan_mode && value.bytes[0] == 3) return true;
    bool zero = true;
    for (size_t index = 0; index < entry->zero_bytes; index++) {
        if (value.bytes[index] != 0) zero = false;
    }
    if (zero && (entry->fan_mode || !entry->required)) return true;
    memset(value.bytes, 0, entry->zero_bytes);
    return recovery_write_key(connection, &value, entry->fan_mode);
}

typedef enum { kVerifyZero, kVerifyPending, kVerifyError } RecoveryVerify;

static RecoveryVerify recovery_verify_zero(io_connect_t connection, RecoveryKey *entry,
    int attempt) {
    SMCValue readback = {0};
    RecoveryRead status = recovery_read_key(connection, entry->key, &readback);
    if (status == kRecoveryReadMissing && entry->optional && !entry->required && !entry->seen) {
        return kVerifyZero;
    }
    if (status == kRecoveryReadOK) entry->seen = true;
    const char *type = entry->zero_bytes == 2 ? "ui16" : "ui8 ";
    bool format_ok = status == kRecoveryReadOK && has_format(&readback, type, entry->zero_bytes);
    bool zero = format_ok;
    if (format_ok && entry->fan_mode && readback.bytes[0] == 3) return kVerifyZero;
    for (size_t index = 0; zero && index < entry->zero_bytes; index++) {
        if (readback.bytes[index] != 0) zero = false;
    }
    if (zero) return kVerifyZero;
    if (format_ok && attempt < kModeAttempts) return kVerifyPending;
    struct timespec timestamp = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &timestamp);
    fprintf(stderr, "fanctl: %.4s verify attempt %d: status=%d type=%.4s size=%zu "
        "bytes=%02x%02x monotonic=%lld.%09ld\n", entry->key, attempt, (int)status,
        readback.data_type, readback.size, (unsigned int)readback.bytes[0],
        (unsigned int)readback.bytes[1], (long long)timestamp.tv_sec, timestamp.tv_nsec);
    if (!format_ok) {
        fprintf(stderr, "fanctl: cannot verify automatic value for key %.4s\n", entry->key);
        return kVerifyError;
    }
    return kVerifyPending;
}

static bool restore_fans_best_effort(io_connect_t connection, int count,
    bool require_force_bits, bool require_test_mode) {
    RecoveryKey keys[12] = {0};
    size_t total = 0;
    memcpy(keys[total].key, "FS! ", 4);
    keys[total].zero_bytes = 2;
    keys[total].optional = true;
    keys[total++].required = require_force_bits;
    for (int number = 0; number < count; number++) {
        const char key[4] = {'F', (char)('0' + number), 'M', 'd'};
        memcpy(keys[total].key, key, 4);
        keys[total].zero_bytes = 1;
        keys[total].fan_mode = true;
        keys[total++].required = true;
    }
    memcpy(keys[total].key, "Ftst", 4);
    keys[total].zero_bytes = 1;
    keys[total].optional = true;
    keys[total++].required = require_test_mode;
    bool success = true;
    /* An accepted Ftst=1 write can still read as zero for several seconds.
       Drain that pending transition before clearing it, so a late enable cannot
       land after recovery has already reported automatic mode. */
    if (force_recovery.pending_test_mode) {
        bool acknowledged = false;
        for (int attempt = 0; attempt < kModeAttempts; attempt++) {
            SMCValue value = {0};
            if (recovery_read_key(connection, "Ftst", &value) != kRecoveryReadOK ||
                !has_format(&value, "ui8 ", 1)) break;
            if (value.bytes[0] == 1) { acknowledged = true; break; }
            if (value.bytes[0] != 0) break;
            progress_note("Restoring Auto");
            if (attempt + 1 < kModeAttempts) usleep(kModePollMicroseconds);
        }
        force_recovery.pending_test_mode = false;
        if (!acknowledged) {
            fputs("fanctl: pending control handoff could not be confirmed before recovery\n", stderr);
            success = false;
        }
    }
    /* Attempt every reset before verification: some controllers apply a mode
       only after all control keys have been cleared. Transport and unexpected
       SMC write failures remain failures even when the final read shows zero. */
    for (size_t index = 0; index < total; index++) {
        if (!recovery_write_zero(connection, &keys[index])) success = false;
    }
    bool verified = false;
    /* Reuse the thermal-handoff window; only a valid nonzero value is retried.
       Missing/invalid reads or failed writes return promptly for fallback. */
    int attempts = success ? kModeAttempts : 1;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        bool all_zero = true;
        bool error = false;
        for (size_t index = 0; index < total; index++) {
            RecoveryVerify result = recovery_verify_zero(connection, &keys[index], attempt);
            if (result != kVerifyZero) all_zero = false;
            if (result == kVerifyError) error = true;
        }
        if (all_zero) { verified = true; break; }
        if (error || attempt == attempts) break;
        progress_note("Returning to Auto");
        usleep(kModePollMicroseconds);
    }
    return success && verified;
}

static uint32_t unsigned_value(const SMCValue *value) {
    uint32_t result = 0;
    for (size_t index = 0; index < value->size; index++) {
        result = (result << 8) | value->bytes[index];
    }
    return result;
}

static float numeric_value(const SMCValue *value) {
    if (strcmp(value->data_type, "flt ") == 0 && value->size == sizeof(float)) {
        float result;
        memcpy(&result, value->bytes, sizeof(result));
        return result;
    }
    if (strcmp(value->data_type, "fpe2") == 0 && value->size == 2) {
        return (float)unsigned_value(value) / 4.0f;
    }
    if ((strcmp(value->data_type, "ui8 ") == 0 || strcmp(value->data_type, "ui16") == 0 ||
            strcmp(value->data_type, "ui32") == 0) && value->size <= sizeof(uint32_t)) {
        return (float)unsigned_value(value);
    }
    fail("Unsupported SMC numeric data type");
}

static void fan_key(int number, const char suffix[2], char key[4]) {
    if (number < 0 || number > 9) {
        fail("Invalid fan number");
    }
    key[0] = 'F';
    key[1] = (char)('0' + number);
    key[2] = suffix[0];
    key[3] = suffix[1];
}

static int fan_count(io_connect_t connection) {
    SMCValue value;
    if (try_read_key(connection, "FNum", &value)) {
        float count = numeric_value(&value);
        if (!isfinite(count) || count < 0 || count > 10 || floorf(count) != count) {
            fail("Invalid fan count");
        }
        return (int)count;
    }

    int count = 0;
    for (int number = 0; number < 8; number++) {
        char key[4];
        fan_key(number, "Ac", key);
        if (!try_read_key(connection, key, &value)) {
            break;
        }
        count++;
    }
    return count;
}

static SMCValue read_fan_key(io_connect_t connection, int number, const char suffix[2]) {
    char key[4];
    fan_key(number, suffix, key);
    return read_key(connection, key);
}

static float fan_rpm(io_connect_t connection, int number, const char suffix[2]) {
    SMCValue value = read_fan_key(connection, number, suffix);
    float rpm = numeric_value(&value);
    if (!isfinite(rpm) || rpm < 0) fail("Invalid fan RPM data");
    return rpm;
}

static int fan_mode(io_connect_t connection, int number) {
    SMCValue value = read_fan_key(connection, number, "Md");
    float mode = numeric_value(&value);
    if (!isfinite(mode) || mode < 0 || (double)mode > (double)INT32_MAX ||
        floorf(mode) != mode) fail("Invalid fan mode");
    return (int)mode;
}

static FanState read_fan(io_connect_t connection, int number) {
    FanState fan = {.number = number};
    fan.current = fan_rpm(connection, number, "Ac");
    fan.minimum = fan_rpm(connection, number, "Mn");
    fan.maximum = fan_rpm(connection, number, "Mx");
    fan.target = fan_rpm(connection, number, "Tg");
    fan.mode = fan_mode(connection, number);
    if (fan.minimum > fan.maximum) fail("Invalid fan RPM data");
    return fan;
}

static int all_fans(io_connect_t connection, FanState fans[10]) {
    int count = fan_count(connection);
    if (count <= 0 || count > 10) {
        fail("No fans detected");
    }
    for (int number = 0; number < count; number++) {
        fans[number] = read_fan(connection, number);
    }
    return count;
}

static void set_optional_byte(io_connect_t connection, const char key[4], uint8_t byte,
    bool expected) {
    SMCValue value;
    if (try_read_key(connection, key, &value)) {
        if (!has_format(&value, "ui8 ", 1)) fail("Unsupported optional byte key format");
        if (value.bytes[0] == byte) return;
        value.bytes[0] = byte;
        write_key(connection, &value);
    } else if (expected) fail("Previously detected optional byte key disappeared");
}

static void set_optional_force_bits(io_connect_t connection, uint16_t bits, bool expected) {
    SMCValue value;
    if (try_read_key(connection, "FS! ", &value)) {
        if (!has_format(&value, "ui16", 2)) fail("Unsupported global force key format");
        if (unsigned_value(&value) == bits) return;
        value.bytes[0] = (uint8_t)(bits >> 8);
        value.bytes[1] = (uint8_t)bits;
        write_key(connection, &value);
    } else if (expected) fail("Previously detected global force key disappeared");
}

static void set_manual_modes(io_connect_t connection, const FanState *fans, int count) {
    int attempts = force_recovery.touched_test_mode ? kModeAttempts : 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        check_interrupted();
        bool all_manual = true;
        for (int index = 0; index < count; index++) {
            SMCValue value = read_fan_key(connection, fans[index].number, "Md");
            if (!has_format(&value, "ui8 ", 1)) fail("Unsupported fan mode key format");
            if (value.bytes[0] == 1) continue;
            value.bytes[0] = 1;
            uint8_t result = write_key_result(connection, &value);
            if (result != kSMCSuccess && !(result == kSMCWriteProtected && attempts > 1)) {
                fail_smc_result("Cannot write fan mode", result);
            }
            SMCValue readback = read_fan_key(connection, fans[index].number, "Md");
            if (!has_format(&readback, "ui8 ", 1)) fail("Unsupported fan mode key format");
            if (result != kSMCSuccess || readback.bytes[0] != 1) all_manual = false;
        }
        if (all_manual) return;
        progress_note("Taking control");
        if (attempt + 1 < attempts) usleep(kModePollMicroseconds);
    }
    fail(attempts > 1 ? "macOS did not release fan control within 30 seconds" :
        "Manual fan mode could not be verified");
}

static SMCValue encoded_target(io_connect_t connection, int number, int rpm) {
    SMCValue value = read_fan_key(connection, number, "Tg");
    if (strcmp(value.data_type, "flt ") == 0 && value.size == sizeof(float)) {
        float target = (float)rpm;
        memcpy(value.bytes, &target, sizeof(target));
        return value;
    }
    if (strcmp(value.data_type, "fpe2") == 0 && value.size == 2) {
        if (rpm < 0 || (unsigned int)rpm > UINT16_MAX / 4U) {
            fail("RPM cannot be represented by the fan target key");
        }
        uint16_t encoded = (uint16_t)((unsigned int)rpm << 2);
        value.bytes[0] = (uint8_t)(encoded >> 8);
        value.bytes[1] = (uint8_t)encoded;
        return value;
    }
    fail("Unsupported fan target data type");
}

static void restore_auto(io_connect_t connection) {
    int count = fan_count(connection);
    if (count <= 0 || count > 10) {
        fail("No fans detected");
    }
    if (!restore_fans_best_effort(connection, count, false, false)) {
        fail("Automatic mode could not be verified for every fan");
    }
}

static const char *mode_label(int mode) {
    return mode == 0 || mode == 3 ? "Auto" : mode == 1 ? "Manual" : "Unknown";
}

/* Each tachometer spans zero to this fan's rated maximum. The number remains
   legible on its own in a narrow terminal or a monochrome color scheme. */
static void print_meter(float current, float maximum, int segments) {
    if (segments == 0 || maximum <= 0) return;
    int lit = (int)lroundf(fminf(current / maximum, 1) * segments);
    for (int i = 0; i < segments; i++) {
        if (color_output) {
            if (i == lit) fputs("\033[0;2m", stdout);
            else if (i < lit) {
                if (i == 0) fputs("\033[33m", stdout);
                else if (i == segments / 2) fputs("\033[38;5;208m", stdout);
                else if (i == segments * 3 / 4) fputs("\033[31m", stdout);
            }
        }
        fputs(i < lit ? "▰" : "·", stdout);
    }
    if (color_output) fputs("\033[0m", stdout);
    fputs("  ", stdout);
}

static void print_terminal_fan(const FanState *fan, const FanState *before, bool maximum,
    bool verbose) {
    const char *bold = color_output ? "\033[1m" : "";
    const char *reset = color_output ? "\033[0m" : "";
    printf(terminal_columns < 32 ? "Fan %d " : "Fan %02d  ", fan->number + 1);
    int segments = terminal_columns >= 72 ? 16 : terminal_columns >= 48 ? 8 : 0;
    print_meter(fan->current, fan->maximum, segments);
    printf("%s%5.0f%s RPM  %s\n", bold, fan->current, reset,
        maximum ? "Max" : mode_label(fan->mode));
    if (before) {
        printf("        from %.0f RPM", before->current);
        if (fan->mode == 1 && fabsf(fan->current - fan->target) > kTargetTolerance) {
            printf("%starget %.0f", terminal_columns >= 48 ? "  ·  " : "\n        ", fan->target);
        }
        putchar('\n');
    } else if (verbose) {
        printf("        target %.0f%srange %.0f–%.0f%sraw mode %d\n",
            fan->target, terminal_columns >= 48 ? "  ·  " : "\n        ", fan->minimum, fan->maximum,
            terminal_columns >= 64 ? "  ·  " : "\n        ", fan->mode);
    }
}

static void print_status(io_connect_t connection, bool verbose) {
    FanState fans[10];
    int count = all_fans(connection, fans);
    if (verbose && !terminal_output) printf("Fans: %d\n", count);
    for (int number = 0; number < count; number++) {
        const FanState *fan = &fans[number];
        if (terminal_output) {
            print_terminal_fan(fan, NULL, false, verbose);
        } else if (verbose) {
            printf("Fan %d: %.0f RPM current, %.0f target, %.0f-%.0f allowed, %s (mode %d)\n",
                fan->number + 1, fan->current, fan->target, fan->minimum, fan->maximum,
                mode_label(fan->mode), fan->mode);
        } else {
            printf("Fan %d  %5.0f RPM  %s\n", fan->number + 1, fan->current, mode_label(fan->mode));
        }
    }
}

static void finish_change(io_connect_t connection, const FanState *before, int count,
    bool automatic, bool maximum, int rpm, bool wait) {
    FanState after[10];
    for (int attempt = 0; attempt < kSpeedAttempts; attempt++) {
        check_interrupted();
        if (fan_count(connection) != count) fail("Fan count changed during control");
        const FanState *unsettled = NULL;
        for (int index = 0; index < count; index++) {
            FanState *fan = &after[index];
            fan->number = before[index].number;
            fan->maximum = before[index].maximum;
            fan->current = fan_rpm(connection, fan->number, "Ac");
            fan->target = fan_rpm(connection, fan->number, "Tg");
            fan->mode = fan_mode(connection, fan->number);
            float target = automatic ? fan->target : maximum ? before[index].maximum : (float)rpm;
            if (!automatic && (fan->mode != 1 || fabsf(fan->target - target) > kTargetTolerance)) {
                fail("macOS changed fan control before the command completed");
            }
            if (automatic && fan->mode != 0 && fan->mode != 3) fail("Automatic mode was lost");
            if (!unsettled && fabsf(fan->current - target) > kTargetTolerance) unsettled = fan;
        }
        if (!wait || !unsettled || attempt + 1 == kSpeedAttempts) break;
        progress_sample("Tracking RPM", unsettled);
        usleep(kModePollMicroseconds);
    }
    check_interrupted();
    progress_clear();
    force_recovery.armed = false;
    for (int index = 0; index < count; index++) {
        const FanState *fan = &after[index];
        if (terminal_output) {
            print_terminal_fan(fan, &before[index], maximum, false);
            continue;
        }
        printf("Fan %d  %5.0f -> %5.0f RPM%s", fan->number + 1, before[index].current,
            fan->current, automatic ? "  Auto" : maximum ? "  Max" : "");
        if (!automatic && fabsf(fan->current - fan->target) > kTargetTolerance) {
            printf("  (target %.0f)", fan->target);
        }
        putchar('\n');
    }
}

static int parse_rpm(const char *text) {
    char *end = NULL;
    long result = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || result < 0 || result > INT32_MAX) {
        fail("RPM must be a non-negative integer");
    }
    return (int)result;
}

static bool is_rpm_text(const char *text) {
    char *end = NULL;
    strtol(text, &end, 10);
    return text[0] != '\0' && *end == '\0';
}

static void force_fans(io_connect_t connection, const FanState *fans, int count,
    bool use_rpm, int rpm) {
    if (count <= 0 || count > 10) fail("No fans detected");
    int targets[10];
    SMCValue encoded[10];
    for (int number = 0; number < count; number++) {
        const FanState *fan = &fans[number];
        if (!isfinite(fan->minimum) || !isfinite(fan->maximum) || fan->minimum < 0 ||
            fan->minimum > fan->maximum || (double)fan->maximum > (double)INT32_MAX) {
            fail("Invalid fan RPM range");
        }
        long target = use_rpm ? rpm : lroundf(fan->maximum);
        if (target < 0 || target > INT32_MAX || (float)target < fan->minimum ||
            (float)target > fan->maximum) {
            fail("RPM is outside a fan's allowed range");
        }
        targets[number] = (int)target;
        SMCValue mode = read_fan_key(connection, fan->number, "Md");
        if (!has_format(&mode, "ui8 ", 1)) fail("Unsupported fan mode key format");
        encoded[number] = encoded_target(connection, fan->number, targets[number]);
    }
    SMCValue optional = {0};
    bool saw_test_mode = try_read_key(connection, "Ftst", &optional);
    if (saw_test_mode && !has_format(&optional, "ui8 ", 1)) {
        fail("Unsupported optional byte key format");
    }
    bool saw_force_bits = try_read_key(connection, "FS! ", &optional);
    if (saw_force_bits && !has_format(&optional, "ui16", 2)) {
        fail("Unsupported global force key format");
    }
    check_interrupted();
    force_recovery = (ForceRecovery){.armed = true, .connection = connection,
        .count = count, .touched_force_bits = saw_force_bits,
        .touched_test_mode = saw_test_mode};
    set_optional_byte(connection, "Ftst", 1, saw_test_mode);
    set_optional_force_bits(connection, (uint16_t)((1U << count) - 1), saw_force_bits);
    set_manual_modes(connection, fans, count);
    for (int number = 0; number < count; number++) {
        check_interrupted();
        write_key(connection, &encoded[number]);
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        check_interrupted();
        bool reached = true;
        for (int number = 0; number < count; number++) {
            FanState current = read_fan(connection, fans[number].number);
            if (current.mode != 1 || fabsf(current.target - (float)targets[number]) > kTargetTolerance) {
                reached = false;
            }
        }
        if (reached) return;
        progress_note("Applying RPM");
        if (attempt + 1 < 8) usleep(kModePollMicroseconds);
    }
    fail("Fan target did not settle at the requested RPM");
}

static void usage(void) {
    if (terminal_output) {
        const char *accent = color_output ? "\033[1;33m" : "";
        const char *bold = color_output ? "\033[1m" : "";
        const char *reset = color_output ? "\033[0m" : "";
        printf("%s◉  fanctl%s\n\n", accent, reset);
        printf("  %sstat%s     Read fan speeds and modes\n", bold, reset);
        printf("  %s<RPM>%s    Set all fans to this speed\n", bold, reset);
        printf("  %smax%s      Use each fan’s maximum\n", bold, reset);
        printf("  %sauto%s     Return control to macOS\n\n", bold, reset);
        puts("  stat --verbose     Targets, ranges and raw modes\n"
             "  --wait             Observe RPM for up to 8s after changes\n"
             "  --non-interactive  Never prompt for a password\n"
             "  --no-color         Disable color");
        return;
    }
    puts("fanctl — macOS fan inspection and temporary control\n"
         "\n"
         "Usage:\n"
         "  fanctl max [--wait]\n"
         "  fanctl auto [--wait]\n"
         "  fanctl <RPM> [--wait]\n"
         "  fanctl stat [--verbose]\n"
         "\n"
         "Options:\n"
         "  --wait             Observe measured RPM for up to 8s after confirmation\n"
         "  --no-color         Disable color (also: NO_COLOR)\n"
         "  --non-interactive  Never open a password prompt");
}

// Standalone builds use system sudo. The managed owner build explicitly keeps
// its adjacent native authorizer; no private home path is compiled by default.
#ifndef FANCTL_USE_ADJACENT_AUTHORIZER
#define FANCTL_USE_ADJACENT_AUTHORIZER 0
#endif
#if FANCTL_USE_ADJACENT_AUTHORIZER != 0 && FANCTL_USE_ADJACENT_AUTHORIZER != 1
#error "FANCTL_USE_ADJACENT_AUTHORIZER must be 0 or 1"
#endif

#if FANCTL_USE_ADJACENT_AUTHORIZER
/* Probe sudo's own cache without a prompt. Never keep a password in fanctl. */
static bool cached_authorization(void) {
    pid_t child = fork();
    if (child < 0) fail("Cannot check cached administrator access");
    if (child == 0) {
        int sink = open("/dev/null", O_RDWR);
        if (sink < 0 || dup2(sink, STDIN_FILENO) < 0 ||
            dup2(sink, STDOUT_FILENO) < 0 || dup2(sink, STDERR_FILENO) < 0) _exit(127);
        if (sink > STDERR_FILENO) close(sink);
        char *arguments[] = {"/usr/bin/sudo", "-n", "-v", NULL};
        execv(arguments[0], arguments);
        _exit(127);
    }
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result < 0) fail("Cannot read cached administrator access result");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

static void run_elevated(int argc, char *argv[]) {
    bool noninteractive = false;
    for (int index = 2; index < argc; index++) {
        if (strcmp(argv[index], "--non-interactive") == 0) noninteractive = true;
    }
    char executable[PATH_MAX];
    char resolved[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0 ||
        realpath(executable, resolved) == NULL) {
        fail("Cannot resolve this executable for administrator access");
    }
#if FANCTL_USE_ADJACENT_AUTHORIZER
    char authorizer[PATH_MAX];
    const char *separator = strrchr(resolved, '/');
    if (separator == NULL || snprintf(authorizer, sizeof(authorizer),
        "%.*s/admin_authorize", (int)(separator - resolved), resolved) >= (int)sizeof(authorizer)) {
        fail("Cannot locate the configured administrator helper");
    }
    bool cached = !noninteractive && cached_authorization();
    const char *ssh_connection = getenv("SSH_CONNECTION");
    const char *ssh_client = getenv("SSH_CLIENT");
    bool incoming_ssh = (ssh_connection && ssh_connection[0]) || (ssh_client && ssh_client[0]);
    bool askpass = !cached && !noninteractive && !incoming_ssh;
    if (askpass && (setenv("SUDO_ASKPASS", authorizer, 1) != 0 ||
        setenv("ADMIN_AUTHORIZE_ASKPASS_SCOPE", "fanctl", 1) != 0)) {
        fail("Cannot configure native administrator authorization");
    }
    /* Askpass lets sudo create its normal credential timestamp. Executing
       through the command helper would use -k and deliberately suppress it. */
    const char *selected_authorizer = cached || noninteractive || askpass ?
        "/usr/bin/sudo" : authorizer;
#else
    bool cached = false;
    bool askpass = false;
    const char *selected_authorizer = "/usr/bin/sudo";
#endif

    char *authorize_args[argc + 4];
    int next = 0;
    authorize_args[next++] = (char *)selected_authorizer;
    if (cached || noninteractive) authorize_args[next++] = "-n";
    else if (askpass) authorize_args[next++] = "-A";
    authorize_args[next++] = resolved;
    bool has_no_color = false;
    for (int index = 1; index < argc; index++) {
        authorize_args[next++] = argv[index];
        if (strcmp(argv[index], "--no-color") == 0) has_no_color = true;
    }
    /* sudo can remove NO_COLOR from its environment; preserve the user's
       presentation choice as a normal CLI flag across either authorizer. */
    if (getenv("NO_COLOR") != NULL && !has_no_color) authorize_args[next++] = "--no-color";
    authorize_args[next] = NULL;
    execv(selected_authorizer, authorize_args);
    fail("Cannot request administrator access");
}

int main(int argc, char *argv[]) {
    bool no_color = getenv("NO_COLOR") != NULL;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--no-color") == 0) no_color = true;
    }
    const char *term = getenv("TERM");
    terminal_output = isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb") != 0);
    color_output = terminal_output && !no_color;
    if (terminal_output) {
        struct winsize size = {0};
        terminal_columns = ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col
            ? size.ws_col : 80;
    }

    if (argc == 1 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        usage();
        return EXIT_SUCCESS;
    }

    bool wait = false;
    bool verbose = false;

    const char *command = argv[1];
    bool want_stat = strcmp(command, "stat") == 0;
    bool want_max = strcmp(command, "max") == 0;
    bool want_auto = strcmp(command, "auto") == 0;
    bool want_set = is_rpm_text(command);
    if (!(want_stat || want_max || want_auto || want_set)) {
        usage();
        return EXIT_FAILURE;
    }
    for (int index = 2; index < argc; index++) {
        if (strcmp(argv[index], "--no-color") == 0) continue;
        else if (!want_stat && strcmp(argv[index], "--wait") == 0) wait = true;
        else if (want_stat && strcmp(argv[index], "--verbose") == 0) verbose = true;
        else if (strcmp(argv[index], "--non-interactive") == 0) continue;
        else { usage(); return EXIT_FAILURE; }
    }

    int rpm = want_set ? parse_rpm(command) : 0;
    if (geteuid() != 0) {
        if (!want_stat) run_elevated(argc, argv);
        stat_argc = argc;
        stat_argv = argv;
    }

    progress_terminal = !want_stat && terminal_output && isatty(STDERR_FILENO);

    io_connect_t connection = open_smc();
    if (want_stat) {
        print_status(connection, verbose);
    } else {
        struct sigaction action = {0};
        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
            fail("Cannot install cancellation handler");
        }
        FanState fans[10];
        int count = all_fans(connection, fans);
        if (want_set || want_max) force_fans(connection, fans, count, want_set, rpm);
        else restore_auto(connection);
        finish_change(connection, fans, count, want_auto, want_max, rpm, wait);
    }

    IOServiceClose(connection);
    return EXIT_SUCCESS;
}
