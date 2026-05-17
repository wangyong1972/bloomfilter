import argparse
import resource
import tempfile
import time
from pathlib import Path

from pybloomfilter import BloomFilter


def make_urls(count: int, prefix: str) -> list[str]:
    return [f"{prefix}/item/{i}" for i in range(count)]


def max_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if value < 10 * 1024 * 1024:
        return value * 1024
    return value


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--insert-count", type=int, default=100_000)
    parser.add_argument("--query-count", type=int, default=100_000)
    parser.add_argument("--error-rate", type=float, default=1e-6)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bench.pybloom"
        bloom = BloomFilter(args.insert_count, args.error_rate, str(path))

        insert_urls = make_urls(args.insert_count, "https://known.example.com")
        query_urls = make_urls(args.query_count, "https://known.example.com")
        for i in range(1, len(query_urls), 2):
            query_urls[i] = f"https://unknown.example.com/item/{i}"

        start = time.perf_counter()
        for url in insert_urls:
            bloom.add(url)
        add_seconds = time.perf_counter() - start

        start = time.perf_counter()
        positives = sum(1 for url in query_urls if url in bloom)
        query_seconds = time.perf_counter() - start

        print("implementation=pybloomfiltermmap3")
        print(f"insert_count={args.insert_count}")
        print(f"query_count={args.query_count}")
        print(f"error_rate={args.error_rate}")
        print(f"add_seconds={add_seconds:.6f}")
        print(f"add_urls_per_sec={args.insert_count / add_seconds:.2f}")
        print(f"query_seconds={query_seconds:.6f}")
        print(f"query_urls_per_sec={args.query_count / query_seconds:.2f}")
        print(f"positives={positives}")
        rss = max_rss_bytes()
        print(f"max_rss_bytes={rss}")
        print(f"max_rss_mib={rss / 1024 / 1024:.2f}")


if __name__ == "__main__":
    main()
