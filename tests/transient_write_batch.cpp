#include <qssd/packed_transient_pages.hpp>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sys/resource.h>

namespace {

using qssd::PackedTransientPages;
using qssd::TransientPages;
using Page = std::array<std::uint8_t, TransientPages::kPageSize>;
constexpr auto kPageSize = TransientPages::kPageSize;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Error, typename Action>
void expect_error(Action action, const char* message) {
  try {
    action();
  } catch (const Error&) {
    return;
  }
  throw std::runtime_error(message);
}

class FileSizeLimit {
 public:
  explicit FileSizeLimit(rlim_t bytes) {
    require(::getrlimit(RLIMIT_FSIZE, &previous_) == 0, "cannot read file size limit");
    auto next = previous_;
    next.rlim_cur = bytes;
    signal_ = std::signal(SIGXFSZ, SIG_IGN);
    require(signal_ != SIG_ERR, "cannot ignore file size signal");
    require(::setrlimit(RLIMIT_FSIZE, &next) == 0, "cannot restrict file size");
  }
  ~FileSizeLimit() {
    ::setrlimit(RLIMIT_FSIZE, &previous_);
    std::signal(SIGXFSZ, signal_);
  }

 private:
  rlimit previous_{};
  void (*signal_)(int) = nullptr;
};

void raw_batch(const std::filesystem::path& directory, const Page& first, const Page& second) {
  TransientPages backing(directory, 8 * kPageSize, 4 * kPageSize);
  const std::array writes{
    TransientPages::PageWrite{2, first.data()},
    TransientPages::PageWrite{1, second.data()},
    TransientPages::PageWrite{0, first.data()}
  };
  Page actual;
  {
    const FileSizeLimit limit(kPageSize + 512);
    expect_error<std::system_error>([&] { backing.write_pages(writes.data(), writes.size()); },
                                    "raw batch survived the filesystem write limit");
  }
  require(backing.file_payload_bytes() == 0, "failed raw batch published slots");
  expect_error<std::runtime_error>([&] { backing.read(0, actual.data()); },
                                   "a partially written raw batch became readable");
  const auto before = backing.write_calls();
  backing.write_pages(writes.data(), writes.size());
  require(backing.write_calls() == before + 1, "adjacent raw pages did not share one call");
  for (const auto& write : writes) {
    backing.read(write.index, actual.data());
    require(std::equal(actual.begin(), actual.end(), write.bytes), "raw batch changed bytes");
  }

  const std::array other{
    TransientPages::PageWrite{0, second.data()},
    TransientPages::PageWrite{7, first.data()}
  };
  backing.write_pages(other.data(), other.size());
  require(backing.write_calls() == before + 3, "disjoint raw pages did not use separate calls");
  require(backing.file_payload_bytes() == 4 * kPageSize, "rewrote raw slot consumed extra space");
  backing.read(0, actual.data());
  require(actual == second, "rewrote raw slot retained its stale contents");
}

void packed_batch(const std::filesystem::path& directory, const Page& first, const Page& second) {
  constexpr auto half = PackedTransientPages::kHalfPageSize;
  PackedTransientPages backing(directory, 8 * kPageSize, 3 * kPageSize);
  backing.write(0, first.data(), true);
  const std::array writes{
    PackedTransientPages::PageWrite{1, second.data(), true},
    PackedTransientPages::PageWrite{3, first.data(), false},
    PackedTransientPages::PageWrite{2, second.data(), false}
  };
  std::array<std::uint8_t, half> packed;
  Page full;
  const std::array overflow{
    writes[0], writes[1], writes[2],
    PackedTransientPages::PageWrite{4, first.data(), false}
  };
  expect_error<PackedTransientPages::CapacityExceeded>(
      [&] { backing.write_pages(overflow.data(), overflow.size()); },
      "packed batch exceeded its block budget");
  require(backing.write_calls() == 1, "excessive batch issued any file writes");
  {
    const FileSizeLimit limit(kPageSize + 512);
    expect_error<std::system_error>([&] { backing.write_pages(writes.data(), writes.size()); },
                                    "packed batch survived the filesystem write limit");
  }
  require(backing.file_payload_bytes() == kPageSize, "failed packed batch published new blocks");
  require(backing.packed_pages() == 1, "failed packed batch published a new half");
  for (const auto& write : writes)
    expect_error<std::runtime_error>([&] { backing.read(write.index, full.data(), packed.data()); },
                                     "failed packed batch published a location");
  require(backing.read(0, full.data(), packed.data()) &&
          std::equal(packed.begin(), packed.end(), first.begin()),
          "failed packed batch damaged the neighboring live half");

  const auto before = backing.write_calls();
  backing.write_pages(writes.data(), writes.size());
  require(backing.write_calls() == before + 1, "adjacent full and half blocks did not share one call");
  require(backing.file_payload_bytes() == 3 * kPageSize && backing.packed_pages() == 2,
          "retry did not reuse every uncommitted block");
  for (const auto& write : writes) {
    const auto compressed = backing.read(write.index, full.data(), packed.data());
    require(compressed == write.packed, "packed batch published the wrong representation");
    const auto* bytes = compressed ? packed.data() : full.data();
    require(std::equal(bytes, bytes + (compressed ? half : kPageSize), write.bytes),
            "packed batch changed bytes");
  }
  expect_error<std::invalid_argument>([&] { backing.write_pages(writes.data(), writes.size()); },
                                      "batch accepted already authoritative locations");
  require(backing.write_calls() == before + 1, "invalid batch issued any file writes");
}

void packed_relocation(const std::filesystem::path& directory, const Page& first, const Page& second) {
  PackedTransientPages backing(directory, 8 * kPageSize, 2 * kPageSize);
  std::array<std::uint8_t, PackedTransientPages::kHalfPageSize> packed;
  Page full;
  require(backing.room_for_raw(2) && !backing.room_for_raw(3), "raw room ignored the block quota");
  const PackedTransientPages::PageWrite staged{0, first.data(), false};
  backing.write_pages(&staged, 1);
  require(backing.room_for_raw(1) && !backing.room_for_raw(2), "raw staging lost its relocation reserve");
  require(!backing.pack_raw_copy(1, second.data()), "a missing raw source was relocated");
  {
    const FileSizeLimit limit(kPageSize + 512);
    expect_error<std::system_error>([&] { backing.pack_raw_copy(0, second.data()); },
                                    "relocation into a new block survived a short write");
  }
  require(!backing.read(0, full.data(), packed.data()) && full == first,
          "failed relocation damaged the original raw bytes");
  require(backing.file_payload_bytes() == kPageSize && backing.packed_pages() == 0 &&
          backing.room_for_raw(1) && !backing.room_for_raw(2),
          "failed relocation published or consumed its reserved block");
  require(backing.pack_raw_copy(0, second.data()) && backing.read(0, full.data(), packed.data()) &&
          std::equal(packed.begin(), packed.end(), second.begin()),
          "relocation retry did not publish the packed bytes");
  require(!backing.pack_raw_copy(0, first.data()), "relocation replaced an already compacted page");

  backing.write(1, second.data(), false);
  require(backing.room_for_raw(0) && !backing.room_for_raw(1), "a free half counted as a raw block");
  {
    const FileSizeLimit limit(2 * kPageSize - 512);
    expect_error<std::system_error>([&] { backing.pack_raw_copy(1, first.data()); },
                                    "relocation into a neighboring half survived a short write");
  }
  require(!backing.read(1, full.data(), packed.data()) && full == second && backing.packed_pages() == 1,
          "failed half relocation retired the raw page");
  require(backing.read(0, full.data(), packed.data()) && std::equal(packed.begin(), packed.end(), second.begin()),
          "failed half relocation damaged the neighboring packed bytes");
  require(backing.pack_raw_copy(1, first.data()) && backing.read(1, full.data(), packed.data()) &&
          std::equal(packed.begin(), packed.end(), first.begin()) && backing.room_for_raw(1),
          "relocation into the free half did not release its whole raw block");
  backing.write(2, first.data(), false);
  require(backing.file_payload_bytes() == 2 * kPageSize && backing.packed_pages() == 2 &&
          !backing.room_for_raw(1), "relocation exceeded or lost the original block quota");
  expect_error<PackedTransientPages::CapacityExceeded>([&] { backing.pack_raw_copy(2, second.data()); },
                                                       "relocation exceeded a full allocation");
  require(!backing.read(2, full.data(), packed.data()) && full == first,
          "capacity failure changed the source bytes");
  backing.discard(1);
  require(!backing.room_for_raw(1), "discarding one half counted as a whole block");
  backing.discard(0);
  require(backing.room_for_raw(1), "discarding the last half did not free a whole block");
  backing.reset();
  require(backing.room_for_raw(2) && backing.packed_pages() == 0 && backing.file_payload_bytes() == 0,
          "reset did not restore the original allocation geometry");
}

} // namespace

int main() {
  try {
    Page first, second;
    for (std::size_t i = 0; i < first.size(); ++i) {
      first[i] = static_cast<std::uint8_t>(i * 19 + i / 17);
      second[i] = static_cast<std::uint8_t>(i * 23 - i / 13);
    }
    const auto directory = std::filesystem::temp_directory_path();
    raw_batch(directory, first, second);
    packed_batch(directory, first, second);
    packed_relocation(directory, first, second);
    std::cout << "Grouped RAM writes and packed relocations preserve authoritative bytes.\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
