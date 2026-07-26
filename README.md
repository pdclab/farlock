<img width="60%" alt="FARLock" src="https://github.com/user-attachments/assets/f146b12b-fbd1-4653-b77c-cc002bfedb00" />

# FARLock: Asymmetric RDMA Locking Made Fair

FARLock is the RDMA-backed lock design described in our [paper](https://www.usenix.org/system/files/osdi26-hu-yuehao.pdf):
```
FARLock: Asymmetric RDMA Locking Made Fair
Yuehao Hu, Jiatang Zhou, Tianzheng Wang, and Keval Vora
OSDI 2026
```

## Layout

- `config/`: runtime configuration files
- `include/`: shared lock, RDMA, logging, and benchmark headers
- `scripts/`: helper scripts for running experiments on remote machines
- `benchmark/`: benchmark `.cc` sources. Each source builds into its own executable that measures throughput and latency for different lock implementations. The produced binaries include:
	- `farlock`
	- `farlockg`
    - `a_lock`
	- `rdma_ticket_lock`
	- `rdma_mcs_lock`
	- `rdma_spin_lock`

## Requirements

The benchmark build expects an RDMA development environment with `ibverbs` available, plus CMake and a C++20 compiler.

On Ubuntu-like systems, the following packages are typically needed:

- `cmake`
- `build-essential`
- `libibverbs-dev`
- `librdmacm-dev`
- `rdma-core`

## Build

Build the benchmark suite from the `benchmark/` directory:

```bash
cd benchmark
cmake -DCMAKE_BUILD_TYPE=Release -DLOG_LEVEL=OFF .
make
```

The CMake project compiles each benchmark source file into its own binary. The build uses a Release configuration by default here, and the CMake files limit compile concurrency to reduce peak memory usage.

If you need to cap or raise the compile fan-out, set `BENCHMARK_COMPILE_CONCURRENCY` before running CMake:

```bash
BENCHMARK_COMPILE_CONCURRENCY=4 cmake -DCMAKE_BUILD_TYPE=Release -DLOG_LEVEL=OFF .
make
```

## Run

Each benchmark binary expects a node ID and an optional config file path:

```bash
./farlock <node_id> [config_file]
```

If no config file is provided, the binary uses `../config/config.json`.

The configuration file controls the RDMA cluster, lock budgets, and benchmark parameters. The default JSON includes:

- RDMA node addresses and port
- number of available nodes
- region and chunk sizes
- local and remote lock budgets
- distribution mode (`uniform` or `selective`)
- thread counts, warmup, duration, cooldown, and sampling interval

Example:

```bash
./farlock 0 ../config/config.json
```

## Experiment Scripts

`scripts/cloudlab.sh` contains helper commands for copying files, installing dependencies, and launching benchmarks across a fixed set of hosts.

`scripts/test.py` can sweep benchmark parameters, rewrite the JSON config, and run the binaries repeatedly.

## Output

The benchmark prints aggregated throughput and latency statistics for local and remote lock operations once node `0` gathers results from the other nodes.

## Paper Release

This code is provided to support the FARLock paper and experiment reproduction. If you use it in your own work, please cite the paper.

Some of the lock implementations are from the [ALock project](https://github.com/sss-lehigh/alock).

- `a_lock`
- `rdma_mcs_lock`
- `rdma_spin_lock`
