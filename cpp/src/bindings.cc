#include "url_bloom/bloom_filter.h"

#include <pybind11/pytypes.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <memory>
#include <optional>

namespace py = pybind11;

namespace {

std::string_view BufferAsBytes(const py::buffer& buffer, py::buffer_info* info) {
  *info = buffer.request();
  if (info->itemsize != 1) {
    throw std::invalid_argument("data buffer itemsize must be 1 byte");
  }
  if (info->ndim != 1) {
    throw std::invalid_argument("data buffer must be one-dimensional");
  }
  return std::string_view(static_cast<const char*>(info->ptr),
                          static_cast<std::size_t>(info->size));
}

std::vector<std::uint64_t> OffsetsFromSequence(const py::sequence& offsets) {
  std::vector<std::uint64_t> out;
  out.reserve(py::len(offsets));
  for (const auto item : offsets) {
    out.push_back(py::cast<std::uint64_t>(item));
  }
  return out;
}

template <typename OffsetT, typename Fn>
void ForEachArrowValue(std::string_view data, const py::buffer_info& offsets_info,
                       std::size_t start, std::size_t count, Fn fn) {
  if (offsets_info.ndim != 1 || offsets_info.itemsize != sizeof(OffsetT)) {
    throw std::invalid_argument("offsets buffer has the wrong item size");
  }
  const auto* offsets = static_cast<const OffsetT*>(offsets_info.ptr);
  if (start + count + 1 > static_cast<std::size_t>(offsets_info.size)) {
    throw std::invalid_argument("start/count exceed offsets buffer length");
  }
  const auto base = static_cast<std::uint64_t>(offsets[start]);
  for (std::size_t i = 0; i < count; ++i) {
    const auto raw_start = static_cast<std::uint64_t>(offsets[start + i]);
    const auto raw_end = static_cast<std::uint64_t>(offsets[start + i + 1]);
    if (raw_start > raw_end || raw_start < base || raw_end > data.size()) {
      throw std::invalid_argument("offsets must be sorted and within data");
    }
    fn(i, std::string_view(data.data() + raw_start, raw_end - raw_start));
  }
}

std::size_t ArrowCount(const py::buffer_info& offsets_info, std::size_t start,
                       const std::optional<std::size_t>& count) {
  if (offsets_info.size == 0) {
    throw std::invalid_argument("offsets buffer must not be empty");
  }
  if (start >= static_cast<std::size_t>(offsets_info.size)) {
    throw std::invalid_argument("start exceeds offsets buffer length");
  }
  if (count.has_value()) {
    return *count;
  }
  return static_cast<std::size_t>(offsets_info.size) - start - 1;
}

class PyUrlBloomFilter {
 public:
  static PyUrlBloomFilter create(const std::string& path,
                                 std::uint64_t expected_items,
                                 std::uint64_t size_bytes,
                                 std::uint32_t hash_count,
                                 std::uint64_t seed) {
    return PyUrlBloomFilter(url_bloom::UrlBloomFilter::Create(
        path, expected_items, size_bytes, hash_count, seed));
  }

  static PyUrlBloomFilter open(const std::string& path, bool read_only) {
    return PyUrlBloomFilter(url_bloom::UrlBloomFilter::Open(path, read_only));
  }

  void add_url(const std::string& url) { filter_.Add(url); }

  void add_urls(const std::vector<std::string>& urls) {
    py::gil_scoped_release release;
    filter_.AddUrls(urls);
  }

  void add_urls_offsets(const py::buffer& data, const py::sequence& offsets) {
    py::buffer_info data_info;
    const auto bytes = BufferAsBytes(data, &data_info);
    auto parsed_offsets = OffsetsFromSequence(offsets);
    py::gil_scoped_release release;
    filter_.AddUrlsFromOffsets(bytes, parsed_offsets);
  }

  void add_urls_arrow(const py::buffer& data, const py::buffer& offsets,
                      std::size_t start, std::optional<std::size_t> count) {
    py::buffer_info data_info;
    const auto bytes = BufferAsBytes(data, &data_info);
    const auto offsets_info = offsets.request();
    const auto actual_count = ArrowCount(offsets_info, start, count);
    py::gil_scoped_release release;
    if (offsets_info.itemsize == sizeof(std::int32_t)) {
      ForEachArrowValue<std::int32_t>(
          bytes, offsets_info, start, actual_count,
          [this](std::size_t, std::string_view url) { filter_.Add(url); });
    } else if (offsets_info.itemsize == sizeof(std::int64_t)) {
      ForEachArrowValue<std::int64_t>(
          bytes, offsets_info, start, actual_count,
          [this](std::size_t, std::string_view url) { filter_.Add(url); });
    } else {
      throw std::invalid_argument("Arrow offsets must be int32 or int64");
    }
  }

  bool contains_url(const std::string& url) const { return filter_.Contains(url); }

  py::bytes contains_urls_packed(const std::vector<std::string>& urls) const {
    std::vector<std::uint8_t> packed;
    {
      py::gil_scoped_release release;
      packed = filter_.ContainsUrlsPacked(urls);
    }
    return py::bytes(reinterpret_cast<const char*>(packed.data()), packed.size());
  }

  py::bytes contains_urls_packed_offsets(const py::buffer& data,
                                         const py::sequence& offsets) const {
    py::buffer_info data_info;
    const auto bytes = BufferAsBytes(data, &data_info);
    auto parsed_offsets = OffsetsFromSequence(offsets);
    std::vector<std::uint8_t> packed;
    {
      py::gil_scoped_release release;
      packed = filter_.ContainsUrlsPackedFromOffsets(bytes, parsed_offsets);
    }
    return py::bytes(reinterpret_cast<const char*>(packed.data()), packed.size());
  }

  py::bytes contains_urls_packed_arrow(
      const py::buffer& data, const py::buffer& offsets, std::size_t start,
      std::optional<std::size_t> count) const {
    py::buffer_info data_info;
    const auto bytes = BufferAsBytes(data, &data_info);
    const auto offsets_info = offsets.request();
    const auto actual_count = ArrowCount(offsets_info, start, count);
    std::vector<std::uint8_t> packed((actual_count + 7) / 8, 0);
    {
      py::gil_scoped_release release;
      auto set_if_contains = [this, &packed](std::size_t i,
                                             std::string_view url) {
        if (filter_.Contains(url)) {
          packed[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
      };
      if (offsets_info.itemsize == sizeof(std::int32_t)) {
        ForEachArrowValue<std::int32_t>(bytes, offsets_info, start, actual_count,
                                        set_if_contains);
      } else if (offsets_info.itemsize == sizeof(std::int64_t)) {
        ForEachArrowValue<std::int64_t>(bytes, offsets_info, start, actual_count,
                                        set_if_contains);
      } else {
        throw std::invalid_argument("Arrow offsets must be int32 or int64");
      }
    }
    return py::bytes(reinterpret_cast<const char*>(packed.data()), packed.size());
  }

  void merge_from(const std::string& other_path) {
    py::gil_scoped_release release;
    filter_.MergeFrom(other_path);
  }

  void flush() { filter_.Flush(); }

  url_bloom::BloomMetadata metadata() const { return filter_.metadata(); }

 private:
  explicit PyUrlBloomFilter(url_bloom::UrlBloomFilter filter)
      : filter_(std::move(filter)) {}

  url_bloom::UrlBloomFilter filter_;
};

}  // namespace

PYBIND11_MODULE(url_bloom_native, m) {
  m.doc() = "mmap-backed URL Bloom filter native extension";

  py::class_<url_bloom::BloomMetadata>(m, "BloomMetadata")
      .def_readonly("expected_items", &url_bloom::BloomMetadata::expected_items)
      .def_readonly("bit_count", &url_bloom::BloomMetadata::bit_count)
      .def_readonly("bit_bytes", &url_bloom::BloomMetadata::bit_bytes)
      .def_readonly("hash_count", &url_bloom::BloomMetadata::hash_count)
      .def_readonly("seed", &url_bloom::BloomMetadata::seed);

  py::class_<PyUrlBloomFilter>(m, "UrlBloomFilter")
      .def_static("create", &PyUrlBloomFilter::create,
                  py::arg("path"),
                  py::arg("expected_items") =
                      url_bloom::UrlBloomFilter::kDefaultExpectedItems,
                  py::arg("size_bytes") =
                      url_bloom::UrlBloomFilter::kDefaultSizeBytes,
                  py::arg("hash_count") = 0,
                  py::arg("seed") = url_bloom::UrlBloomFilter::kDefaultSeed)
      .def_static("open", &PyUrlBloomFilter::open, py::arg("path"),
                  py::arg("read_only") = true)
      .def("add_url", &PyUrlBloomFilter::add_url)
      .def("add_urls", &PyUrlBloomFilter::add_urls)
      .def("add_urls_offsets", &PyUrlBloomFilter::add_urls_offsets)
      .def("add_urls_arrow", &PyUrlBloomFilter::add_urls_arrow,
           py::arg("data"), py::arg("offsets"), py::arg("start") = 0,
           py::arg("count") = std::nullopt)
      .def("contains_url", &PyUrlBloomFilter::contains_url)
      .def("contains_urls_packed", &PyUrlBloomFilter::contains_urls_packed)
      .def("contains_urls_packed_offsets",
           &PyUrlBloomFilter::contains_urls_packed_offsets)
      .def("contains_urls_packed_arrow",
           &PyUrlBloomFilter::contains_urls_packed_arrow, py::arg("data"),
           py::arg("offsets"), py::arg("start") = 0,
           py::arg("count") = std::nullopt)
      .def("merge_from", &PyUrlBloomFilter::merge_from)
      .def("flush", &PyUrlBloomFilter::flush)
      .def_property_readonly("metadata", &PyUrlBloomFilter::metadata);

  m.def("optimal_hash_count", &url_bloom::optimal_hash_count);
  m.def("bits_for_bytes", &url_bloom::bits_for_bytes);
}
