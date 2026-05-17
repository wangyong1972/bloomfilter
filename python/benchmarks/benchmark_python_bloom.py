import argparse
import resource
import time


def mix64(x: int) -> int:
    x &= 0xFFFFFFFFFFFFFFFF
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 31
    return x & 0xFFFFFFFFFFFFFFFF


def fnv1a64(value: str, seed: int) -> int:
    h = 1469598103934665603 ^ seed
    for b in value.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def fingerprint(value: str, seed: int) -> tuple[int, int]:
    h1 = mix64(fnv1a64(value, seed ^ 0x243F6A8885A308D3) ^ len(value))
    h2 = mix64(fnv1a64(value, seed ^ 0x13198A2E03707344) ^ (len(value) << 1)) | 1
    return h1, h2


class PythonBloom:
    def __init__(self, size_bytes: int, hash_count: int, seed: int = 123) -> None:
        self.bits = bytearray(size_bytes)
        self.bit_count = size_bytes * 8
        self.hash_count = hash_count
        self.seed = seed

    def add(self, value: str) -> None:
        h1, h2 = fingerprint(value, self.seed)
        for i in range(self.hash_count):
            bit = (h1 + i * h2) % self.bit_count
            self.bits[bit >> 3] |= 1 << (bit & 7)

    def contains(self, value: str) -> bool:
        h1, h2 = fingerprint(value, self.seed)
        for i in range(self.hash_count):
            bit = (h1 + i * h2) % self.bit_count
            if not (self.bits[bit >> 3] & (1 << (bit & 7))):
                return False
        return True


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
    parser.add_argument("--size-bytes", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--hash-count", type=int, default=7)
    args = parser.parse_args()

    bloom = PythonBloom(args.size_bytes, args.hash_count)
    insert_urls = make_urls(args.insert_count, "https://known.example.com")
    query_urls = make_urls(args.query_count, "https://known.example.com")
    for i in range(1, len(query_urls), 2):
        query_urls[i] = f"https://unknown.example.com/item/{i}"

    start = time.perf_counter()
    for url in insert_urls:
        bloom.add(url)
    add_seconds = time.perf_counter() - start

    start = time.perf_counter()
    positives = sum(1 for url in query_urls if bloom.contains(url))
    query_seconds = time.perf_counter() - start

    print("implementation=pure_python")
    print(f"insert_count={args.insert_count}")
    print(f"query_count={args.query_count}")
    print(f"size_bytes={args.size_bytes}")
    print(f"hash_count={args.hash_count}")
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
