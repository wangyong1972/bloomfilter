#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace url_bloom {

struct Fingerprint128 {
  std::uint64_t h1;
  std::uint64_t h2;
};

struct BloomMetadata {
  std::uint64_t expected_items;
  std::uint64_t bit_count;
  std::uint64_t bit_bytes;
  std::uint32_t hash_count;
  std::uint64_t seed;
};

Fingerprint128 fingerprint128(std::string_view value, std::uint64_t seed);

std::uint32_t optimal_hash_count(std::uint64_t bit_count,
                                 std::uint64_t expected_items);

std::uint64_t bits_for_bytes(std::uint64_t bytes);

bool is_power_of_two(std::uint64_t value);

std::uint64_t next_power_of_two(std::uint64_t value);

std::uint64_t next_power_of_two_bytes(std::uint64_t bytes);

class UrlBloomFilter {
 public:
  static constexpr std::uint64_t kDefaultExpectedItems = 3'500'000'000ULL;
  static constexpr std::uint64_t kDefaultSizeBytes = 13'000'000'000ULL;
  static constexpr std::uint64_t kDefaultSeed = 0x9ae16a3b2f90404fULL;

  static UrlBloomFilter Create(const std::filesystem::path& path,
                               std::uint64_t expected_items =
                                   kDefaultExpectedItems,
                               std::uint64_t size_bytes = kDefaultSizeBytes,
                               std::uint32_t hash_count = 0,
                               std::uint64_t seed = kDefaultSeed);

  static UrlBloomFilter Open(const std::filesystem::path& path,
                             bool read_only = true);

  UrlBloomFilter(const UrlBloomFilter&) = delete;
  UrlBloomFilter& operator=(const UrlBloomFilter&) = delete;
  UrlBloomFilter(UrlBloomFilter&& other) noexcept;
  UrlBloomFilter& operator=(UrlBloomFilter&& other) noexcept;
  ~UrlBloomFilter();

  const BloomMetadata& metadata() const { return metadata_; }

  void Add(std::string_view url);
  void AddUrls(std::span<const std::string> urls);
  void AddUrlsFromOffsets(std::string_view data,
                          std::span<const std::uint64_t> offsets);

  bool Contains(std::string_view url) const;
  std::vector<std::uint8_t> ContainsUrlsPacked(
      std::span<const std::string> urls) const;
  std::vector<std::uint8_t> ContainsUrlsPackedFromOffsets(
      std::string_view data, std::span<const std::uint64_t> offsets) const;

  void MergeFrom(const std::filesystem::path& other_path);
  void Flush();

 private:
  UrlBloomFilter() = default;

  void MapFile(const std::filesystem::path& path, bool read_only);
  void Close();
  void SetBit(std::uint64_t bit_index);
  bool GetBit(std::uint64_t bit_index) const;

  int fd_ = -1;
  bool read_only_ = true;
  std::uint8_t* mapping_ = nullptr;
  std::uint64_t mapping_size_ = 0;
  std::uint8_t* bits_ = nullptr;
  BloomMetadata metadata_{};
};

}  // namespace url_bloom
