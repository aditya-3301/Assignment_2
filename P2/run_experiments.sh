#!/usr/bin/env bash
# Run on the Ubuntu VM. Writes all 14 real measurements; restores RR sysctl.
set -Eeuo pipefail
cd -- "$(dirname -- "$0")"
if (( EUID != 0 )); then
    echo "Run: sudo bash P2/run_experiments.sh" >&2
    exit 1
fi
cpu=${CPU_CORE:-0}
priority=${RT_PRIORITY:-20}
[[ $cpu =~ ^[0-9]+$ && $priority =~ ^[0-9]+$ ]] || exit 1
(( priority >= 10 && priority <= 50 )) || { echo 'Priority must be 10..50.' >&2; exit 1; }
taskset -c "$cpu" true
[[ -x program_a && -x program_b ]] || { echo 'Run make first.' >&2; exit 1; }
# Avoid overlapping runs which would change the sysctl and distort results.
exec 9>/run/lock/os-assignment2-p2.lock
flock -n 9 || { echo 'Another experiment is running.' >&2; exit 1; }
old=$(sysctl -n kernel.sched_rr_timeslice_ms)
trap 'sysctl -q -w "kernel.sched_rr_timeslice_ms=$old" || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
stamp=$(date -u +%Y%m%dT%H%M%S)
partial="results-${stamp}.partial.txt"
{
    echo 'OS Assignment 2 - P2 measurements'
    date -u --iso-8601=seconds
    uname -a
    cat /etc/os-release
    echo "CPU=$cpu priority=$priority"
    echo 'Filesystem backing /tmp (I/O uses fsync):'
    findmnt -T /tmp -o TARGET,SOURCE,FSTYPE,OPTIONS
    echo 'RT bandwidth configuration:'
    sysctl kernel.sched_rt_period_us kernel.sched_rt_runtime_us
    echo 'Workload source checksums:'
    sha256sum workload_cpu.h workload_io.h
} > "$partial"
success=0
for program in program_a program_b; do
    for quantum in 1 10 50 150 300 500; do
        echo "Running $program RR ${quantum} ms (7 runs per program)..."
        sysctl -q -w "kernel.sched_rr_timeslice_ms=$quantum"
        printf '\n===== %s / RR / requested %s ms =====\n' "$program" "$quantum" >> "$partial"
        # timeout runs at normal policy, then chrt changes only the benchmark.
        # It signals the entire benchmark process group, including all children.
        if timeout --kill-after=5s 120s taskset -c "$cpu" chrt -r "$priority" "./$program" >> "$partial" 2>&1; then
            success=$((success + 1))
        else
            code=$?
            echo "FAILED (exit=$code). Partial data: P2/$partial" >&2
            echo "INVALID RUN: exit=$code" >> "$partial"
            exit 1
        fi
    done
    echo "Running $program FIFO (30-second cap)..."
    printf '\n===== %s / FIFO =====\n' "$program" >> "$partial"
    if timeout --kill-after=5s 30s taskset -c "$cpu" chrt -f "$priority" "./$program" >> "$partial" 2>&1; then
        success=$((success + 1))
    else
        code=$?
        echo "FIFO FAILED (exit=$code); retain the timeout evidence and rerun on an idle VM." >&2
        echo "INVALID RUN: exit=$code" >> "$partial"
        exit 1
    fi
done
echo "COMPLETE: $success / 14 successful measurements" >> "$partial"
# Keep any earlier complete dataset. Never disguise a partial run as complete.
if [[ -e results.txt ]]; then cp -- results.txt "results-before-${stamp}.txt"; fi
mv -- "$partial" results.txt
if [[ -n ${SUDO_UID:-} && -n ${SUDO_GID:-} ]]; then chown "$SUDO_UID:$SUDO_GID" results.txt; fi
echo 'Saved all 14 measurements to P2/results.txt.'
