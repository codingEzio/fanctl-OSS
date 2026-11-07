// SPDX-License-Identifier: GPL-2.0-only
// Apple SMC access is based on mac-fans-cli and smcFanControl.
// Original SMC portions: Copyright (C) 2006 devnull; (C) 2013 Michael Wilber.
// Credits also retain Hendrik Holtmann and park-jemin; see THIRD_PARTY_NOTICES.md.
// Modified by fanctl contributors on 2026-11-07: preflight, diagnostic and
// best-effort automatic-mode recovery when a forced fan change fails.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    kTargetTolerance = 25,
};

typedef struct {
    bool armed;
    bool attempted;
    bool touched_force_bits;
    bool touched_test_mode;
    io_connect_t connection;
    int count;
} ForceRecovery;
static ForceRecovery force_recovery;

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
    exit(EXIT_FAILURE);
}

static void fail_code(const char *action, kern_return_t code) {
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

static void write_key(io_connect_t connection, const SMCValue *value) {
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
    }
    kern_return_t result = smc_call(connection, &input, &output);
    if (result != KERN_SUCCESS) {
        fail_code("Cannot write SMC key", result);
    }
    if (output.result != kSMCSuccess) {
        fail_smc_result("Cannot write SMC key", output.result);
    }
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

static bool recovery_write_key(io_connect_t connection, const SMCValue *value) {
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
    memset(value.bytes, 0, entry->zero_bytes);
    return recovery_write_key(connection, &value);
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
    for (size_t index = 0; zero && index < entry->zero_bytes; index++) {
        if (readback.bytes[index] != 0) zero = false;
    }
    if (zero) return kVerifyZero;
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
        keys[total++].required = true;
    }
    memcpy(keys[total].key, "Ftst", 4);
    keys[total].zero_bytes = 1;
    keys[total].optional = true;
    keys[total++].required = require_test_mode;
    bool success = true;
    /* Attempt every reset before verification: some controllers apply a mode
       only after all control keys have been cleared. A failed write remains a
       failure even when the final read happens to show zero. */
    for (size_t index = 0; index < total; index++) {
        if (!recovery_write_zero(connection, &keys[index])) success = false;
    }
    bool verified = false;
    /* Reuse the target-settling window; only a valid nonzero value is retried.
       Missing/invalid reads or failed writes return promptly for fallback. */
    int attempts = success ? 8 : 1;
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
        usleep(250000);
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

static FanState read_fan(io_connect_t connection, int number) {
    FanState fan = {.number = number};
    SMCValue value = read_fan_key(connection, number, "Ac");
    fan.current = numeric_value(&value);
    value = read_fan_key(connection, number, "Mn");
    fan.minimum = numeric_value(&value);
    value = read_fan_key(connection, number, "Mx");
    fan.maximum = numeric_value(&value);
    value = read_fan_key(connection, number, "Tg");
    fan.target = numeric_value(&value);
    value = read_fan_key(connection, number, "Md");
    float mode = numeric_value(&value);
    if (!isfinite(mode) || mode < 0 || (double)mode > (double)INT32_MAX ||
        floorf(mode) != mode) fail("Invalid fan mode");
    if (!isfinite(fan.current) || !isfinite(fan.target) || !isfinite(fan.minimum) ||
        !isfinite(fan.maximum) || fan.current < 0 || fan.target < 0 || fan.minimum < 0 ||
        fan.minimum > fan.maximum) fail("Invalid fan RPM data");
    fan.mode = (int)mode;
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
        value.bytes[0] = byte;
        write_key(connection, &value);
    } else if (expected) fail("Previously detected optional byte key disappeared");
}

static void set_optional_force_bits(io_connect_t connection, uint16_t bits, bool expected) {
    SMCValue value;
    if (try_read_key(connection, "FS! ", &value)) {
        if (!has_format(&value, "ui16", 2)) fail("Unsupported global force key format");
        value.bytes[0] = (uint8_t)(bits >> 8);
        value.bytes[1] = (uint8_t)bits;
        write_key(connection, &value);
    } else if (expected) fail("Previously detected global force key disappeared");
}

static void set_mode(io_connect_t connection, int number, int mode) {
    SMCValue value = read_fan_key(connection, number, "Md");
    if (!has_format(&value, "ui8 ", 1)) {
        fail("Unsupported fan mode key format");
    }
    value.bytes[0] = (uint8_t)mode;
    write_key(connection, &value);
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

static void set_fan(io_connect_t connection, const FanState *fan, int rpm,
    const SMCValue *target) {
    if ((float)rpm < fan->minimum || (float)rpm > fan->maximum) {
        char message[160];
        snprintf(message, sizeof(message), "Fan %d RPM must be between %.0f and %.0f",
            fan->number, fan->minimum, fan->maximum);
        fail(message);
    }

    set_mode(connection, fan->number, 1);
    write_key(connection, target);
    set_mode(connection, fan->number, 1);

    for (int attempt = 0; attempt < 8; attempt++) {
        usleep(250000);
        SMCValue readback = read_fan_key(connection, fan->number, "Tg");
        if (fabsf(numeric_value(&readback) - (float)rpm) <= kTargetTolerance) {
            return;
        }
    }
    fail("Fan target did not settle at the requested RPM");
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

static void print_status(io_connect_t connection) {
    FanState fans[10];
    int count = all_fans(connection, fans);
    printf("Fans: %d\n", count);
    for (int number = 0; number < count; number++) {
        const FanState *fan = &fans[number];
        printf("Fan %d: %.0f RPM current, %.0f target, %.0f-%.0f allowed, %s\n",
            fan->number, fan->current, fan->target, fan->minimum, fan->maximum,
            fan->mode == 0 ? "auto" : "forced");
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
    force_recovery = (ForceRecovery){.armed = true, .connection = connection,
        .count = count, .touched_force_bits = saw_force_bits,
        .touched_test_mode = saw_test_mode};
    set_optional_byte(connection, "Ftst", 1, saw_test_mode);
    set_optional_force_bits(connection, (uint16_t)((1U << count) - 1), saw_force_bits);
    for (int number = 0; number < count; number++) {
        set_fan(connection, &fans[number], targets[number], &encoded[number]);
    }
    print_status(connection);
    force_recovery.armed = false;
}

static void usage(void) {
    puts("fanctl — macOS fan inspection and temporary control\n"
         "\n"
         "Usage:\n"
         "  fanctl max\n"
         "  fanctl auto\n"
         "  fanctl <RPM>\n"
         "  fanctl stat");
}

// Standalone builds use system sudo. The managed owner build explicitly keeps
// its adjacent native authorizer; no private home path is compiled by default.
#ifndef FANCTL_USE_ADJACENT_AUTHORIZER
#define FANCTL_USE_ADJACENT_AUTHORIZER 0
#endif
#if FANCTL_USE_ADJACENT_AUTHORIZER != 0 && FANCTL_USE_ADJACENT_AUTHORIZER != 1
#error "FANCTL_USE_ADJACENT_AUTHORIZER must be 0 or 1"
#endif

static void run_elevated(int argc, char *argv[]) {
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
#else
    const char *authorizer = "/usr/bin/sudo";
#endif

    char *authorize_args[argc + 2];
    authorize_args[0] = (char *)authorizer;
    authorize_args[1] = resolved;
    for (int index = 1; index < argc; index++) {
        authorize_args[index + 1] = argv[index];
    }
    authorize_args[argc + 1] = NULL;
    execv(authorizer, authorize_args);
    fail("Cannot request administrator access");
}

int main(int argc, char *argv[]) {
    if (argc == 1 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        usage();
        return EXIT_SUCCESS;
    }

    if (argc != 2) {
        usage();
        return EXIT_FAILURE;
    }

    const char *command = argv[1];
    bool want_stat = strcmp(command, "stat") == 0;
    bool want_max = strcmp(command, "max") == 0;
    bool want_auto = strcmp(command, "auto") == 0;
    bool want_set = is_rpm_text(command);
    if (!(want_stat || want_max || want_auto || want_set)) {
        usage();
        return EXIT_FAILURE;
    }

    int rpm = want_set ? parse_rpm(command) : 0;
    if (geteuid() != 0) {
        run_elevated(argc, argv);
    }

    io_connect_t connection = open_smc();
    if (want_stat) {
        print_status(connection);
    } else if (want_set || want_max) {
        FanState fans[10];
        int count = all_fans(connection, fans);
        force_fans(connection, fans, count, want_set, rpm);
    } else if (want_auto) {
        restore_auto(connection);
        print_status(connection);
    }

    IOServiceClose(connection);
    return EXIT_SUCCESS;
}
