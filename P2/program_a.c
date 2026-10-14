#define _GNU_SOURCE
#include "workload_cpu.h"
#define WORKLOAD_NAME "Program-A (CPU-Bound)"
#define RUN_WORKLOAD(id) cpu_task(id)
#include "benchmark.h"
