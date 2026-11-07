/* Synthetic driver responses only: no IOKit call is made to hardware.
 * Modified by fanctl contributors on 2026-11-07: include the expected key format.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static kern_return_t fake_smc(io_connect_t connection, uint32_t selector,
    const void *input, size_t input_size, void *output, size_t *output_size);
#define main fanctl_command_main
#define IOConnectCallStructMethod fake_smc
#ifndef FANCTL_SOURCE_INCLUDE
#define FANCTL_SOURCE_INCLUDE "../fanctl.c"
#endif
#include FANCTL_SOURCE_INCLUDE
#undef IOConnectCallStructMethod
#undef main

static const char *scenario;
static kern_return_t fake_smc(io_connect_t connection, uint32_t selector,
    const void *raw_input, size_t input_size, void *raw_output, size_t *output_size) {
    (void)connection; (void)selector; (void)input_size; (void)output_size;
    const SMCKeyData *input = raw_input;
    SMCKeyData *output = raw_output;
    memset(output, 0, sizeof(*output));
    if (strcmp(scenario, "transport-error") == 0 ||
        (strcmp(scenario, "optional-metadata-transport-error") == 0 && input->data8 == kSMCCommandReadKeyInfo) ||
        (strcmp(scenario, "optional-read-transport-error") == 0 && input->data8 == kSMCCommandReadBytes)) return KERN_FAILURE;
    if (input->data8 == kSMCCommandReadKeyInfo) {
        output->key_info.data_size = 2;
        output->key_info.data_type = key_code("fpe2");
        if (strcmp(scenario, "metadata-error") == 0 ||
            strcmp(scenario, "optional-metadata-error") == 0) output->result = 1;
        if (strcmp(scenario, "optional-missing") == 0) output->result = 132;
    } else if (input->data8 == kSMCCommandReadBytes) {
        output->bytes[0] = 0x10;
        if (strcmp(scenario, "read-error") == 0 ||
            strcmp(scenario, "optional-read-error") == 0) output->result = 1;
        if (strcmp(scenario, "optional-read-missing") == 0) output->result = 132;
    } else if (input->data8 == kSMCCommandWriteBytes) {
        if (strcmp(scenario, "write-error") == 0) output->result = 1;
    } else {
        abort();
    }
    return KERN_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    scenario = argv[1];
    if (strcmp(scenario, "optional-metadata-transport-error") == 0 ||
        strcmp(scenario, "optional-read-transport-error") == 0) {
        set_optional_force_bits(0, 0, false);
        puts("continued-after-optional-write");
        return 0;
    }
    if (strncmp(scenario, "optional-", 9) == 0) {
        SMCValue value = {0};
        printf("optional-present=%d\n", try_read_key(0, "F0Tg", &value));
        return 0;
    }
    if (strcmp(scenario, "metadata-error") == 0 || strcmp(scenario, "read-error") == 0 ||
        strcmp(scenario, "transport-error") == 0) {
        SMCValue value = read_key(0, "F0Tg");
        printf("read-returned=%zu\n", value.size);
        return 0;
    }
    if (strcmp(scenario, "write-error") == 0 || strcmp(scenario, "success") == 0) {
        SMCValue value = {.key = "F0Tg", .data_type = "fpe2", .size = 2, .bytes = {0x10, 0}};
        write_key(0, &value);
        puts("write-returned");
        return 0;
    }
    return 2;
}
