#include "url_bloom/bloom_filter.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> GenerateUrls(std::size_t count, std::string_view prefix) {
  std::vector<std::string> urls;
  urls.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    urls.push_back(std::string(prefix) + "/item/" + std::to_string(i));
  }
  return urls;
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

std::size_t CountPacked(const std::vector<std::uint8_t>& packed) {
  std::size_t count = 0;
  for (std::uint8_t byte : packed) {
    count += static_cast<std::size_t>(__builtin_popcount(byte));
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t insert_count =
      argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 1'000'000;
  const std::size_t query_count =
      argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 1'000'000;
  const std::uint64_t size_bytes =
      argc > 3 ? static_cast<std::uint64_t>(std::stoull(argv[3])) : 64ULL * 1024 * 1024;
  const std::uint32_t hash_count =
      argc > 4 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 21;

  const auto path = std::filesystem::temp_directory_path() / "url_bloom_bench.bloom";
  std::filesystem::remove(path);

  auto filter =
      url_bloom::UrlBloomFilter::Create(path, insert_count, size_bytes, hash_count);

  auto insert_urls = GenerateUrls(insert_count, "https://known.example.com");
  auto query_urls = GenerateUrls(query_count, "https://known.example.com");
  for (std::size_t i = 1; i < query_urls.size(); i += 2) {
    query_urls[i] = "https://unknown.example.com/item/" + std::to_string(i);
  }

  auto start = std::chrono::steady_clock::now();
  filter.AddUrls(insert_urls);
  const double add_seconds = SecondsSince(start);

  start = std::chrono::steady_clock::now();
  const auto packed = filter.ContainsUrlsPacked(query_urls);
  const double query_seconds = SecondsSince(start);
  const std::size_t positives = CountPacked(packed);

  std::cout << "insert_count=" << insert_count << "\n";
  std::cout << "query_count=" << query_count << "\n";
  std::cout << "size_bytes=" << size_bytes << "\n";
  std::cout << "hash_count=" << filter.metadata().hash_count << "\n";
  std::cout << "add_seconds=" << add_seconds << "\n";
  std::cout << "add_urls_per_sec=" << insert_count / add_seconds << "\n";
  std::cout << "query_seconds=" << query_seconds << "\n";
  std::cout << "query_urls_per_sec=" << query_count / query_seconds << "\n";
  std::cout << "positives=" << positives << "\n";

  std::filesystem::remove(path);
  return 0;
}
