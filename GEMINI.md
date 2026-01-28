# PipeANN / OdinANN Project Context

## Project Overview

**PipeANN** (and its variant **OdinANN**) is a high-performance, billion-scale, graph-based vector search system designed for SSDs. It achieves extremely low latency (<1ms) and high throughput while supporting efficient vector updates (inserts/deletes).

*   **Core Technology:** Graph-based Approximate Nearest Neighbor (ANN) search optimized for SSDs (using `io_uring` or `libaio`).
*   **Key Features:**
    *   Low memory footprint (caches only connectivity in DRAM).
    *   High concurrency.
    *   Support for dynamic updates (OdinANN features).
    *   Python and C++ interfaces.

## Directory Structure

*   **`src/`**: Core C++ source code.
    *   `index.cpp`: In-memory Vamana index.
    *   `ssd_index.cpp`: On-disk index implementation.
    *   `search/`: Search algorithms (PipeSearch, BeamSearch, etc.).
    *   `update/`: Update logic (Direct Insert, Delete/Merge).
*   **`include/`**: C++ header files.
*   **`pipeann/`**: Python package source.
*   **`tests/`**: C++ benchmarks, tools, and unit tests.
    *   `build_disk_index.cpp`: Tool to build the on-disk index.
    *   `search_disk_index.cpp`: Main search benchmark tool.
    *   `test_insert_search.cpp`: Benchmark for concurrent insert/search.
*   **`tests_py/`**: Python usage examples.
*   **`scripts/`**: Shell scripts for running experiments and reproducing paper results.
*   **`third_party/`**: Dependencies (e.g., `liburing`).

## Build and Installation

### Prerequisites

*   **OS:** Linux (Kernel >= 5.15 recommended for `io_uring`).
*   **Compilers:** C++17 compliant compiler (`g++`, `clang++`).
*   **Libraries:** `cmake`, `libaio-dev`, `libgoogle-perftools-dev`, `libmkl-full-dev` (or OpenBLAS), `libjemalloc-dev`.
*   **Python:** Python 3.7+.

### Building C++ Core

The project uses CMake. A convenience script is provided:

```bash
# Builds liburing and the main C++ project
bash ./build.sh
```

This creates a `build/` directory containing the compiled binaries (e.g., `build/tests/search_disk_index`).

### Building Python Bindings

```bash
python setup.py install
```

This installs the `pipeann` package into the current Python environment.

## Usage and Workflows

### 1. Data Preparation
Data (base vectors, queries, ground truth) is typically expected in binary format (`.bin` or `.bbin`). Conversion tools are available in `build/tests/utils/`.

### 2. Index Building
Typical workflow involves:
1.  **Generate Random Slice:** Sample data for training.
2.  **Build In-Memory Index:** Build a small graph in memory.
3.  **Build Disk Index:** Build the full on-disk index using `build/tests/build_disk_index`.

### 3. Running Benchmarks
*   **Search Only:** Use `build/tests/search_disk_index`.
*   **Search & Update:** Use `build/tests/test_insert_search`.
*   **Scripts:** Refer to `scripts/hello_world.sh` or `scripts/run_all.sh` for automated benchmark runs.

## Development Conventions

*   **Code Style:** `.clang-format` is present.
*   **Testing:** New features should be verified with existing benchmarks in `tests/` or new scripts in `scripts/`.
*   **Performance:** Performance is critical. Use `Release` builds (`-DCMAKE_BUILD_TYPE=Release`) for timing measurements.
*   **IO:** The system heavily relies on asynchronous IO. Code changes in `src/utils/linux_aligned_file_reader.cpp` or `include/linux_aligned_file_reader.h` should be done with care.

## Key Configuration Macros
(Found in `CMakeLists.txt` or passed via CMake)
*   `USE_AIO`: Toggle between `libaio` and `io_uring`.
*   `READ_ONLY_TESTS`: Optimizes for search-only workloads (excludes update logic).
*   `NO_MAPPING`: Disables certain memory mappings for performance in specific setups.
