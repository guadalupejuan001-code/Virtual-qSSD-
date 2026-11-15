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
#include <unistd.h>

namespace qssd {

// Temporary QSSD backing for volatile pages. Fixed sparse offsets avoid
// append history and durability barriers. The caller keeps the current page
// resident until a full write succeeds and serializes access.
class TransientPages final {
 public:
  static constexpr std::uint64_t kPageSize = 4096;
  static constexpr std::uint64_t kStorageReserve = 1ULL << 30;

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
    check_index(page_index);
    if (!contains(page_index)) throw std::runtime_error("Página ausente en el respaldo de RAM.");
    const auto started = std::chrono::steady_clock::now();
    std::size_t done = 0;
    while (done < kPageSize) {
      const auto bytes = ::pread(fd_, page + done, kPageSize - done, offset(page_index, done));
      if (bytes < 0 && errno == EINTR) continue;
      if (bytes < 0) throw_io("Leer respaldo de RAM");
      if (bytes == 0) throw std::runtime_error("Lectura incompleta del respaldo de RAM.");
      done += static_cast<std::size_t>(bytes);
    }
    read_time_ += std::chrono::steady_clock::now() - started;
  }

  void write(std::uint64_t page_index, const std::uint8_t* page) {
    check_index(page_index);
    const auto started = std::chrono::steady_clock::now();
    auto& bitmap = written_[page_index / kPagesPerBitmap];
    const auto index = page_index % kPagesPerBitmap;
    const auto bit = std::uint64_t{1} << (index % 64);
    const bool new_slot = !bitmap || !((*bitmap)[index / 64] & bit);
    if (new_slot && committed_slots_ >= slot_limit_)
      throw std::runtime_error("Se agotó el espacio asignado al respaldo temporal de RAM.");
    if (new_slot && (committed_slots_ % kSpaceCheckInterval == 0)) check_storage();
    if (!bitmap) bitmap = std::make_unique<WrittenBitmap>();

    std::size_t done = 0;
    while (done < kPageSize) {
      const auto bytes = ::pwrite(fd_, page + done, kPageSize - done, offset(page_index, done));
      if (bytes < 0 && errno == EINTR) continue;
      if (bytes < 0) throw_io("Escribir respaldo de RAM");
      if (bytes == 0) throw std::runtime_error("Escritura incompleta del respaldo de RAM.");
      done += static_cast<std::size_t>(bytes);
    }
    if (new_slot) {
      (*bitmap)[index / 64] |= bit;
      ++committed_slots_;
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
    read_time_ = write_time_ = {};
  }

  std::uint64_t file_payload_bytes() const noexcept { return committed_slots_ * kPageSize; }
  std::uint64_t read_us() const noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(read_time_).count();
  }
  std::uint64_t write_us() const noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(write_time_).count();
  }

 private:
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
    throw std::system_error(errno, std::generic_category(), operation);
  }

  void check_index(std::uint64_t index) const {
    if (index >= page_count_) throw std::out_of_range("Página de respaldo fuera de la RAM invitada.");
  }

  bool contains(std::uint64_t index) const {
    const auto& bitmap = written_[index / kPagesPerBitmap];
    const auto in_bitmap = index % kPagesPerBitmap;
    return bitmap && ((*bitmap)[in_bitmap / 64] & (std::uint64_t{1} << (in_bitmap % 64)));
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
  std::chrono::steady_clock::duration read_time_{}, write_time_{};
  int fd_ = -1;
};

} // namespace qssd
