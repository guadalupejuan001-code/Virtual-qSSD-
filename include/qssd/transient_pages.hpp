#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>

namespace qssd {

// Temporary QSSD backing for volatile pages. Fixed sparse offsets avoid
// append history and durability barriers. The caller keeps the current page
// resident until a full write succeeds and serializes access.
class TransientPages final {
 public:
  static constexpr std::uint64_t kPageSize = 4096;
  static constexpr std::uint64_t kStorageReserve = 1ULL << 30;
  static constexpr std::size_t kMaxWritePages = 16;

  struct PageWrite {
    std::uint64_t index;
    const std::uint8_t* bytes;
  };

  static std::uint64_t spill_budget(const std::filesystem::path& directory) {
    struct statvfs stats {};
    if (directory.empty() || ::statvfs(directory.c_str(), &stats) != 0) return 0;
    const auto available = available_bytes(stats);
    return available >= kStorageReserve + write_batch_bytes(stats) ?
        (available - kStorageReserve) / kPageSize * kPageSize : 0;
  }

  TransientPages(const std::filesystem::path& directory, std::uint64_t logical_bytes,
                 std::uint64_t spill_bytes)
      : page_count_(checked_page_count(logical_bytes)),
        slot_limit_(std::min(spill_bytes / kPageSize, page_count_)),
        written_((page_count_ + kPagesPerBitmap - 1) / kPagesPerBitmap) {
    if (!slot_limit_)
      throw std::invalid_argument("Tamaño inválido para el respaldo de RAM.");
    const auto file_template = (directory / "reverse-ram-XXXXXX").string();
    std::vector<char> name(file_template.begin(), file_template.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) throw_io("Crear respaldo temporal de RAM");
    // Delete the directory entry before any guest bytes enter the file. The
    // kernel releases the file when its last fd closes, including after a crash.
    int unlinked;
    do {
      unlinked = ::unlink(name.data());
    } while (unlinked != 0 && errno == EINTR);
    if (unlinked != 0) {
      const auto error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "Desvincular respaldo temporal de RAM");
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      const auto error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "Proteger descriptor de la RAM");
    }
    fd_ = fd;
  }

  TransientPages(const TransientPages&) = delete;
  TransientPages& operator=(const TransientPages&) = delete;
  ~TransientPages() { ::close(fd_); }

  void read(std::uint64_t page_index, std::uint8_t* page) {
    read_part(page_index, 0, page, kPageSize);
  }

  void read_part(std::uint64_t page_index, std::size_t start, std::uint8_t* target,
                 std::size_t size) {
    check_index(page_index);
    check_part(start, size);
    if (!contains(page_index)) throw std::runtime_error("Página ausente en el respaldo de RAM.");
    const auto started = std::chrono::steady_clock::now();
    std::size_t done = 0;
    while (done < size) {
      const auto bytes = ::pread(fd_, target + done, size - done, offset(page_index, start + done));
      if (bytes < 0 && errno == EINTR) continue;
      if (bytes < 0) throw_io("Leer respaldo de RAM");
      if (bytes == 0) throw std::runtime_error("Lectura incompleta del respaldo de RAM.");
      done += static_cast<std::size_t>(bytes);
    }
    read_time_ += std::chrono::steady_clock::now() - started;
  }

  void write(std::uint64_t page_index, const std::uint8_t* page) {
    write_part(page_index, 0, page, kPageSize);
  }

  // The caller still owns every input page (including old slots it has
  // superseded) until this batch returns. Publish new slots after all runs.
  void write_pages(const PageWrite* pages, std::size_t count) {
    if (!count) return;
    if (!pages || count > kMaxWritePages)
      throw std::invalid_argument("Lote de escritura de RAM inválido.");
    std::array<PartWrite, kMaxWritePages> parts;
    for (std::size_t i = 0; i < count; ++i)
      parts[i] = {pages[i].index, 0, pages[i].bytes, kPageSize};
    write_parts(parts, count);
  }

  void write_part(std::uint64_t page_index, std::size_t start, const std::uint8_t* source,
                  std::size_t size) {
    check_index(page_index);
    check_part(start, size);
    const auto started = std::chrono::steady_clock::now();
    auto& bitmap = written_[page_index / kPagesPerBitmap];
    const auto index = page_index % kPagesPerBitmap;
    const auto bit = std::uint64_t{1} << (index % 64);
    const bool new_slot = !bitmap || !((*bitmap)[index / 64] & bit);
    if (new_slot && committed_slots_ >= slot_limit_)
      throw std::runtime_error("Se agotó el espacio asignado al respaldo temporal de RAM.");
    check_allocation(new_slot);
    if (!bitmap) bitmap = std::make_unique<WrittenBitmap>();

    iovec part{const_cast<std::uint8_t*>(source), size};
    try {
      write_contiguous(&part, 1, offset(page_index, start));
    } catch (...) {
      if (new_slot) slots_until_space_check_ = 0;
      throw;
    }
    if (new_slot) {
      (*bitmap)[index / 64] |= bit;
      ++committed_slots_;
      --slots_until_space_check_;
    }
    write_time_ += std::chrono::steady_clock::now() - started;
  }

  void reset() noexcept {
    // The in-memory index belongs to this session only. Clear it even if the
    // filesystem cannot release the old blocks.
    int result;
    do {
      result = ::ftruncate(fd_, 0);
    } while (result != 0 && errno == EINTR);
    for (auto& bitmap : written_) bitmap.reset();
    committed_slots_ = 0;
    write_calls_ = 0;
    slots_until_space_check_ = 0;
    read_time_ = write_time_ = {};
  }

  std::uint64_t file_payload_bytes() const noexcept { return committed_slots_ * kPageSize; }
  std::uint64_t read_us() const noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(read_time_).count();
  }
  std::uint64_t write_us() const noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(write_time_).count();
  }
  std::uint64_t write_calls() const noexcept { return write_calls_; }

 private:
  friend class PackedTransientPages;

  struct PartWrite {
    std::uint64_t index;
    std::size_t start;
    const std::uint8_t* bytes;
    std::size_t size;
  };

  // Keep the free-space check off the eviction hot path and leave room for
  // the following batch of writes, including larger filesystem fragments.
  static constexpr std::uint64_t kSpaceCheckInterval = 256;
  static constexpr std::size_t kPagesPerBitmap = 4096;
  using WrittenBitmap = std::array<std::uint64_t, kPagesPerBitmap / 64>;

  static std::uint64_t checked_page_count(std::uint64_t bytes) {
    constexpr auto max_bytes = std::uint64_t(std::numeric_limits<off_t>::max()) /
        kPageSize * kPageSize;
    if (!bytes || bytes > max_bytes)
      throw std::invalid_argument("Tamaño inválido para el respaldo de RAM.");
    return bytes / kPageSize + (bytes % kPageSize != 0);
  }

  static std::uint64_t fragment_bytes(const struct statvfs& stats) noexcept {
    return stats.f_frsize ? stats.f_frsize : stats.f_bsize;
  }

  static std::uint64_t available_bytes(const struct statvfs& stats) {
    const auto unit = fragment_bytes(stats);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return unit && stats.f_bavail > max / unit ? max : std::uint64_t(stats.f_bavail) * unit;
  }

  static std::uint64_t write_batch_bytes(const struct statvfs& stats) noexcept {
    return std::max(fragment_bytes(stats), kPageSize) * kSpaceCheckInterval;
  }

  static off_t offset(std::uint64_t index, std::size_t in_page) {
    return static_cast<off_t>(index * kPageSize + in_page);
  }

  [[noreturn]] static void throw_io(const char* operation) {
    const auto error = errno;
    throw std::system_error(error, std::generic_category(), operation);
  }

  void check_index(std::uint64_t index) const {
    if (index >= page_count_) throw std::out_of_range("Página de respaldo fuera de la RAM invitada.");
  }

  static void check_part(std::size_t start, std::size_t size) {
    if (start >= kPageSize || !size || size > kPageSize - start)
      throw std::out_of_range("Fragmento fuera de la página de respaldo de RAM.");
  }

  bool contains(std::uint64_t index) const {
    const auto& bitmap = written_[index / kPagesPerBitmap];
    const auto in_bitmap = index % kPagesPerBitmap;
    return bitmap && ((*bitmap)[in_bitmap / 64] & (std::uint64_t{1} << (in_bitmap % 64)));
  }

  void write_parts(std::array<PartWrite, kMaxWritePages>& parts, std::size_t count) {
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
      check_index(parts[i].index);
      check_part(parts[i].start, parts[i].size);
      if (!parts[i].bytes) throw std::invalid_argument("Lote de escritura de RAM inválido.");
    }
    std::sort(parts.begin(), parts.begin() + count, [](const auto& left, const auto& right) {
      return offset(left.index, left.start) < offset(right.index, right.start);
    });

    std::size_t new_slots = 0;
    off_t previous_end = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& part = parts[i];
      const auto start = offset(part.index, part.start);
      if (start < previous_end)
        throw std::invalid_argument("Lote de escritura de RAM superpuesto.");
      previous_end = start + static_cast<off_t>(part.size);
      if (i != 0 && part.index == parts[i - 1].index) continue;
      if (!contains(part.index)) ++new_slots;
    }
    if (new_slots > slot_limit_ - committed_slots_)
      throw std::runtime_error("Se agotó el espacio asignado al respaldo temporal de RAM.");
    check_allocation(new_slots);
    for (std::size_t i = 0; i < count; ++i) {
      auto& bitmap = written_[parts[i].index / kPagesPerBitmap];
      if (!bitmap) bitmap = std::make_unique<WrittenBitmap>();
    }

    try {
      std::array<iovec, kMaxWritePages> run;
      for (std::size_t first = 0; first < count;) {
        const auto start = offset(parts[first].index, parts[first].start);
        auto end = start;
        auto last = first;
        while (last < count && offset(parts[last].index, parts[last].start) == end) {
          const auto& part = parts[last];
          run[last - first] = {const_cast<std::uint8_t*>(part.bytes), part.size};
          end += static_cast<off_t>(part.size);
          ++last;
        }
        write_contiguous(run.data(), last - first, start);
        first = last;
      }
    } catch (...) {
      if (new_slots) slots_until_space_check_ = 0;
      throw;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto page = parts[i].index;
      const auto bit = page % kPagesPerBitmap;
      (*written_[page / kPagesPerBitmap])[bit / 64] |= std::uint64_t{1} << (bit % 64);
    }
    committed_slots_ += new_slots;
    slots_until_space_check_ -= new_slots;
    write_time_ += std::chrono::steady_clock::now() - started;
  }

  void write_contiguous(iovec* parts, std::size_t count, off_t at) {
    while (count) {
      ++write_calls_;
      const auto bytes = count == 1 ? ::pwrite(fd_, parts->iov_base, parts->iov_len, at)
                                   : ::pwritev(fd_, parts, static_cast<int>(count), at);
      if (bytes < 0 && errno == EINTR) continue;
      if (bytes < 0) throw_io("Escribir respaldo de RAM");
      if (bytes == 0) throw std::runtime_error("Escritura incompleta del respaldo de RAM.");
      at += bytes;
      auto written = static_cast<std::size_t>(bytes);
      while (count && written >= parts->iov_len) {
        written -= parts->iov_len;
        ++parts;
        --count;
      }
      if (count && written) {
        parts->iov_base = static_cast<std::uint8_t*>(parts->iov_base) + written;
        parts->iov_len -= written;
      }
    }
  }

  void check_allocation(std::size_t new_slots) {
    if (new_slots > slots_until_space_check_) {
      check_storage();
      slots_until_space_check_ = kSpaceCheckInterval;
    }
  }

  void check_storage() const {
    struct statvfs stats {};
    if (::fstatvfs(fd_, &stats) != 0) throw_io("Consultar almacenamiento para la RAM");
    if (available_bytes(stats) < kStorageReserve + write_batch_bytes(stats))
      throw std::runtime_error("Android tiene poco espacio para continuar el respaldo de RAM.");
  }

  const std::uint64_t page_count_;
  const std::uint64_t slot_limit_;
  std::vector<std::unique_ptr<WrittenBitmap>> written_;
  std::uint64_t committed_slots_ = 0;
  std::uint64_t write_calls_ = 0;
  std::size_t slots_until_space_check_ = 0;
  std::chrono::steady_clock::duration read_time_{}, write_time_{};
  int fd_ = -1;
};

} // namespace qssd
