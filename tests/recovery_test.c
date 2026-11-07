/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 fanctl contributors.
 * Synthetic controller state: real IOKit access is forbidden.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jmp_buf stopped;
static int exit_status;
static const char *scenario;
static int writes;
static int mode0_metadata_reads;
static int force_metadata_reads;
static int sleeps;
static bool injected;
static bool pending_mode0;
static bool pending_mode1;
static bool verification_phase;
static bool delay_first_mode_read;
static _Noreturn void fake_exit(int status) { exit_status = status; longjmp(stopped, 1); }
static int fake_sleep(useconds_t duration) { (void)duration; sleeps++; return 0; }
static kern_return_t fake_smc(io_connect_t, uint32_t, const void *, size_t, void *, size_t *);
static kern_return_t forbidden_service(mach_port_t port, CFDictionaryRef matching,
    io_iterator_t *iterator) {
    (void)port; (void)matching; (void)iterator;
    fputs("Real IOKit access is forbidden in recovery tests\n", stderr);
    _Exit(80);
}

#define main fanctl_original_main
#define exit fake_exit
#define usleep fake_sleep
#define IOConnectCallStructMethod fake_smc
#define IOServiceGetMatchingServices forbidden_service
#ifndef FANCTL_SOURCE_INCLUDE
#define FANCTL_SOURCE_INCLUDE "../fanctl.c"
#endif
#include FANCTL_SOURCE_INCLUDE
#undef IOServiceGetMatchingServices
#undef IOConnectCallStructMethod
#undef usleep
#undef exit
#undef main

typedef struct {
    const char *name;
    const char *type;
    uint32_t size;
    uint8_t bytes[4];
} MockKey;

enum { kFanCount, kTestMode, kForceBits, kMode0, kTarget0, kActual0, kMin0, kMax0,
    kMode1, kTarget1, kActual1, kMin1, kMax1 };
static MockKey keys[] = {
    {"FNum", "ui8 ", 1, {2, 0}}, {"Ftst", "ui8 ", 1, {0, 0}},
    {"FS! ", "ui16", 2, {0, 0}}, {"F0Md", "ui8 ", 1, {0, 0}},
    {"F0Tg", "fpe2", 2, {0x1f, 0x40}}, {"F0Ac", "fpe2", 2, {0x1f, 0x40}},
    {"F0Mn", "fpe2", 2, {0x0f, 0xa0}}, {"F0Mx", "fpe2", 2, {0x5d, 0xc0}},
    {"F1Md", "ui8 ", 1, {0, 0}}, {"F1Tg", "fpe2", 2, {0x1f, 0x40}},
    {"F1Ac", "fpe2", 2, {0x1f, 0x40}}, {"F1Mn", "fpe2", 2, {0x0f, 0xa0}},
    {"F1Mx", "fpe2", 2, {0x5d, 0xc0}},
};

static bool is_case(const char *name) { return strcmp(scenario, name) == 0; }

static kern_return_t fake_smc(io_connect_t connection, uint32_t selector,
    const void *raw_input, size_t input_size, void *raw_output, size_t *output_size) {
    (void)connection; (void)selector; (void)input_size;
    const SMCKeyData *input = raw_input;
    SMCKeyData *output = raw_output;
    memset(output, 0, sizeof(*output));
    MockKey *key = NULL;
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
        if (input->key == key_code(keys[index].name)) key = &keys[index];
    }
    if (!key) { output->result = kSMCKeyNotFound; return KERN_SUCCESS; }
    if (input->data8 == kSMCCommandReadKeyInfo) {
        if (strcmp(key->name, "F0Md") == 0) mode0_metadata_reads++;
        if (strcmp(key->name, "FS! ") == 0) force_metadata_reads++;
        if (is_case("verification-key-vanishes") && strcmp(key->name, "FS! ") == 0 &&
            (force_metadata_reads == 1 || force_metadata_reads >= 3)) {
            output->result = kSMCKeyNotFound; return KERN_SUCCESS;
        }
        if (is_case("optional-vanishes-before-write") &&
            strcmp(key->name, "FS! ") == 0 && force_metadata_reads > 1) {
            output->result = kSMCKeyNotFound; return KERN_SUCCESS;
        }
        if (is_case("short-preflight") && strcmp(key->name, "F1Tg") == 0) {
            *output_size = 0; return KERN_SUCCESS;
        }
        if ((is_case("auto-optional-missing") || is_case("force-optional-missing") ||
            is_case("missing-optional-after-write")) &&
            (strcmp(key->name, "FS! ") == 0 || strcmp(key->name, "Ftst") == 0)) {
            output->result = kSMCKeyNotFound; return KERN_SUCCESS;
        }
        if (is_case("preflight-optional-error") && strcmp(key->name, "FS! ") == 0) {
            output->result = 1; return KERN_SUCCESS;
        }
        if ((is_case("preflight-error") || is_case("preexisting-force")) &&
            strcmp(key->name, "F1Tg") == 0) { output->result = 1; return KERN_SUCCESS; }
        if (is_case("status-error") && writes && strcmp(key->name, "F1Ac") == 0) {
            output->result = 1; return KERN_SUCCESS;
        }
        if (is_case("optional-disappears") && injected && strcmp(key->name, "FS! ") == 0) {
            output->result = kSMCKeyNotFound; return KERN_SUCCESS;
        }
        output->key_info.data_size = key->size;
        output->key_info.data_type = key_code(is_case("type-changed") &&
            strcmp(key->name, "F0Md") == 0 && mode0_metadata_reads == 3 ? "flag" : key->type);
    } else if (input->data8 == kSMCCommandReadBytes) {
        memcpy(output->bytes, key->bytes, key->size);
        if (verification_phase && strcmp(key->name, "F0Md") == 0 &&
            (is_case("persistent-nonzero") || (is_case("delayed-auto") && delay_first_mode_read))) {
            output->bytes[0] = 1;
            delay_first_mode_read = false;
        }
    } else if (input->data8 == kSMCCommandWriteBytes) {
        writes++;
        if ((is_case("partial-recovery") || is_case("auto-partial")) &&
            strcmp(key->name, "F0Md") == 0 && input->bytes[0] == 0) return KERN_FAILURE;
        if (is_case("recovery-smc-error") && strcmp(key->name, "F0Md") == 0 &&
            input->bytes[0] == 0) { output->result = 1; return KERN_SUCCESS; }
        if (is_case("prewrite-handshake") && strcmp(key->name, "F0Md") == 0 &&
            input->bytes[0] == 0 && mode0_metadata_reads < 2) return KERN_FAILURE;
        /* Synthetic controller: fan modes settle only after test mode clears. */
        if (is_case("deferred-auto") && input->bytes[0] == 0 && keys[kTestMode].bytes[0] == 1 &&
            (strcmp(key->name, "F0Md") == 0 || strcmp(key->name, "F1Md") == 0)) {
            if (strcmp(key->name, "F0Md") == 0) pending_mode0 = true;
            else pending_mode1 = true;
            return KERN_SUCCESS;
        }
        memcpy(key->bytes, input->bytes, key->size);
        if ((is_case("delayed-auto") || is_case("persistent-nonzero")) &&
            strcmp(key->name, "Ftst") == 0 && input->bytes[0] == 0) {
            verification_phase = true;
            delay_first_mode_read = true;
        }
        if (is_case("deferred-auto") && strcmp(key->name, "Ftst") == 0 && input->bytes[0] == 0) {
            if (pending_mode0) keys[kMode0].bytes[0] = 0;
            if (pending_mode1) keys[kMode1].bytes[0] = 0;
        }
        if (!injected && is_case("short-after-write") && strcmp(key->name, "F1Tg") == 0) {
            injected = true; *output_size = 0; return KERN_SUCCESS;
        }
        if ((is_case("short-recovery-write") || is_case("short-auto")) &&
            strcmp(key->name, "F0Md") == 0 && input->bytes[0] == 0) {
            *output_size = 0; return KERN_SUCCESS;
        }
        if (!injected && strcmp(key->name, "F1Tg") == 0 &&
            (is_case("transport-after-write") || is_case("partial-recovery") ||
                is_case("missing-optional-after-write") || is_case("short-recovery-write"))) {
            injected = true; return KERN_FAILURE;
        }
        if (!injected && is_case("optional-disappears") && strcmp(key->name, "FS! ") == 0) {
            injected = true; return KERN_FAILURE;
        }
    } else {
        _Exit(81);
    }
    return KERN_SUCCESS;
}

static bool state(unsigned int test, unsigned int force, unsigned int mode0, unsigned int mode1) {
    return keys[kTestMode].bytes[0] == test && keys[kForceBits].bytes[1] == force &&
        keys[kMode0].bytes[0] == mode0 && keys[kMode1].bytes[0] == mode1;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    scenario = argv[1];
    FanState fans[2] = {
        {.number = 0, .minimum = 1000, .maximum = 6000},
        {.number = 1, .minimum = 1000, .maximum = 6000},
    };
    if (is_case("rpm-overflow")) fans[0].maximum = fans[1].maximum = 20000;
    if (is_case("invalid-range")) fans[1].maximum = NAN;
    if (is_case("max-success")) {
        fans[1].maximum = 5500;
        keys[kMax1].bytes[0] = 0x55; keys[kMax1].bytes[1] = 0xf0;
    }
    if (is_case("preexisting-force") || is_case("auto-partial") || is_case("auto-success") ||
        is_case("deferred-auto") || is_case("prewrite-handshake") || is_case("recovery-smc-error") ||
        is_case("delayed-auto") || is_case("persistent-nonzero") || is_case("verification-key-vanishes")) {
        keys[kTestMode].bytes[0] = 1; keys[kForceBits].bytes[1] = 3;
        keys[kMode0].bytes[0] = 1; keys[kMode1].bytes[0] = 1;
    }
    if (is_case("auto-optional-missing")) {
        keys[kMode0].bytes[0] = 1; keys[kMode1].bytes[0] = 1;
    }
    if (is_case("invalid-fan-count") || is_case("fractional-fan-count")) {
        float count = is_case("invalid-fan-count") ? NAN : 1.5f;
        keys[kFanCount].type = "flt "; keys[kFanCount].size = sizeof(count);
        memcpy(keys[kFanCount].bytes, &count, sizeof(count));
    }
    if (is_case("large-fan-count")) keys[kFanCount].bytes[0] = 11;
    if (is_case("preflight-fs-format") || is_case("auto-fs-format")) {
        keys[kForceBits].type = "ui32"; keys[kForceBits].size = 4;
    }
    if (is_case("preflight-fs-size")) keys[kForceBits].size = 0;
    if (is_case("optional-vanishes-before-write")) keys[kForceBits].bytes[1] = 3;
    if (is_case("preflight-ftst-format")) keys[kTestMode].type = "flag";
    if (is_case("preflight-mode-format")) keys[kMode0].type = "flag";
    if (is_case("invalid-mode") || is_case("invalid-actual")) {
        MockKey *key = is_case("invalid-mode") ? &keys[kMode0] : &keys[kActual0];
        float value = NAN; key->type = "flt "; key->size = sizeof(value);
        memcpy(key->bytes, &value, sizeof(value));
    }
    if (is_case("auto-fs-format")) {
        keys[kForceBits].bytes[3] = 3; keys[kTestMode].bytes[0] = 1;
        keys[kMode0].bytes[0] = 1; keys[kMode1].bytes[0] = 1;
    }
    if (is_case("short-auto")) {
        keys[kTestMode].bytes[0] = 1; keys[kForceBits].bytes[1] = 3;
        keys[kMode0].bytes[0] = 1; keys[kMode1].bytes[0] = 1;
    }
    if (setjmp(stopped) == 0) {
        if (is_case("auto-partial") || is_case("auto-success") ||
            is_case("deferred-auto") || is_case("prewrite-handshake") || is_case("recovery-smc-error") ||
            is_case("delayed-auto") || is_case("persistent-nonzero") || is_case("verification-key-vanishes") ||
            is_case("auto-optional-missing") || is_case("invalid-fan-count") ||
            is_case("fractional-fan-count") || is_case("large-fan-count") ||
            is_case("auto-fs-format") || is_case("short-auto")) restore_auto(0);
        else if (is_case("invalid-mode") || is_case("invalid-actual")) (void)read_fan(0, 0);
        else force_fans(0, fans, 2, !is_case("max-success"),
            is_case("rpm-overflow") ? 17000 : 2500);
    }
    bool pass = false;
    if (is_case("success")) {
        pass = exit_status == 0 && state(1, 3, 1, 1) &&
            keys[kTarget0].bytes[0] == 0x27 && keys[kTarget1].bytes[0] == 0x27;
    } else if (is_case("max-success")) {
        pass = exit_status == 0 && state(1, 3, 1, 1) &&
            keys[kTarget0].bytes[0] == 0x5d && keys[kTarget1].bytes[0] == 0x55;
    } else if (is_case("auto-success") || is_case("auto-optional-missing") ||
        is_case("deferred-auto") || is_case("prewrite-handshake")) {
        pass = exit_status == 0 && state(0, 0, 0, 0);
    } else if (is_case("delayed-auto")) {
        pass = exit_status == 0 && state(0, 0, 0, 0) && writes == 4 && sleeps == 1;
    } else if (is_case("persistent-nonzero")) {
        pass = exit_status == EXIT_FAILURE && state(0, 0, 0, 0) && writes == 4 && sleeps == 7;
    } else if (is_case("verification-key-vanishes")) {
        pass = exit_status == EXIT_FAILURE && state(0, 3, 0, 0) && writes == 3 && sleeps == 1;
    } else if (is_case("force-optional-missing")) {
        pass = exit_status == 0 && state(0, 0, 1, 1);
    } else if (is_case("preflight-error") || is_case("preflight-optional-error") ||
        is_case("rpm-overflow") || is_case("invalid-range") ||
        is_case("invalid-fan-count") || is_case("fractional-fan-count") ||
        is_case("large-fan-count") || is_case("invalid-actual") ||
        is_case("short-preflight") || is_case("preflight-fs-format") ||
        is_case("preflight-ftst-format") || is_case("preflight-mode-format") ||
        is_case("preflight-fs-size")) {
        pass = exit_status == EXIT_FAILURE && writes == 0 && state(0, 0, 0, 0);
    } else if (is_case("invalid-mode")) {
        pass = exit_status == EXIT_FAILURE && writes == 0 && keys[kTestMode].bytes[0] == 0 &&
            keys[kForceBits].bytes[1] == 0 && keys[kMode1].bytes[0] == 0;
    } else if (is_case("preexisting-force")) {
        pass = exit_status == EXIT_FAILURE && writes == 0 && state(1, 3, 1, 1);
    } else if (is_case("transport-after-write") || is_case("status-error") ||
        is_case("missing-optional-after-write") || is_case("short-after-write") ||
        is_case("short-recovery-write") || is_case("short-auto") || is_case("type-changed")) {
        pass = exit_status == EXIT_FAILURE && state(0, 0, 0, 0);
    } else if (is_case("partial-recovery") || is_case("auto-partial") || is_case("recovery-smc-error")) {
        pass = exit_status == EXIT_FAILURE && state(0, 0, 1, 0);
    } else if (is_case("optional-disappears") || is_case("optional-vanishes-before-write")) {
        pass = exit_status == EXIT_FAILURE && state(0, 3, 0, 0);
    } else if (is_case("auto-fs-format")) {
        pass = exit_status == EXIT_FAILURE && keys[kForceBits].bytes[3] == 3 &&
            keys[kTestMode].bytes[0] == 0 && keys[kMode0].bytes[0] == 0 && keys[kMode1].bytes[0] == 0;
    }
    if (!pass) {
        fprintf(stderr, "unexpected synthetic state for %s: exit=%d writes=%d test=%u force=%u mode=%u,%u\n",
            scenario, exit_status, writes, keys[kTestMode].bytes[0], keys[kForceBits].bytes[1],
            keys[kMode0].bytes[0], keys[kMode1].bytes[0]);
        return 1;
    }
    printf("%s: passed\n", scenario);
    return 0;
}
