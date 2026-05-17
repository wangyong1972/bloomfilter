import argparse
import resource
import tempfile
import time
from array import array
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import url_bloom_native


def make_url_arrow_buffers(
    count: int, known_prefix: str, unknown_prefix: str | None = None
) -> tuple[bytes, array]:
    data = bytearray()
    offsets = array("i", [0])
    for i in range(count):
        prefix = (
            unknown_prefix
            if unknown_prefix is not None and i % 2 == 1
            else known_prefix
        )
        chunk = f"{prefix}/item/{i}".encode("utf-8")
        data.extend(chunk)
        offsets.append(len(data))
    return bytes(data), offsets


def count_packed(packed: bytes) -> int:
    return sum(byte.bit_count() for byte in packed)


def max_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if value < 10 * 1024 * 1024:
        return value * 1024
    return value


def split_counts(total: int, parts: int) -> list[int]:
    base, remainder = divmod(total, parts)
    return [base + (1 if i < remainder else 0) for i in range(parts)]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--insert-count", type=int, default=1_000_000)
    parser.add_argument("--query-count", type=int, default=1_000_000)
    parser.add_argument("--size-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--hash-count", type=int, default=21)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bench.bloom"
        bloom = url_bloom_native.UrlBloomFilter.create(
            str(path),
            expected_items=args.insert_count,
            size_bytes=args.size_bytes,
            hash_count=args.hash_count,
        )

        insert_data, insert_offsets = make_url_arrow_buffers(
            args.insert_count, "https://known.example.com"
        )
        bloom.add_urls_arrow(insert_data, insert_offsets)
        bloom.flush()

        query_batches = [
            make_url_arrow_buffers(
                count,
                "https://known.example.com",
                "https://unknown.example.com",
            )
            for count in split_counts(args.query_count, args.threads)
        ]

        readonly_bloom = url_bloom_native.UrlBloomFilter.open(str(path), read_only=True)

        def run_batch(batch: tuple[bytes, array]) -> int:
            data, offsets = batch
            packed = readonly_bloom.contains_urls_packed_arrow(data, offsets)
            return count_packed(packed)

        start = time.perf_counter()
        with ThreadPoolExecutor(max_workers=args.threads) as executor:
            positives = sum(executor.map(run_batch, query_batches))
        query_seconds = time.perf_counter() - start

        rss = max_rss_bytes()
        print("implementation=native_arrow_threads")
        print(f"insert_count={args.insert_count}")
        print(f"query_count={args.query_count}")
        print(f"size_bytes={args.size_bytes}")
        print(f"hash_count={readonly_bloom.metadata.hash_count}")
        print(f"threads={args.threads}")
        print(f"query_seconds={query_seconds:.6f}")
        print(f"query_urls_per_sec={args.query_count / query_seconds:.2f}")
        print(f"positives={positives}")
        print(f"max_rss_bytes={rss}")
        print(f"max_rss_mib={rss / 1024 / 1024:.2f}")


if __name__ == "__main__":
    main()
