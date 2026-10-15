# M3 Max integer sorter

A small C++20 program that sorts raw **8-, 16-, 32-, or 64-bit integers** in
ascending numeric order. Defaults: unsigned 64-bit integers, little-endian files.
Signed values use two's-complement representation. Output keeps the input format;
files have no header, separators, or padding.

## Build and run

Requires macOS, Apple Command Line Tools (`xcode-select --install`), and `make`.
The default ARM build targets the Apple M3 CPU; no third-party libraries are needed.

```sh
make
./intsort sort --bits 32 input.bin output.bin
./intsort sort --bits 64 --signed input.bin output.bin
./intsort sort --bits 16 --big-endian input.bin output.bin
./intsort sort --bits 32 --threads 12 input.bin output.bin
./intsort --help
```

Create a sample and check its result with Python:

```sh
python3 -c 'import struct; open("input.bin", "wb").write(struct.pack("<5I", 9, 2, 4294967295, 0, 2))'
./intsort sort --bits 32 input.bin output.bin
python3 -c 'import struct; print(struct.unpack("<5I", open("output.bin", "rb").read()))'
# (0, 2, 2, 9, 4294967295)
```

The entire file must fit in memory. The sorter uses at most approximately twice the input
size plus about 35 KiB per active worker. Empty files work; partial integers are
rejected. The output is written to a temporary file beside the destination and
renamed after a successful write, so the input and output may be the same path.
Existing output is replaced. New output has private permissions (0600); replacing
a path does not preserve its metadata and replaces a symlink itself. Rename provides
atomic visibility, not a power-loss durability guarantee.

## How it uses the M3 Max

- Large inputs first partition on the highest varying byte. Workers then sort
  independent buckets using local LSD radix passes, keeping their smaller working
  sets in cache. A worker collects all lower-byte histograms in one scan rather
  than rescanning for counts on every pass. A dynamic bucket queue balances work
  without exceeding the requested worker limit.
- Small inputs and uneven bucket distributions retain the parallel LSD path.
  A NEON scan detects constant byte positions and skips their passes. Inputs that
  vary in just one byte use histogram counts to reconstruct the sorted values
  directly, without allocating the scratch buffer.
- Ordered input gets a parallel order check (including chunk boundaries), followed
  by an immediate return or parallel reversal. Random inputs usually reject this
  path after a few comparisons. Scratch storage is left uninitialized because
  every destination value is written before it is read.
- ARM NEON processes 128 bits at a time for signed-key conversion, byte swapping,
  and the variation scan. Signed integers become sortable unsigned keys by flipping
  each integer's top bit; the conversion is reversed after sorting.
- Grand Central Dispatch runs histogram and scatter work in parallel at
  user-initiated QoS. The default worker limit comes from the performance-core
  count, rather than hard-coding one M3 Max configuration. macOS controls actual
  core placement. `--threads` overrides the limit; small inputs use fewer workers.
- Per-worker histograms are aligned to 128 bytes. Each worker buffers a 128-byte
  burst per radix bucket for the initial partition or full-array LSD passes;
  cache-local bucket passes use direct stores. Chunks write disjoint output
  slices without atomic value operations. Inputs below 128 integers use `std::sort`.
- `-O3 -mcpu=apple-m3` lets Apple Clang tune generated code for the target CPU.

This is a CPU implementation. Its NEON and GCD choices follow Apple's guidance on
[tuning CPU performance for Apple silicon](https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon).
It has a scalar/sequential fallback for other CPUs; the supported build and benchmark
target is the M3 Max. To compile for another Apple Silicon Mac, override the flags,
for example `make clean all CXXFLAGS='-O3 -std=c++20 -Wall -Wextra -Wpedantic -pthread -mcpu=native'`.

## Validate and benchmark

```sh
make test
make sanitize
./intsort bench --bits 64 --count 4000000 --threads 12 --repeats 7
./intsort bench --bits 64 --count 4000000 --threads 1 --repeats 7
./intsort bench --bits 32 --count 4000000 --threads 12 --repeats 7
```

Tests compare exact output against `std::sort` and an independent Python integer
oracle across all widths, signedness, byte orders, edge values, duplicate-heavy
inputs, sorted/reversed inputs, constant byte positions, and parallel boundaries.
Additional cases cover the bucket-sort threshold, highly skewed top bytes,
constant middle bytes, and order violations exactly between worker chunks.
CLI checks include malformed files, invalid options, failed output writes, and
same-path input/output. `sanitize` runs the C++ tests with AddressSanitizer and
UndefinedBehaviorSanitizer. Python 3 is needed only for CLI tests and the example.

The benchmark uses reproducible uniform random input, alternates execution order,
reports median times, and verifies exact equality after every repetition. It includes
scratch allocation and signed conversion, excludes input generation/copies and file
I/O, and compares against single-threaded `std::sort`. Throughput is input bytes
divided by sort time, not total memory traffic. Benchmark memory peaks near four
times the input size. Results depend on input distribution, size, and system load.

Measured locally on October 15, 2026, with Apple Clang 21 and an M3 Max
(12 performance + 4 efficiency cores), using 4,000,000 unsigned random integers
and seven repetitions:

| Width | Worker limit | Radix median | `std::sort` median | Speedup |
| --- | ---: | ---: | ---: | ---: |
| 64 bits | 12 (automatic) | 5.856 ms | 108.022 ms | 18.45× |
| 64 bits | 1 | 37.746 ms | 107.726 ms | 2.85× |
| 32 bits | 12 | 2.947 ms | 105.094 ms | 35.66× |

For a direct comparison with the previous implementation, a separate paired
benchmark alternated the old and new sorters in the same process, using seed 42,
seven repetitions, 12 workers, and exact verification against `std::sort`:

| Width | Integers | Previous median | Current median | Improvement |
| --- | ---: | ---: | ---: | ---: |
| 64 bits | 4,000,000 | 15.698 ms | 4.532 ms | 3.46× |
| 32 bits | 4,000,000 | 5.632 ms | 2.557 ms | 2.20× |
| 64 bits | 16,000,000 | 69.886 ms | 22.492 ms | 3.11× |
| 32 bits | 16,000,000 | 22.860 ms | 9.936 ms | 2.30× |

The paired checks also covered equal values, narrow ranges, sorted and reversed
input, and a top byte with 99.9% of values in one bucket. Cache state and execution
order differ from the standalone CLI benchmark, so its absolute timings differ.

These are in-memory timings against a single-threaded baseline, not file-sorting
or universal speedup claims. The recorded 12-worker runs use the performance-core
count as a task limit; the scheduler decides where those tasks run.
