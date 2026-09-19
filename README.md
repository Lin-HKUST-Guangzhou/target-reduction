# target-reduction

This repository is the reference implementation for:

> Zhuofan Lin and Shiju Lin, "Node Reduction via Targeted ODC-Based Wire Additions for AIG Optimization," ASP-DAC, 2027.

## Build

Target-Reduction is a CPU implementation and requires a C++17 compiler and CMake 3.18 or newer.

```bash
git submodule update --init --recursive
mkdir -p build && cd build
cmake .. && make
```

## Usage

Run one optimization round:

```bash
./build/target-reduction -c \
  'read benchmarks/iwls2022/ex00.aig; tred; write ex00_optimized.aig'
```

Run until one consecutive round has no node reduction:

```bash
./build/target-reduction -c \
  'read benchmarks/iwls2022/ex00.aig; tred -Q 1; write ex00_optimized.aig'
```

The interactive command line is also supported by using `./build/target-reduction`. Available commands are `read`, `write`, `ps`, and `tred`. Use `tred -h` for all optimization options.

## Benchmarks

The `benchmarks` directory provides the structurally hashed AIGs used in the
IWLS 2022, IWLS 2023, and IWLS 2024 experiments. Each suite contains 100 AIGs.

## Batch Run

Use `run.py` to optimize a benchmark suite and generate optimized AIGs, logs,
and a `summary.csv` file:

```bash
python3 run.py benchmarks/iwls2022 \
  --engine-path ./build/target-reduction \
  --cmd 'tred -Q 1' \
  --output-dir outputs/iwls2022 \
  --log-dir logs
```

CEC is disabled by default. To check every optimized AIG, enable it explicitly
and provide the path to a working ABC executable:

```bash
python3 run.py benchmarks/iwls2022 \
  --engine-path ./build/target-reduction \
  --cmd 'tred -Q 1' \
  --output-dir outputs/iwls2022 \
  --log-dir logs \
  --cec \
  --cec-path /path/to/abc
```

`--cec-path` must point to an executable ABC binary that supports the `cec`
command. The default `../abc/abc` works only when ABC exists at that location.

## License

MIT License. See `LICENSE`.
