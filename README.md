# ProcWarden

ProcWarden is an ICS433 Operating Systems project for studying how to monitor and clean up related processes. The current build provides `warden-sim`, a simulator that creates repeatable workloads. Its parent process forks children, reads their memory reports through a pipe, and reaps them on shutdown.

## Build and run

From the repository root on macOS, use Make and Clang to build and run a small example:

```sh
make
./build/bin/warden-sim watcher -n 2 --mb 8
```

This starts two idle children with 8 MB each. After about a second, look for `alive=2/2` and `total=16MB` in the output. Press Ctrl+C to stop the simulator and its children.

## Workload profiles

| Profile | What it simulates |
| --- | --- |
| `install` | Children gradually allocate memory. |
| `devserver` | A local development server listens on a TCP port. |
| `watcher` | Idle children hold memory and report their usage. |
| `agents` | Worker threads can simulate a growing memory leak. |

## Project files

- `src/sim/` contains the parent simulator and the child workload implementations.
- `src/common/` contains shared code for configuration, logging, and interprocess communication. Its headers and the simulator interface are in `include/`.
- `tests/` contains C checks and a shell script that exercises the simulator profiles.
- `docs/` contains the project proposal, phase reports, and implementation notes.
