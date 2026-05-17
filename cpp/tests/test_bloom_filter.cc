#include "url_bloom/bloom_filter.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path TempPath(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

bool PackedBit(const std::vector<std::uint8_t>& packed, std::size_t index) {
  return (packed[index >> 3] & (1U << (index & 7U))) != 0;
}

}  // namespace

int main() {
  const auto path = TempPath("url_bloom_test.bloom");
  const auto path2 = TempPath("url_bloom_test_2.bloom");
  std::filesystem::remove(path);
  std::filesystem::remove(path2);

  {
    auto filter = url_bloom::UrlBloomFilter::Create(
        path, /*expected_items=*/1000, /*size_bytes=*/16 * 1024,
        /*hash_count=*/0, /*seed=*/123);
    assert(filter.metadata().hash_count > 0);

    const std::vector<std::string> urls = {
        "https://example.com/a",
        "https://example.com/b",
        "https://example.com/c",
    };
    filter.AddUrls(urls);
    assert(filter.Contains("https://example.com/a"));
    assert(filter.Contains("https://example.com/b"));
    assert(!filter.Contains("https://example.com/missing"));

    const std::vector<std::string> query = {
        "https://example.com/a",
        "https://example.com/missing",
        "https://example.com/c",
    };
    const auto packed = filter.ContainsUrlsPacked(query);
    assert(PackedBit(packed, 0));
    assert(!PackedBit(packed, 1));
    assert(PackedBit(packed, 2));
    filter.Flush();
  }

  {
    const auto filter = url_bloom::UrlBloomFilter::Open(path);
    assert(filter.Contains("https://example.com/a"));
    assert(!filter.Contains("https://example.com/missing"));
  }

  {
    auto a = url_bloom::UrlBloomFilter::Create(path, 1000, 16 * 1024, 0, 456);
    auto b = url_bloom::UrlBloomFilter::Create(path2, 1000, 16 * 1024,
                                               a.metadata().hash_count, 456);
    a.Add("https://example.com/a");
    b.Add("https://example.com/b");
    a.MergeFrom(path2);
    assert(a.Contains("https://example.com/a"));
    assert(a.Contains("https://example.com/b"));
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path2);
  return 0;
}

