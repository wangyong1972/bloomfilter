# URL Bloom Filter

This project contains a mmap-backed native URL Bloom filter.

Default production sizing is intended for:

```text
expected_items = 3.5B URLs
size_bytes = 13GB
hash_count = optimal for size/items, about 21
```

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
c++ -std=c++20 -O2 -Icpp/include \
  cpp/src/bloom_filter.cc cpp/tests/test_bloom_filter.cc \
  -o cpp/build/manual/url_bloom_test
cpp/build/manual/url_bloom_test
```

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
| Native `list[str]` API | 64MiB, 21 hashes | 2.87M/s | 4.30M/s | 325MiB |
| Native offsets API | 64MiB, 21 hashes | 3.11M/s | 5.42M/s | 331MiB |
| Native Arrow-style API | 64MiB, 21 hashes | 3.25M/s | 5.63M/s | 256MiB |
| `pybloomfiltermmap3` | capacity 1M, error 1e-6 | 2.03M/s | 3.77M/s | 195MiB |
| Pure Python baseline | 64MiB, 21 hashes | 61.5K/s | 77.8K/s | 255MiB |

The `pybloomfiltermmap3` comparison above uses `capacity=1_000_000` and
`error_rate=1e-6`, which creates a much smaller filter than 64MiB: about
3.6MiB with 19 hash probes. With the native Arrow-style API configured to the
same approximate size, a local run produced:

| Implementation | Filter size | Hash probes | Add URLs/s | Query URLs/s | Max RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Native Arrow-style API | 3.6MiB | 19 | 8.10M/s | 10.68M/s | 196MiB |
| `pybloomfiltermmap3` | ~3.6MiB | 19 | 1.88M/s | 3.30M/s | 197MiB |

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

For Dataflow, the most important practical lesson is to avoid a Python per-row
hot loop. Pass batches as contiguous URL bytes plus offsets, ideally from an
Arrow-like representation, and return packed result bits.
