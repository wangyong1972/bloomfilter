import argparse
import resource
import tempfile
import time
from array import array
from pathlib import Path

import url_bloom_native


def make_urls(count: int, prefix: str) -> list[str]:
    return [f"{prefix}/item/{i}" for i in range(count)]


def count_packed(packed: bytes) -> int:
    return sum(byte.bit_count() for byte in packed)


def pack_urls(urls: list[str]) -> tuple[bytes, list[int], array]:
    chunks = [url.encode("utf-8") for url in urls]
    offsets = [0]
    for chunk in chunks:
        offsets.append(offsets[-1] + len(chunk))
    return b"".join(chunks), offsets, array("i", offsets)


def make_url_offset_buffers(
    count: int, known_prefix: str, unknown_prefix: str | None = None
) -> tuple[bytes, list[int]]:
    data = bytearray()
    offsets = [0]
    for i in range(count):
        prefix = (
            unknown_prefix
            if unknown_prefix is not None and i % 2 == 1
            else known_prefix
        )
        chunk = f"{prefix}/item/{i}".encode("utf-8")
        data.extend(chunk)
        offsets.append(offsets[-1] + len(chunk))
    return bytes(data), offsets


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


def max_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    # Linux reports KiB; macOS reports bytes.
    if value < 10 * 1024 * 1024:
        return value * 1024
    return value


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--insert-count", type=int, default=1_000_000)
    parser.add_argument("--query-count", type=int, default=1_000_000)
    parser.add_argument("--size-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--hash-count", type=int, default=21)
    parser.add_argument(
        "--mode",
        choices=["list", "offsets", "arrow"],
        default="list",
        help="Python list[str], bytes+offset list, or Arrow-style bytes+offset buffer.",
    )
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bench.bloom"
        bloom = url_bloom_native.UrlBloomFilter.create(
            str(path),
            expected_items=args.insert_count,
            size_bytes=args.size_bytes,
            hash_count=args.hash_count,
        )

        if args.mode == "list":
            insert_urls = make_urls(args.insert_count, "https://known.example.com")
            query_urls = make_urls(args.query_count, "https://known.example.com")
            for i in range(1, len(query_urls), 2):
                query_urls[i] = f"https://unknown.example.com/item/{i}"
            insert_data = query_data = b""
            insert_offsets = query_offsets = []
            insert_arrow_offsets = query_arrow_offsets = array("i")
        elif args.mode == "offsets":
            insert_data, insert_offsets = make_url_offset_buffers(
                args.insert_count, "https://known.example.com"
            )
            query_data, query_offsets = make_url_offset_buffers(
                args.query_count,
                "https://known.example.com",
                "https://unknown.example.com",
            )
            insert_urls = query_urls = []
            insert_arrow_offsets = query_arrow_offsets = array("i")
        else:
            insert_data, insert_arrow_offsets = make_url_arrow_buffers(
                args.insert_count, "https://known.example.com"
            )
            query_data, query_arrow_offsets = make_url_arrow_buffers(
                args.query_count,
                "https://known.example.com",
                "https://unknown.example.com",
            )
            insert_urls = query_urls = []
            insert_offsets = query_offsets = []

        start = time.perf_counter()
        if args.mode == "list":
            bloom.add_urls(insert_urls)
        elif args.mode == "offsets":
            bloom.add_urls_offsets(insert_data, insert_offsets)
        else:
            bloom.add_urls_arrow(insert_data, insert_arrow_offsets)
        add_seconds = time.perf_counter() - start

        start = time.perf_counter()
        if args.mode == "list":
            packed = bloom.contains_urls_packed(query_urls)
        elif args.mode == "offsets":
            packed = bloom.contains_urls_packed_offsets(query_data, query_offsets)
        else:
            packed = bloom.contains_urls_packed_arrow(query_data, query_arrow_offsets)
        query_seconds = time.perf_counter() - start

        print(f"mode={args.mode}")
        print(f"insert_count={args.insert_count}")
        print(f"query_count={args.query_count}")
        print(f"size_bytes={args.size_bytes}")
        print(f"hash_count={bloom.metadata.hash_count}")
        print(f"add_seconds={add_seconds:.6f}")
        print(f"add_urls_per_sec={args.insert_count / add_seconds:.2f}")
        print(f"query_seconds={query_seconds:.6f}")
        print(f"query_urls_per_sec={args.query_count / query_seconds:.2f}")
        print(f"positives={count_packed(packed)}")
        rss = max_rss_bytes()
        print(f"max_rss_bytes={rss}")
        print(f"max_rss_mib={rss / 1024 / 1024:.2f}")


if __name__ == "__main__":
    main()
