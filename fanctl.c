// SPDX-License-Identifier: GPL-2.0-only
// Apple SMC access is based on mac-fans-cli and smcFanControl.
// Original SMC portions: Copyright (C) 2006 devnull; (C) 2013 Michael Wilber.
// Credits also retain Hendrik Holtmann and park-jemin; see THIRD_PARTY_NOTICES.md.
// Modified by fanctl contributors on 2026-11-07: preflight, diagnostic and
// best-effort automatic-mode recovery when a forced fan change fails.
// Modified on 2026-11-15: wait for thermal-manager handoff and recognize system mode.
// Modified on 2026-11-19: direct RPM reads, verified control changes and session-aware authorization.

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
#include <sys/stat.h>
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
    /* Thermal-manager handoff is asynchronous; use the researched 30s bound. */
    kModeAttempts = 120,
    kModePollMicroseconds = 250000,
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
static void handle_signal(int number) { interrupted = number; }

static bool restore_fans_best_effort(io_connect_t connection, int count,
    bool require_force_bits, bool require_test_mode);
static void fail(const char *message) __attribute__((noreturn));

static void fail(const char *message) {
    fprintf(stderr, "fanctl: %s\n", message);
    if (force_recovery.armed && force_recovery.attempted) {
        force_recovery.armed = false;
        bool restored = restore_fans_best_effort(force_recovery.connection,
            force_recovery.count, force_recovery.touched_force_bits,
            force_recovery.touched_test_mode);
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

/* The unprivileged preflight only reads. Permission failures can safely retry. */
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

/* Subdivide the first quarter-second so an early confirmation can return
   immediately. Afterwards keep the original cadence and total timeout. */
static const useconds_t early_polls[] = {10000, 20000, 40000, 80000, 100000};
enum { kExtraEarlyPolls = sizeof(early_polls) / sizeof(early_polls[0]) - 1 };

static void poll_change(int attempt) {
    usleep(attempt <= kExtraEarlyPolls ? early_polls[attempt] : kModePollMicroseconds);
}

static void set_manual_modes(io_connect_t connection, const FanState *fans, int count) {
    int attempts = force_recovery.touched_test_mode ? kModeAttempts + kExtraEarlyPolls : 1;
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
        if (attempt + 1 < attempts) poll_change(attempt);
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

static void print_rpm(const FanState *fans, int count) {
    for (int i = 0; i < count; i++) {
        printf("Fan %d  %.0f RPM\n", fans[i].number + 1, fans[i].current);
    }
}

/* The current sample and the requested controller target are different values. */
static void print_change(const FanState *fans, int count, bool automatic, bool maximum,
    int rpm, bool queued) {
    if (automatic) {
        puts(queued ? "macOS auto control requested" : "macOS auto control active");
    } else if (maximum) {
        puts(queued ? "Maximum targets queued" : "Maximum targets set");
        for (int i = 0; i < count; i++) {
            printf("Fan %d  %.0f RPM now  /  %.0f RPM max\n",
                fans[i].number + 1, fans[i].current, roundf(fans[i].maximum));
        }
        return;
    } else {
        printf("Target %d RPM %s\n", rpm, queued ? "queued" : "set");
    }
    print_rpm(fans, count);
}

static void print_status(io_connect_t connection) {
    FanState fans[10];
    int count = fan_count(connection);
    if (!count) fail("No fans detected");
    for (int i = 0; i < count; i++) {
        fans[i].number = i;
        fans[i].current = fan_rpm(connection, i, "Ac");
    }
    print_rpm(fans, count);
}

/* An exact manual target is already applied, even while the motor is moving. */
static bool already_applied(const FanState *fans, int count, bool maximum, int rpm) {
    for (int i = 0; i < count; i++) {
        float target = maximum ? roundf(fans[i].maximum) : (float)rpm;
        if (fans[i].mode != 1 || fans[i].target != target ||
            target < fans[i].minimum || target > fans[i].maximum) return false;
    }
    return true;
}

static void finish_change(io_connect_t connection, FanState *fans, int count,
    bool automatic, bool maximum, int rpm) {
    if (fan_count(connection) != count) fail("Fan count changed during control");
    for (int i = 0; i < count; i++) {
        int mode = fan_mode(connection, i);
        if (automatic) {
            if (mode != 0 && mode != 3) fail("Automatic mode was lost");
        } else {
            float target = maximum ? roundf(fans[i].maximum) : (float)rpm;
            if (mode != 1 || fan_rpm(connection, i, "Tg") != target) {
                fail("macOS changed fan control before the command completed");
            }
        }
        fans[i].current = fan_rpm(connection, i, "Ac");
    }
    check_interrupted();
    force_recovery.armed = false;
    print_change(fans, count, automatic, maximum, rpm, false);
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
    const int attempts = 8 + kExtraEarlyPolls;
    for (int attempt = 0; attempt < attempts; attempt++) {
        check_interrupted();
        bool reached = true;
        for (int number = 0; number < count; number++) {
            FanState current = read_fan(connection, fans[number].number);
            if (current.mode != 1 || current.target != (float)targets[number]) {
                reached = false;
            }
        }
        if (reached) return;
        if (attempt + 1 < attempts) poll_change(attempt);
    }
    fail("Fan target did not settle at the requested RPM");
}

static void usage(void) {
    puts("Usage:\n"
         "  fanctl stat    Read current RPM\n"
         "  fanctl <RPM>   Set one target for all fans\n"
         "  fanctl max     Set each fan to its maximum\n"
         "  fanctl auto    Return control to macOS");
}

#ifndef FANCTL_USE_ADJACENT_AUTHORIZER
#define FANCTL_USE_ADJACENT_AUTHORIZER 0
#endif
#if FANCTL_USE_ADJACENT_AUTHORIZER != 0 && FANCTL_USE_ADJACENT_AUTHORIZER != 1
#error "FANCTL_USE_ADJACENT_AUTHORIZER must be 0 or 1"
#endif

#define FANCTL_SYSTEM_TOOL "/Library/PrivilegedHelperTools/fanctl"
static bool system_tool_installed(void) {
    struct stat st;
    return lstat(FANCTL_SYSTEM_TOOL, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_uid == 0 && !(st.st_mode & 0022) && (st.st_mode & 0001);
}

/* A per-account receipt opts this client into detached, passwordless changes.
   Sudo still enforces the rule for the independently installed root executable. */
static bool passwordless_enabled(void) {
    char path[PATH_MAX], resolved[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) || !realpath(path, resolved) ||
        strlcat(resolved, ".passwordless", sizeof(resolved)) >= sizeof(resolved)) return false;
    FILE *receipt = fopen(resolved, "r");
    if (!receipt) return false;
    unsigned long uid;
    bool enabled = fscanf(receipt, "%lu", &uid) == 1 && uid == getuid();
    fclose(receipt);
    return enabled && system_tool_installed();
}

#if FANCTL_USE_ADJACENT_AUTHORIZER
static bool session_marker(const char *key) {
    const char *value = getenv(key);
    return value && *value;
}
#endif

static void run_elevated(int argc, char *argv[]) {
    char executable[PATH_MAX], resolved[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0 || !realpath(executable, resolved)) {
        fail("Cannot resolve this executable for administrator access");
    }
    char *command = argc == 1 ? "stat" : argv[1];
    char numeric[16];
    if (is_rpm_text(command)) {
        snprintf(numeric, sizeof(numeric), "%d", parse_rpm(command));
        command = numeric;
    }
    char *tool = strcmp(command, "stat") && system_tool_installed() ? FANCTL_SYSTEM_TOOL : resolved;
    char *args[6] = {"/usr/bin/sudo"};
    int next = 1;
    int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty < 0) {
        args[next++] = "-n";
    } else {
        close(tty);
#if FANCTL_USE_ADJACENT_AUTHORIZER
        bool local = !session_marker("SSH_CONNECTION") && !session_marker("SSH_CLIENT") &&
            !session_marker("SSH_TTY") && !session_marker("TMUX") && !session_marker("STY");
        if (local && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            char *sep = strrchr(resolved, '/');
            char helper[PATH_MAX];
            if (!sep || snprintf(helper, sizeof(helper), "%.*s/fanctl_askpass",
                (int)(sep - resolved), resolved) >= (int)sizeof(helper)) {
                fail("Cannot locate the local password prompt");
            }
            if (access(helper, X_OK) == 0) {
                if (setenv("SUDO_ASKPASS", helper, 1) != 0) fail("Cannot select local prompt");
                args[next++] = "-A";
            }
        }
#endif
    }
    args[next++] = "--";
    args[next++] = tool;
    args[next] = command;
    execv(args[0], args);
    fail("Cannot request administrator access");
}

#include "fan_jobs.h"

int main(int argc, char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--apply-pending") && geteuid() != 0) {
        struct stat directory, lock;
        if (fstat(3,&directory) || !S_ISDIR(directory.st_mode) || directory.st_uid != getuid() ||
            fstat(4,&lock) || !S_ISREG(lock.st_mode) || lock.st_uid != getuid()) return EXIT_FAILURE;
        fcntl(3, F_SETFD, FD_CLOEXEC);
        fcntl(4, F_SETFD, FD_CLOEXEC);
        return work_jobs(3,4);
    }
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "help") ||
        !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        usage();
        return EXIT_SUCCESS;
    }
    const char *command = argv[1];
    bool stat = !strcmp(command, "stat");
    bool maximum = !strcmp(command, "max");
    bool automatic = !strcmp(command, "auto");
    bool set = is_rpm_text(command);
    if (argc > 2 || !(stat || maximum || automatic || set)) {
        usage();
        return EXIT_FAILURE;
    }
    int rpm = set ? parse_rpm(command) : 0;
    bool user = geteuid() != 0;
    bool managed = user && passwordless_enabled();
    if (user && automatic && !managed) run_elevated(argc, argv);
    if (user) {
        stat_argc = argc;
        stat_argv = argv;
    }
    io_connect_t connection = open_smc();
    if (stat) {
        print_status(connection);
        IOServiceClose(connection);
        return managed ? job_result() : EXIT_SUCCESS;
    } else if (managed) {
        submit_job(connection, command, maximum, automatic, rpm);
    } else {
        FanState fans[10];
        int count = all_fans(connection, fans);
        if (!automatic && already_applied(fans, count, maximum, rpm)) {
            print_change(fans, count, false, maximum, rpm, false);
        } else {
            if (user) {
                IOServiceClose(connection);
                run_elevated(argc, argv);
            }
            struct sigaction action = {0};
            action.sa_handler = handle_signal;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
                fail("Cannot install cancellation handler");
            }
            if (automatic) restore_auto(connection);
            else force_fans(connection, fans, count, set, rpm);
            finish_change(connection, fans, count, automatic, maximum, rpm);
        }
    }
    IOServiceClose(connection);
    return EXIT_SUCCESS;
}
