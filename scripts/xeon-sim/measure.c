#include <cpuid.h>
#include <errno.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run inside the Bochs guest. The port is handled by counters.patch. */
int main(int argc, char **argv) {
    unsigned a, b, c, d, xcr0, high;
    const unsigned required = bit_AVX512F | bit_AVX512BW | bit_AVX512DQ |
                              bit_AVX512VL | bit_BMI2;
    if (argc < 2 || !__get_cpuid(1, &a, &b, &c, &d) || !(c & bit_OSXSAVE) ||
        !(c & bit_PCLMUL) || !__get_cpuid_count(7, 0, &a, &b, &c, &d) ||
        (b & required) != required || sysconf(_SC_NPROCESSORS_ONLN) != 8) {
        fputs("Missing required guest CPU features or vCPUs\n", stderr);
        return 1;
    }
    __asm__ volatile ("xgetbv" : "=a"(xcr0), "=d"(high) : "c"(0));
    if ((xcr0 & 0xe6) != 0xe6 || ioperm(0xe9, 4, 1)) {
        fputs("Cannot use guest ZMM state or simulation counter port\n", stderr);
        return 1;
    }
    outl(0x53484f31, 0xe9);
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        if (ioperm(0xe9, 4, 0) || setgroups(0, NULL) || setgid(1000) || setuid(1000))
            _exit(126);
        execv(argv[1], argv + 1);
        perror(argv[1]);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return 1; }
    }
    outl(0x53484f32, 0xe9);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
