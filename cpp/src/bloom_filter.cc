#include "url_bloom/bloom_filter.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xxhash.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace url_bloom {
namespace {

constexpr std::uint64_t kMagic = 0x4d4f4f4c4252554cULL;  // "LURBLOOM" LE-ish.
constexpr std::uint32_t kVersion = 1;
constexpr std::uint64_t kHeaderSize = 4096;

struct FileHeader {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint64_t expected_items;
  std::uint64_t bit_count;
  std::uint64_t bit_bytes;
  std::uint32_t hash_count;
  std::uint32_t reserved0;
  std::uint64_t seed;
  std::uint8_t reserved[4040];
};

static_assert(sizeof(FileHeader) == kHeaderSize);

std::uint64_t RoundUpToWordBytes(std::uint64_t bytes) {
  return (bytes + 7ULL) & ~7ULL;
}

std::uint64_t Mix64(std::uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

std::uint64_t Fnv1a64(std::string_view value, std::uint64_t seed) {
  std::uint64_t hash = 1469598103934665603ULL ^ seed;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t ProbeIndex(const Fingerprint128& fp, std::uint32_t i,
                         std::uint64_t bit_count) {
  const std::uint64_t value = fp.h1 + static_cast<std::uint64_t>(i) * fp.h2;
  if (is_power_of_two(bit_count)) {
    return value & (bit_count - 1);
  }
  return static_cast<std::uint64_t>(
      (static_cast<unsigned __int128>(value) * bit_count) >> 64);
}

FileHeader* Header(std::uint8_t* mapping) {
  return reinterpret_cast<FileHeader*>(mapping);
}

const FileHeader* Header(const std::uint8_t* mapping) {
  return reinterpret_cast<const FileHeader*>(mapping);
}

std::runtime_error SystemError(const std::string& action,
                               const std::filesystem::path& path) {
  return std::runtime_error(action + " failed for " + path.string() + ": " +
                            std::strerror(errno));
}

}  // namespace

Fingerprint128 fingerprint128(std::string_view value, std::uint64_t seed) {
  const auto hash = XXH3_128bits_withSeed(value.data(), value.size(), seed);
  return Fingerprint128{hash.low64, hash.high64 | 1ULL};
}

std::uint32_t optimal_hash_count(std::uint64_t bit_count,
                                 std::uint64_t expected_items) {
  if (expected_items == 0) {
    throw std::invalid_argument("expected_items must be greater than zero");
  }
  const double bits_per_item =
      static_cast<double>(bit_count) / static_cast<double>(expected_items);
  const auto k = static_cast<std::uint32_t>(std::llround(bits_per_item *
                                                        std::log(2.0)));
  return std::max<std::uint32_t>(1, k);
}

std::uint64_t bits_for_bytes(std::uint64_t bytes) {
  return RoundUpToWordBytes(bytes) * 8ULL;
}

bool is_power_of_two(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t next_power_of_two(std::uint64_t value) {
  if (value <= 1) {
    return 1;
  }
  if (value > (1ULL << 63)) {
    throw std::overflow_error("next_power_of_two would overflow uint64_t");
  }
  --value;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value |= value >> 32;
  return value + 1;
}

std::uint64_t next_power_of_two_bytes(std::uint64_t bytes) {
  return next_power_of_two(RoundUpToWordBytes(bytes));
}

UrlBloomFilter UrlBloomFilter::Create(const std::filesystem::path& path,
                                      std::uint64_t expected_items,
                                      std::uint64_t size_bytes,
                                      std::uint32_t hash_count,
                                      std::uint64_t seed) {
  if (expected_items == 0) {
    throw std::invalid_argument("expected_items must be greater than zero");
  }
  if (size_bytes == 0) {
    throw std::invalid_argument("size_bytes must be greater than zero");
  }

  const std::uint64_t bit_bytes = RoundUpToWordBytes(size_bytes);
  const std::uint64_t bit_count = bit_bytes * 8ULL;
  if (hash_count == 0) {
    hash_count = optimal_hash_count(bit_count, expected_items);
  }

  const std::uint64_t file_size = kHeaderSize + bit_bytes;
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw SystemError("open", path);
  }
  if (::ftruncate(fd, static_cast<off_t>(file_size)) != 0) {
    const auto err = SystemError("ftruncate", path);
    ::close(fd);
    throw err;
  }

  void* mapping =
      ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    const auto err = SystemError("mmap", path);
    ::close(fd);
    throw err;
  }

  auto* bytes = static_cast<std::uint8_t*>(mapping);
  auto* header = Header(bytes);
  std::memset(header, 0, sizeof(FileHeader));
  header->magic = kMagic;
  header->version = kVersion;
  header->header_size = kHeaderSize;
  header->expected_items = expected_items;
  header->bit_count = bit_count;
  header->bit_bytes = bit_bytes;
  header->hash_count = hash_count;
  header->seed = seed;

  UrlBloomFilter filter;
  filter.fd_ = fd;
  filter.read_only_ = false;
  filter.mapping_ = bytes;
  filter.mapping_size_ = file_size;
  filter.bits_ = bytes + kHeaderSize;
  filter.metadata_ =
      BloomMetadata{expected_items, bit_count, bit_bytes, hash_count, seed};
  return filter;
}

UrlBloomFilter UrlBloomFilter::Open(const std::filesystem::path& path,
                                    bool read_only) {
  UrlBloomFilter filter;
  filter.MapFile(path, read_only);
  return filter;
}

UrlBloomFilter::UrlBloomFilter(UrlBloomFilter&& other) noexcept {
  *this = std::move(other);
}

UrlBloomFilter& UrlBloomFilter::operator=(UrlBloomFilter&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Close();
  fd_ = other.fd_;
  read_only_ = other.read_only_;
  mapping_ = other.mapping_;
  mapping_size_ = other.mapping_size_;
  bits_ = other.bits_;
  metadata_ = other.metadata_;
  other.fd_ = -1;
  other.mapping_ = nullptr;
  other.bits_ = nullptr;
  other.mapping_size_ = 0;
  return *this;
}

UrlBloomFilter::~UrlBloomFilter() { Close(); }

void UrlBloomFilter::MapFile(const std::filesystem::path& path,
                             bool read_only) {
  const int flags = read_only ? O_RDONLY : O_RDWR;
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    throw SystemError("open", path);
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const auto err = SystemError("fstat", path);
    ::close(fd);
    throw err;
  }
  if (st.st_size < static_cast<off_t>(kHeaderSize)) {
    ::close(fd);
    throw std::runtime_error("Bloom file is too small: " + path.string());
  }

  const int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
  void* mapping = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), prot,
                         MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    const auto err = SystemError("mmap", path);
    ::close(fd);
    throw err;
  }

  auto* bytes = static_cast<std::uint8_t*>(mapping);
  const auto* header = Header(bytes);
  if (header->magic != kMagic || header->version != kVersion ||
      header->header_size != kHeaderSize) {
    ::munmap(mapping, static_cast<std::size_t>(st.st_size));
    ::close(fd);
    throw std::runtime_error("Invalid Bloom file header: " + path.string());
  }
  if (static_cast<std::uint64_t>(st.st_size) !=
      kHeaderSize + header->bit_bytes) {
    ::munmap(mapping, static_cast<std::size_t>(st.st_size));
    ::close(fd);
    throw std::runtime_error("Bloom file size does not match header: " +
                             path.string());
  }

  fd_ = fd;
  read_only_ = read_only;
  mapping_ = bytes;
  mapping_size_ = static_cast<std::uint64_t>(st.st_size);
  bits_ = bytes + kHeaderSize;
  metadata_ = BloomMetadata{header->expected_items, header->bit_count,
                            header->bit_bytes, header->hash_count,
                            header->seed};
}

void UrlBloomFilter::Close() {
  if (mapping_ != nullptr) {
    if (!read_only_) {
      ::msync(mapping_, mapping_size_, MS_SYNC);
    }
    ::munmap(mapping_, mapping_size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = -1;
  mapping_ = nullptr;
  bits_ = nullptr;
  mapping_size_ = 0;
}

void UrlBloomFilter::SetBit(std::uint64_t bit_index) {
  if (read_only_) {
    throw std::runtime_error("cannot add to a read-only Bloom filter");
  }
  const std::uint64_t word_index = bit_index >> 6;
  const std::uint64_t mask = 1ULL << (bit_index & 63ULL);
  auto* words = reinterpret_cast<std::uint64_t*>(bits_);
  std::atomic_ref<std::uint64_t> word(words[word_index]);
  word.fetch_or(mask, std::memory_order_relaxed);
}

bool UrlBloomFilter::GetBit(std::uint64_t bit_index) const {
  const std::uint64_t word_index = bit_index >> 6;
  const std::uint64_t mask = 1ULL << (bit_index & 63ULL);
  const auto* words = reinterpret_cast<const std::uint64_t*>(bits_);
  return (words[word_index] & mask) != 0;
}

void UrlBloomFilter::Add(std::string_view url) {
  const auto fp = fingerprint128(url, metadata_.seed);
  for (std::uint32_t i = 0; i < metadata_.hash_count; ++i) {
    SetBit(ProbeIndex(fp, i, metadata_.bit_count));
  }
}

void UrlBloomFilter::AddUrls(std::span<const std::string> urls) {
  for (const auto& url : urls) {
    Add(url);
  }
}

void UrlBloomFilter::AddUrlsFromOffsets(
    std::string_view data, std::span<const std::uint64_t> offsets) {
  if (offsets.empty()) {
    throw std::invalid_argument("offsets must contain at least one value");
  }
  for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
    const auto start = offsets[i];
    const auto end = offsets[i + 1];
    if (start > end || end > data.size()) {
      throw std::invalid_argument("offsets must be sorted and within data");
    }
    Add(std::string_view(data.data() + start, end - start));
  }
}

bool UrlBloomFilter::Contains(std::string_view url) const {
  const auto fp = fingerprint128(url, metadata_.seed);
  for (std::uint32_t i = 0; i < metadata_.hash_count; ++i) {
    if (!GetBit(ProbeIndex(fp, i, metadata_.bit_count))) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> UrlBloomFilter::ContainsUrlsPacked(
    std::span<const std::string> urls) const {
  std::vector<std::uint8_t> packed((urls.size() + 7) / 8, 0);
  for (std::size_t i = 0; i < urls.size(); ++i) {
    if (Contains(urls[i])) {
      packed[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7U));
    }
  }
  return packed;
}

std::vector<std::uint8_t> UrlBloomFilter::ContainsUrlsPackedFromOffsets(
    std::string_view data, std::span<const std::uint64_t> offsets) const {
  if (offsets.empty()) {
    throw std::invalid_argument("offsets must contain at least one value");
  }
  const std::size_t count = offsets.size() - 1;
  std::vector<std::uint8_t> packed((count + 7) / 8, 0);
  for (std::size_t i = 0; i < count; ++i) {
    const auto start = offsets[i];
    const auto end = offsets[i + 1];
    if (start > end || end > data.size()) {
      throw std::invalid_argument("offsets must be sorted and within data");
    }
    if (Contains(std::string_view(data.data() + start, end - start))) {
      packed[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7U));
    }
  }
  return packed;
}

void UrlBloomFilter::MergeFrom(const std::filesystem::path& other_path) {
  if (read_only_) {
    throw std::runtime_error("cannot merge into a read-only Bloom filter");
  }
  auto other = UrlBloomFilter::Open(other_path, true);
  if (other.metadata_.bit_count != metadata_.bit_count ||
      other.metadata_.hash_count != metadata_.hash_count ||
      other.metadata_.seed != metadata_.seed) {
    throw std::runtime_error("cannot merge Bloom filters with different "
                             "bit_count/hash_count/seed");
  }

  auto* dst = reinterpret_cast<std::uint64_t*>(bits_);
  const auto* src = reinterpret_cast<const std::uint64_t*>(other.bits_);
  const std::uint64_t words = metadata_.bit_bytes / sizeof(std::uint64_t);
  for (std::uint64_t i = 0; i < words; ++i) {
    std::atomic_ref<std::uint64_t> word(dst[i]);
    word.fetch_or(src[i], std::memory_order_relaxed);
  }
}

void UrlBloomFilter::Flush() {
  if (!read_only_ && mapping_ != nullptr) {
    if (::msync(mapping_, mapping_size_, MS_SYNC) != 0) {
      throw std::runtime_error("msync failed: " + std::string(std::strerror(errno)));
    }
  }
}

}  // namespace url_bloom
