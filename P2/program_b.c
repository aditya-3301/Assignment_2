#define _GNU_SOURCE
#include "workload_io.h"
#define WORKLOAD_NAME "Program-B (I/O-Bound)"
#define RUN_WORKLOAD(id) io_task(id)
#include "benchmark.h"
