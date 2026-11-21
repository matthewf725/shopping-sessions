#define _GNU_SOURCE
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

struct line {
    struct line *next;
    char padding[64 - sizeof(void *)];
};

static uint32_t random_word(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return *state = value;
}

static struct line *make_ring(unsigned lines) {
    struct line *ring;
    unsigned *order = malloc(lines * sizeof(*order));
    if (!order || posix_memalign((void **)&ring, 4096, lines * sizeof(*ring))) {
        fputs("Cannot allocate pointer chain\n", stderr);
        exit(1);
    }
    for (unsigned i = 0; i < lines; ++i)
        order[i] = i;
    uint32_t state = 74353;
    for (unsigned i = lines - 1; i > 0; --i) {
        unsigned j = random_word(&state) % (i + 1);
        unsigned tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (unsigned i = 0; i < lines; ++i)
        ring[order[i]].next = &ring[order[(i + 1) % lines]];
    free(order);
    return ring;
}

static uint64_t timestamp(void) {
    unsigned low, high;
    __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}

static int64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value)) {
        perror("clock_gettime");
        exit(1);
    }
    return (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

/* Both working sets call this exact instruction sequence. Each load supplies
   the address of the next load. The compiler cannot replace the traversal. */
__attribute__((noinline)) static void chase(struct line *current, unsigned rounds) {
    __asm__ volatile ("1:\n.rept 8\nmovq (%0), %0\n.endr\ndecl %1\njnz 1b"
                      : "+r"(current), "+r"(rounds) : : "cc", "memory");
}

int main(void) {
    cpu_set_t allowed, selected;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
        perror("sched_getaffinity");
        return 1;
    }
    int cpu = 0;
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed))
        ++cpu;
    if (cpu == CPU_SETSIZE)
        return 1;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    if (sched_setaffinity(0, sizeof(selected), &selected)) {
        perror("sched_setaffinity");
        return 1;
    }

    const unsigned sizes[] = {64, 1u << 20};
    struct line *rings[] = {make_ring(sizes[0]), make_ring(sizes[1])};
    for (unsigned i = 0; i < 2; ++i)
        chase(rings[i], sizes[i] / 8);
    monotonic_ns();

    const unsigned loads = 1u << 20;
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        for (unsigned turn = 0; turn < 2; ++turn) {
            unsigned k = (repeat & 1) ^ turn;
            struct rusage before, after;
            if (getrusage(RUSAGE_SELF, &before)) return 1;
            int64_t start_ns = monotonic_ns();
            uint64_t start_ticks = timestamp();
            chase(rings[k], loads / 8);
            uint64_t elapsed_ticks = timestamp() - start_ticks;
            int64_t elapsed_ns = monotonic_ns() - start_ns;
            if (getrusage(RUSAGE_SELF, &after)) return 1;
            printf("SHOP_MEMORY_CONTROL bytes=%zu loads=%u repeat=%u tsc=%" PRIu64
                   " ns=%" PRId64 " minflt=%ld majflt=%ld\n",
                   sizes[k] * sizeof(*rings[k]), loads, repeat, elapsed_ticks, elapsed_ns,
                   after.ru_minflt - before.ru_minflt, after.ru_majflt - before.ru_majflt);
        }
    }
    free(rings[0]);
    free(rings[1]);
    return 0;
}
