# URL Bloom Filter

This project contains a mmap-backed native URL Bloom filter.

Default production sizing is intended for:

```text
expected_items = 3.5B URLs
size_bytes = 13GB
hash_count = optimal for size/items, about 21
```

URL fingerprints are generated with vendored `XXH3_128bits_withSeed()` from
xxHash. The two 64-bit halves of the 128-bit hash feed double hashing for the
Bloom probes.

The Python extension exposes batch lookup:

```python
import url_bloom_native

bloom = url_bloom_native.UrlBloomFilter.create("urls.bloom")
bloom.add_urls(["https://example.com/a", "https://example.com/b"])
packed = bloom.contains_urls_packed(["https://example.com/a", "missing"])
```

Packed result bits use little-endian bit order within each byte:

```text
bit i is stored at packed[i >> 3] & (1 << (i & 7))
```

The library treats input URLs as already canonicalized. URL normalization must be identical during build and query.

## Python Interfaces

The extension exposes three batch styles:

```python
# 1. Simple API. Convenient, but pybind11 converts list[str] to
# std::vector<std::string>, so the URL strings are copied at the Python/C++
# boundary.
packed = bloom.contains_urls_packed(["https://example.com/a", "missing"])

# 2. Offset API. URL bytes are stored once in a contiguous bytes object.
# Offsets define [start, end) ranges for each URL.
data = b"https://example.com/ahttps://example.com/b"
offsets = [0, 21, 42]
bloom.add_urls_offsets(data, offsets)
packed = bloom.contains_urls_packed_offsets(data, offsets)

# 3. Arrow-style API. This accepts a byte buffer plus an int32/int64 offset
# buffer, matching the physical layout of Arrow String/LargeString arrays.
from array import array

arrow_offsets = array("i", [0, 21, 42])
bloom.add_urls_arrow(data, arrow_offsets)
packed = bloom.contains_urls_packed_arrow(data, arrow_offsets)
```

For large Beam/Dataflow batches, prefer the Arrow-style interface. It avoids
materializing one C++ `std::string` per URL and works naturally with columnar
data represented as one values buffer plus one offsets buffer.

## Using Parquet / PyArrow

When the input is a Parquet file with a URL column, read it in Arrow record
batches and pass the Arrow string buffers directly to the native extension:

```python
import pyarrow as pa
import pyarrow.parquet as pq
import url_bloom_native


def count_packed(packed: bytes) -> int:
    return sum(byte.bit_count() for byte in packed)


def match_parquet_urls(parquet_path: str, bloom_path: str) -> int:
    bloom = url_bloom_native.UrlBloomFilter.open(bloom_path)
    parquet_file = pq.ParquetFile(parquet_path)
    positives = 0

    for batch in parquet_file.iter_batches(
        batch_size=1_000_000,
        columns=["url"],
    ):
        urls = batch.column(0)

        if pa.types.is_dictionary(urls.type):
            urls = urls.dictionary_decode()

        if not (pa.types.is_string(urls.type) or pa.types.is_large_string(urls.type)):
            urls = urls.cast(pa.string())

        if urls.null_count:
            raise ValueError("URL column must not contain nulls")

        # Arrow String/LargeString buffers are:
        #   [0] null bitmap, [1] offsets, [2] UTF-8 data bytes.
        _, offsets, data = urls.buffers()
        packed = bloom.contains_urls_packed_arrow(data, offsets, urls.offset, len(urls))
        positives += count_packed(packed)

    return positives
```

This is the intended hot path for Parquet data: Python controls batch iteration,
but the per-URL matching loop runs in C++ over Arrow's contiguous byte and
offset buffers. The current native API does not consume Arrow's null bitmap, so
the URL column should be non-null or nulls should be handled before calling the
Bloom filter.

## Parallel Bloom Filter Build

For billions of input URLs, do not have all workers write to one shared Bloom
file. Each worker should build a local partial Bloom filter with the exact same
metadata, then merge the partial filters with a bitwise OR.

All partial filters must use the same:

```text
expected_items
size_bytes
hash_count
seed
probe algorithm version
URL normalization logic
```

Worker-side build pattern:

```python
import url_bloom_native


def build_partial_bloom(partial_path: str, url_batches) -> str:
    bloom = url_bloom_native.UrlBloomFilter.create(
        partial_path,
        expected_items=3_500_000_000,
        size_bytes=13_000_000_000,
        hash_count=21,
    )

    for data, offsets in url_batches:
        # Prefer Arrow-style buffers when the input comes from Parquet/Arrow.
        bloom.add_urls_arrow(data, offsets)

    bloom.flush()
    return partial_path
```

Driver or merge-stage pattern:

```python
import url_bloom_native


def merge_partials(output_path: str, partial_paths: list[str]) -> None:
    merged = url_bloom_native.UrlBloomFilter.create(
        output_path,
        expected_items=3_500_000_000,
        size_bytes=13_000_000_000,
        hash_count=21,
    )

    for partial_path in partial_paths:
        merged.merge_from(partial_path)

    merged.flush()
```

`merge_from()` is exposed in Python and performs a word-wise OR of another
Bloom file into the destination. It is much cheaper than re-reading all URLs,
but for a 13GB Bloom filter each merge still scans about 13GB from the partial
file and writes the destination file. If there are many partials, use a tree
merge instead of one serial merge stage:

```text
level 0: worker partials
level 1: merge groups of 8-32 partials
level 2: merge group outputs
final:   one complete Bloom filter
```

In Beam/Dataflow, the practical shape is usually:

```text
Read canonical URLs
-> shard/group by build partition
-> each partition writes one partial .bloom file
-> merge partial .bloom files by OR
-> publish final .bloom file for query jobs
```

The final query job can then mmap the published Bloom file read-only on each
worker. Multiple query threads in one worker process can share the same mmap
mapping; the 13GB Bloom file is not copied once per thread.

## Build and Test

Create a virtual environment and install the extension in editable mode:

```bash
cd /Users/wangyong/projects/github/wangyong1972/bloomfilter
python3 -m venv python/.venv
python/.venv/bin/python -m pip install -U pip
python/.venv/bin/python -m pip install -e python
```

Run tests:

```bash
python/.venv/bin/python -m pytest python/tests
```

The C++ core can also be compiled directly:

```bash
mkdir -p cpp/build/manual
cc -O2 -Icpp/third_party/xxhash \
  -c cpp/third_party/xxhash/xxhash.c \
  -o cpp/build/manual/xxhash.o
c++ -std=c++20 -O2 -Icpp/include -Icpp/third_party/xxhash \
  cpp/build/manual/xxhash.o \
  cpp/src/bloom_filter.cc \
  cpp/tests/test_bloom_filter.cc \
  -o cpp/build/manual/url_bloom_test
cpp/build/manual/url_bloom_test
```

## Linux / Docker

The extension is platform-specific. A macOS arm64 build produces a Darwin
shared object and cannot be copied directly to Linux. For Linux, build the
package in a Linux environment.

On a Linux host:

```bash
git clone https://github.com/wangyong1972/bloomfilter.git
cd bloomfilter

python3 -m venv python/.venv
python/.venv/bin/python -m pip install -U pip
python/.venv/bin/python -m pip install -e python
python/.venv/bin/python -m pytest python/tests
```

The Dockerfile builds the extension inside a Linux Beam SDK image:

```bash
docker build -t url-bloom-beam:latest .
```

During the Docker build, this command compiles the native C++ extension for
Linux:

```dockerfile
RUN python -m pip install -v ./python
```

The build log should show CMake/compiler output, and the final verification
step prints the installed native extension path. On Linux it should look similar
to:

```text
/usr/local/lib/python3.12/site-packages/url_bloom_native.cpython-312-x86_64-linux-gnu.so
```

For Dataflow, use this image as the base for the pipeline container or extend it
with the rest of the pipeline dependencies.

## Benchmarks

Native benchmark modes:

```bash
python/.venv/bin/python python/benchmarks/benchmark_url_bloom_native.py \
  --mode list \
  --insert-count 1000000 \
  --query-count 1000000 \
  --size-bytes 67108864 \
  --hash-count 21

python/.venv/bin/python python/benchmarks/benchmark_url_bloom_native.py \
  --mode offsets \
  --insert-count 1000000 \
  --query-count 1000000 \
  --size-bytes 67108864 \
  --hash-count 21

python/.venv/bin/python python/benchmarks/benchmark_url_bloom_native.py \
  --mode arrow \
  --insert-count 1000000 \
  --query-count 1000000 \
  --size-bytes 67108864 \
  --hash-count 21
```

Threaded native Arrow benchmark:

```bash
python/.venv/bin/python python/benchmarks/benchmark_url_bloom_threads.py \
  --threads 4 \
  --insert-count 1000000 \
  --query-count 1000000 \
  --size-bytes 67108864 \
  --hash-count 21
```

`pybloomfiltermmap3` comparison:

```bash
python/.venv/bin/python -m pip install pybloomfiltermmap3
python/.venv/bin/python python/benchmarks/benchmark_pybloomfiltermmap3.py \
  --insert-count 1000000 \
  --query-count 1000000 \
  --error-rate 1e-6
```

Pure Python baseline:

```bash
python/.venv/bin/python python/benchmarks/benchmark_python_bloom.py \
  --insert-count 1000000 \
  --query-count 1000000 \
  --size-bytes 67108864 \
  --hash-count 21
```

Recent local results on an Apple Silicon Mac, using 1M inserts and 1M
match/query URLs:

| Implementation | Filter config | Add URLs/s | Match URLs/s | Max RSS |
| --- | --- | ---: | ---: | ---: |
| Native `list[str]` API | 64MiB, 21 hashes | 3.60M/s | 7.09M/s | 325MiB |
| Native offsets API | 64MiB, 21 hashes | 3.79M/s | 9.65M/s | 329MiB |
| Native Arrow-style API | 64MiB, 21 hashes | 4.12M/s | 10.11M/s | 256MiB |
| `pybloomfiltermmap3` | capacity 1M, error 1e-6 | 1.68M/s | 3.54M/s | 195MiB |
| Pure Python baseline | 64MiB, 21 hashes | 61.5K/s | 77.8K/s | 255MiB |

Threaded native Arrow match results with a shared read-only mmap:

| Threads | Match URLs/s | Max RSS |
| ---: | ---: | ---: |
| 1 | 9.60M/s | 321MiB |
| 2 | 14.44M/s | 288MiB |
| 4 | 22.39M/s | 304MiB |
| 8 | 31.31M/s | 293MiB |

Large-filter local benchmark results on the same machine, using 1M inserted
URLs and 10M match/query URLs:

| Bloom size | Threads | Match URLs/s | Query seconds | Max RSS |
| ---: | ---: | ---: | ---: | ---: |
| 13GB | 4 | 5.49M/s | 1.82s | 25.1GiB |
| 13GB | 8 | 6.11M/s | 1.64s | 25.0GiB |
| 16GiB | 4 | 4.29M/s | 2.33s | 32.9GiB |
| 16GiB | 8 | 4.43M/s | 2.26s | 32.8GiB |

These large-filter numbers measure only the match/query phase. They do not
include creating the mmap file or inserting the 1M URLs into the filter. On this
machine, the 13GB filter was faster than the 16GiB power-of-two filter: the
larger random-access working set outweighed the cheaper bit-mask probe mapping.
For a single full-size filter, 13GB is the better default. Power-of-two sizing is
more attractive after hash sharding, where each shard can be a smaller 64MiB,
128MiB, or 256MiB filter.

The `pybloomfiltermmap3` comparison above uses `capacity=1_000_000` and
`error_rate=1e-6`, which creates a much smaller filter than 64MiB: about
3.6MiB with 19 hash probes. With the native Arrow-style API configured to the
same approximate size, a local run produced:

| Implementation | Filter size | Hash probes | Add URLs/s | Query URLs/s | Max RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Native Arrow-style API | 3.6MiB | 19 | 17.11M/s | 27.12M/s | 196MiB |
| `pybloomfiltermmap3` | ~3.6MiB | 19 | 1.68M/s | 3.54M/s | 195MiB |

`pybloomfiltermmap3` is used from Python one URL at a time in the benchmark:

```python
for url in insert_urls:
    bloom.add(url)

positives = sum(1 for url in query_urls if url in bloom)
```

Each operation still enters its native extension, so it is not equivalent to a
pure Python Bloom filter. The native Arrow-style API is the best interface in
this project for large batches because it avoids copying every URL into a C++
`std::string`.

The probe index uses fast 64-bit range reduction instead of a 128-bit modulo:
power-of-two filter sizes use a bit mask, and other sizes use multiply-high
range mapping. This is faster, but it means Bloom files built by older versions
of this experimental code are not compatible with files built by the current
probe algorithm.

Power-of-two filter sizes are worth considering when the extra memory is
acceptable. They let probe index mapping use a bit mask. For the 13GB production
target, the next power-of-two byte size is 16GiB:

```python
import url_bloom_native

url_bloom_native.next_power_of_two_bytes(13_000_000_000)
# 17179869184
```

That adds about 3.89GiB per mmap file. It can be useful for shard sizes like
64MiB, 128MiB, or 256MiB, but it should be a deliberate sizing decision because
it changes memory footprint and false-positive rate.

For Dataflow, the most important practical lesson is to avoid a Python per-row
hot loop. Pass batches as contiguous URL bytes plus offsets, ideally from an
Arrow-like representation, and return packed result bits.
