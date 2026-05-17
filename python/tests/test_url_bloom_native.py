from pathlib import Path
from array import array

import url_bloom_native


def _bit_is_set(packed: bytes, index: int) -> bool:
    return bool(packed[index >> 3] & (1 << (index & 7)))


def _pack_urls(urls: list[str]) -> tuple[bytes, list[int], array]:
    chunks = [url.encode("utf-8") for url in urls]
    offsets = [0]
    for chunk in chunks:
        offsets.append(offsets[-1] + len(chunk))
    return b"".join(chunks), offsets, array("i", offsets)


def test_add_and_contains_batch(tmp_path: Path) -> None:
    path = tmp_path / "test.bloom"
    bloom = url_bloom_native.UrlBloomFilter.create(
        str(path), expected_items=1000, size_bytes=16 * 1024
    )
    bloom.add_urls(
        [
            "https://example.com/a",
            "https://example.com/b",
            "https://example.com/c",
        ]
    )

    assert bloom.contains_url("https://example.com/a")
    assert not bloom.contains_url("https://example.com/missing")

    packed = bloom.contains_urls_packed(
        [
            "https://example.com/a",
            "https://example.com/missing",
            "https://example.com/c",
        ]
    )
    assert _bit_is_set(packed, 0)
    assert not _bit_is_set(packed, 1)
    assert _bit_is_set(packed, 2)


def test_offsets_and_arrow_batch_interfaces(tmp_path: Path) -> None:
    path = tmp_path / "test.bloom"
    bloom = url_bloom_native.UrlBloomFilter.create(
        str(path), expected_items=1000, size_bytes=16 * 1024
    )

    insert_data, insert_offsets, insert_arrow_offsets = _pack_urls(
        [
            "https://example.com/a",
            "https://example.com/b",
            "https://example.com/c",
        ]
    )
    query_data, query_offsets, query_arrow_offsets = _pack_urls(
        [
            "https://example.com/a",
            "https://example.com/missing",
            "https://example.com/c",
        ]
    )

    bloom.add_urls_offsets(insert_data, insert_offsets)
    packed = bloom.contains_urls_packed_offsets(query_data, query_offsets)
    assert _bit_is_set(packed, 0)
    assert not _bit_is_set(packed, 1)
    assert _bit_is_set(packed, 2)

    arrow_path = tmp_path / "arrow.bloom"
    arrow_bloom = url_bloom_native.UrlBloomFilter.create(
        str(arrow_path), expected_items=1000, size_bytes=16 * 1024
    )
    arrow_bloom.add_urls_arrow(insert_data, insert_arrow_offsets)
    arrow_packed = arrow_bloom.contains_urls_packed_arrow(
        query_data, query_arrow_offsets
    )
    assert _bit_is_set(arrow_packed, 0)
    assert not _bit_is_set(arrow_packed, 1)
    assert _bit_is_set(arrow_packed, 2)


def test_reopen(tmp_path: Path) -> None:
    path = tmp_path / "test.bloom"
    bloom = url_bloom_native.UrlBloomFilter.create(
        str(path), expected_items=1000, size_bytes=16 * 1024
    )
    bloom.add_url("https://example.com/a")
    bloom.flush()

    reopened = url_bloom_native.UrlBloomFilter.open(str(path))
    assert reopened.contains_url("https://example.com/a")
    assert not reopened.contains_url("https://example.com/b")
