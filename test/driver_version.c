#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/driver_version.h"
int main(void) {
    const struct { const char *line, *want; } cases[] = {
        {"NVRM version: NVIDIA UNIX x86_64 Kernel Module  550.163.01  Release Build", "550.163.01"},
        {"NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  595.91.07  Release Build", "595.91.07"},
        {"NVRM version: NVIDIA UNIX Open Kernel Module for aarch64  595.91.07  Release Build", "595.91.07"},
        {"NVRM version: NVIDIA UNIX Open Kernel Module  590.48.01  Release Build", "590.48.01"},
        {"NVRM version: NVIDIA UNIX Kernel Module\t595.91\r\n", "595.91"},
        {"NVRM version: NVIDIA UNIX Kernel Module for x86_64", ""},
        {"NVRM version: NVIDIA UNIX Kernel Module 595..91 Release Build", ""},
        {"NVRM version: NVIDIA UNIX Kernel Module 595.91.beta Release Build", ""},
        {"NVRM version: NVIDIA UNIX Kernel Module 595.91. Release Build", ""},
        {"NVRM version: NVIDIA UNIX Kernel Module .595.91 Release Build", ""},
        {"NVRM version: NVIDIA UNIX Kernel Module 595.91.1.2 Release Build", ""},
        {"GCC version: gcc version 15.2.0", ""},
        {"", ""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char out[64] = "previous";
        kernel_version(cases[i].line, out, sizeof out);
        assert(strcmp(out, cases[i].want) == 0);
    }
    char short_out[4] = "old";
    kernel_version(cases[1].line, short_out, sizeof short_out);
    assert(short_out[0] == 0);
    kernel_version(cases[1].line, NULL, 0);
    puts("driver version: 15 cases passed");
}
