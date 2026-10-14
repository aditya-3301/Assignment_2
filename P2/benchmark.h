#ifndef BENCHMARK_H
#define BENCHMARK_H
/* Shared harness, compiled independently into programs A and B.
 * Viva: start-reference includes fork creation and first-dispatch latency.
 * There is deliberately NO start barrier, since the assignment specifies the
 * first action after fork as the child's start timestamp. The parent retains
 * its inherited RT policy too: fork order can influence FIFO response times.
 */
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CHILDREN = 8 };
struct child_report {
    int index;
    pid_t pid;
    struct timespec start, finish;
};
_Static_assert(sizeof(struct child_report) <= PIPE_BUF,
               "Each child report must fit in one atomic pipe write");

static double seconds_between(struct timespec end, struct timespec start)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static int read_report(int fd, struct child_report *report)
{
    size_t got = 0;
    while (got < sizeof(*report)) {
        ssize_t n = read(fd, (char *)report + got, sizeof(*report) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) return got == 0 ? 0 : -1;
        got += (size_t)n;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int smoke = argc == 2 && strcmp(argv[1], "--smoke") == 0;
    int fds[2], policy = sched_getscheduler(0), created = 0, bad = 0;
    struct sched_param sp;
    cpu_set_t mask;
    struct timespec reference, end, quantum = {0};
    struct child_report reports[CHILDREN];
    int seen[CHILDREN] = {0};
    pid_t children[CHILDREN];
    struct rusage usage;
    if (argc > 2 || (argc == 2 && !smoke)) {
        fprintf(stderr, "Usage: %s [--smoke]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (policy < 0 || sched_getparam(0, &sp) || sched_getaffinity(0, sizeof(mask), &mask)) {
        perror("scheduler query"); return EXIT_FAILURE;
    }
    if (!smoke && ((policy != SCHED_RR && policy != SCHED_FIFO) || CPU_COUNT(&mask) != 1)) {
        fprintf(stderr, "Measurement requires SCHED_RR/FIFO and exactly one allowed CPU.\n"
                "Use: sudo taskset -c 0 chrt -r 20 %s\n"
                "Or use --smoke for a NON-SUBMISSION functional check.\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (policy == SCHED_RR && sched_rr_get_interval(0, &quantum)) {
        perror("sched_rr_get_interval"); return EXIT_FAILURE;
    }
    printf("%s\nScheduling Policy: %s\nPriority: %d\nAllowed CPUs:", WORKLOAD_NAME,
           policy == SCHED_RR ? "SCHED_RR" : policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER",
           sp.sched_priority);
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &mask)) printf(" %d", i);
    putchar('\n');
    if (smoke) puts("SMOKE TEST ONLY: not a required RR/FIFO measurement.");
    if (policy == SCHED_RR) {
        int requested;
        FILE *fp = fopen("/proc/sys/kernel/sched_rr_timeslice_ms", "r");
        if (fp) {
            if (fscanf(fp, "%d", &requested) == 1)
                printf("Requested Time Quantum: %d ms\n", requested);
            fclose(fp);
        }
        printf("Effective RR Interval: %.3f ms\n", quantum.tv_sec * 1000.0 + quantum.tv_nsec / 1e6);
    }
    fflush(stdout); /* Avoid duplicating a parent's buffered output after fork. */
    if (pipe(fds)) { perror("pipe"); return EXIT_FAILURE; }
    if (clock_gettime(CLOCK_MONOTONIC, &reference)) { perror("clock_gettime"); return EXIT_FAILURE; }
    for (int i = 0; i < CHILDREN; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            struct timespec start, finish;
            /* First child operation: capture when the child actually begins. */
            if (clock_gettime(CLOCK_MONOTONIC, &start)) _exit(125);
            close(fds[0]);
            RUN_WORKLOAD((int)getpid());
            if (clock_gettime(CLOCK_MONOTONIC, &finish)) _exit(125);
            struct child_report report = {i, getpid(), start, finish};
            ssize_t written;
            /* Blocking pipe + one record <= PIPE_BUF => no interleaving. */
            do { written = write(fds[1], &report, sizeof(report)); }
            while (written < 0 && errno == EINTR);
            close(fds[1]);
            _exit(written == (ssize_t)sizeof(report) ? 0 : 126);
        }
        if (pid < 0) { perror("fork"); bad = 1; break; }
        children[created++] = pid;
    }
    close(fds[1]); /* EOF is otherwise impossible, even when every child exits. */
    if (bad) for (int i = 0; i < created; ++i) (void)kill(children[i], SIGKILL);
    for (;;) {
        struct child_report report;
        int r = read_report(fds[0], &report);
        if (r == 0) break;
        if (r < 0) { fprintf(stderr, "Truncated or unreadable child report.\n"); bad = 1; break; }
        if (report.index < 0 || report.index >= created || seen[report.index] ||
            report.pid != children[report.index]) { bad = 1; continue; }
        reports[report.index] = report;
        seen[report.index] = 1;
    }
    close(fds[0]);
    for (int i = 0; i < created; ++i) {
        int status;
        pid_t waited;
        do { waited = waitpid(children[i], &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !seen[i]) {
            fprintf(stderr, "Child %d failed or did not report.\n", i);
            bad = 1;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) || getrusage(RUSAGE_CHILDREN, &usage)) {
        perror("final measurement"); return EXIT_FAILURE;
    }
    if (bad || created != CHILDREN) {
        fprintf(stderr, "INVALID RUN: no aggregate statistics produced.\n");
        return EXIT_FAILURE;
    }
    double sum_response = 0, max_response = 0, sum_turnaround = 0, max_turnaround = 0;
    puts("Child  PID       Response(ms)  Turnaround(ms)");
    for (int i = 0; i < CHILDREN; ++i) {
        double response = 1000 * seconds_between(reports[i].start, reference);
        double turnaround = 1000 * seconds_between(reports[i].finish, reference);
        sum_response += response;
        sum_turnaround += turnaround;
        if (response > max_response) max_response = response;
        if (turnaround > max_turnaround) max_turnaround = turnaround;
        printf("%5d  %-8ld  %12.3f  %14.3f\n", i, (long)reports[i].pid, response, turnaround);
    }
    const double elapsed = seconds_between(end, reference);
    printf("Involuntary Context Switches: %ld\nVoluntary Context Switches: %ld\n",
           usage.ru_nivcsw, usage.ru_nvcsw);
    printf("Average Response Time: %.3f ms\nMaximum Response Time: %.3f ms\n",
           sum_response / CHILDREN, max_response);
    printf("Average Turnaround Time: %.3f ms\nMaximum Turnaround Time: %.3f ms\n",
           sum_turnaround / CHILDREN, max_turnaround);
    printf("Throughput: %.6f children/s\nTotal Elapsed Time: %.6f s\n",
           CHILDREN / elapsed, elapsed);
    return EXIT_SUCCESS;
}
#endif
