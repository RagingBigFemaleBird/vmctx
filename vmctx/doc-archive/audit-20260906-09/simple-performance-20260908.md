# Simpler program baseline — userspace 2b3a5b6d2e75

Three measured repetitions; all 72 selected runs passed and released their contexts. AMD source: Linux 7.0.14 #198. Intel executor: Alpine Linux 6.18.35 #62. No governor or spin tuning. Forwarded wall times use executor lifetime, excluding SSH and cleanup. Native wall uses pidfd completion.

| Program | Native AMD (ms) | Loopback (ms) | AMD → Intel (ms) |
|---|---:|---:|---:|
| wc | 7.04 | 161.00 | 648.00 |
| sha256 | 25.14 | 340.00 | 1727.00 |
| gzip | 107.73 | 858.00 | 2278.00 |
| python | 29.02 | 261.00 | 1216.00 |
| vmbench | 95.97 | 885.00 | 5724.00 |
| thread-lock | 15.47 | 2900.00 | 5295.00 |
| fork-pages | 2.49 | 108.00 | 195.00 |
| io1 | 10004.50 | 10087.00 | 10586.00 |

20 million compute iterations: AMD native 0.0941 s, loopback 0.0915 s, remote Intel 0.1266 s, Intel native 0.1188 s. Frequency variation is visible in raw runs, so small compute differences are not evidence of a stable speedup.

getppid latency (µs/call): native 0.14, loopback 152.98, cross 1103.44.

Verified io1 buffer throughput (MiB/s, bytes checked during its 10-second loop): native 1141.22, loopback 11.72, cross 3.03. This includes poisoning, reads, per-byte verification and time queries, and is not raw storage bandwidth.

Selected evidence: all six first-case groups from simple-performance-3 plus fork-pages/io1 from simple-performance-4. Attempt 3 stopped on an Alpine shell cleanup syntax error after a successful workload; that failure is preserved. Attempt 2 native timings had subprocess polling bias and are excluded. Raw stdout, service counters, fixture hashes and machine metadata are retained in simple-performance-3-4-raw.tar.gz.

## Compact CPU-state comparison

Three repetitions per case and mode, alternating old/new order. All 60 runs passed with complete cleanup. Old: 2b3a5b6d2e75; new: b3f52b9697d6. The kernels, input files and guest executables were identical.

| Program | Loopback old → new (s) | Cross-host old → new (s) |
|---|---:|---:|
| vmbench | 0.881 → 0.889 | 5.877 → 3.437 |
| gzip | 0.981 → 0.994 | 2.252 → 2.172 |
| python | 0.281 → 0.292 | 1.248 → 1.139 |
| thread-lock | 3.056 → 2.928 | 5.446 → 4.869 |
| io1 | 10.106 → 10.101 | 10.587 → 10.548 |

Cross-host getppid: 1140.23 → 645.77 µs/call (43.4% lower median latency). Loopback is effectively unchanged.

Verified io1 throughput (MiB/s): loopback 10.01 → 14.20, cross 3.10 → 3.33. Both io1 wall times stay near ten seconds because its verification loop is time-bounded.

The change removes unused CPU-state buffer capacity from transmission, preserving every active component. This reduces cross-host syscall cost but does not resolve the remaining page-transfer and round-trip costs. Smaller differences with only three runs are descriptive, not statistically established.

Evidence: cpu-wire-performance-1-raw.tar.gz, cpu-wire-suite-raw.tar.gz (63/63), and eight cpu-wire-cross controls, including signal/AVX exception restoration.
