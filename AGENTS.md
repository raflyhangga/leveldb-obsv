# Repository Guidelines

## Project Structure & Module Organization

This repository is a LevelDB fork with added compaction tracing and workload tooling. Public APIs live in `include/leveldb/`. Core implementation is split across `db/`, `table/`, `util/`, `port/`, and `helpers/`. Benchmarks are in `benchmarks/`, GoogleTest-based tests are colocated as `*_test.cc` files under `db/`, `table/`, `util/`, `helpers/`, and `issues/`. CMake support is in `CMakeLists.txt` and `cmake/`. Tracing scripts and workload helpers live in `scripts/`; sample data is in `res/`; design and experiment notes are in `doc/`, `technical_notes/`, and `research_notes/`.

## Build, Test, and Development Commands

- `git submodule update --init --recursive`: fetch required third-party dependencies.
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`: configure a release build.
- `cmake --build build -j`: build the library, tests, and enabled tools.
- `cmake --build build --target db_bench -j`: build the benchmark binary used for workload tracing.
- `ctest --test-dir build --verbose`: run the registered test suite.
- `./build/db_bench --db=/tmp/ldb --benchmarks=fillrandom,stats --compaction_trace_path=/tmp/trace.csv`: run a benchmark with compaction trace output.

## Coding Style & Naming Conventions

Follow Google C++ style. Format changed C/C++ files with `clang-format -i --style=file <file>`, using the repository `.clang-format`. Use two-space indentation, include ordering from `.clang-format`, `snake_case` for functions and variables, and `CamelCase` for types. Keep public API changes in `include/leveldb/` conservative and maintain backward compatibility.

## Testing Guidelines

Tests use GoogleTest and are registered through CMake. Name new tests `*_test.cc` and place them near the code they exercise, such as `db/compaction_trace_writer_test.cc` for DB internals. All behavior changes should include a test or a clear justification. For tracing work, also validate emitted CSV shape and event ordering with focused benchmark runs or script checks.

## Commit & Pull Request Guidelines

Recent commits use short imperative summaries, for example `Implement Compaction Trace Writer for detailed compaction lifecycle logging` or `reconfigure workload shell script`. Keep commits focused and include relevant tests or workload validation in the message body when behavior changes. Pull requests should describe the motivation, list key files touched, link related issues or notes, and include test commands run. Add trace snippets or benchmark output when changing compaction instrumentation.

## Agent-Specific Instructions

Do not rewrite upstream LevelDB build configuration unless required for the task. Preserve existing research notes and generated traces unless explicitly asked to clean them up. Prefer narrow changes that keep the fork easy to compare with upstream.
