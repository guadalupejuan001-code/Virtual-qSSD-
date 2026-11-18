#pragma once

#include "transient_pages.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace qssd {

// A guest page occupies a full block or one half of it. Both forms share
// the same storage budget. Released blocks can serve any guest address.
class PackedTransientPages final {
 public:
  static constexpr auto kPageSize = TransientPages::kPageSize;
  static constexpr auto kHalfPageSize = kPageSize / 2;
  static constexpr auto kMaxWritePages = TransientPages::kMaxWritePages;

  struct PageWrite {
    std::uint64_t index;
    const std::uint8_t* bytes;
    bool packed;
  };

  class CapacityExceeded : public std::runtime_error {
   public:
    CapacityExceeded() : std::runtime_error("Se agotó el espacio asignado al respaldo temporal de RAM.") {}
  };

  PackedTransientPages(const std::filesystem::path& directory, std::uint64_t logical_bytes,
                       std::uint64_t storage_bytes)
      : page_count_(logical_bytes / kPageSize + (logical_bytes % kPageSize != 0)),
        block_limit_(checked_limit(page_count_, storage_bytes)),
        disk_(directory, block_limit_ * kPageSize, block_limit_ * kPageSize),
        locations_((page_count_ + kPagesPerTable - 1) / kPagesPerTable) {}

  bool read(std::uint64_t index, std::uint8_t* full, std::uint8_t* packed) {
    check_index(index);
    const auto* table = locations_[index / kPagesPerTable].get();
    const auto slot = table ? (*table)[index % kPagesPerTable] : Slot{};
    if (!slot) throw std::runtime_error("Página ausente en el respaldo de RAM.");
    disk_.read_part(slot.block(), slot.offset(), slot.packed() ? packed : full, slot.size());
    return slot.packed();
  }

  void write(std::uint64_t index, const std::uint8_t* bytes, bool packed) {
    check_index(index);
    auto& table = locations_[index / kPagesPerTable];
    if (!table) table = std::make_unique<LocationTable>();
    auto& location = (*table)[index % kPagesPerTable];
    const auto previous = location;
    auto next = previous;
    if (!previous || previous.packed() != packed) {
      next = previous && blocks_[previous.block()].occupied == previous.mask()
          ? Slot(previous.block(), packed ? kFirstHalf : kFull)
          : free_slot(packed);
    }

    // The caller's page is authoritative until this write succeeds.
    disk_.write_part(next.block(), next.offset(), bytes, next.size());
    if (previous.code == next.code) return;

    if (previous && previous.block() == next.block()) {
      occupy(next.block(), next.mask());
    } else {
      occupy(next.block(), blocks_[next.block()].occupied | next.mask());
      if (previous) release(previous);
    }
    if (previous && previous.packed()) --packed_pages_;
    if (packed) ++packed_pages_;
    location = next;
  }

  // Publish only fresh guest locations. A failed run can write to reserved
  // halves without clobbering any existing authoritative page.
  void write_pages(const PageWrite* pages, std::size_t count) {
    if (!count) return;
    if (!pages || count > kMaxWritePages)
      throw std::invalid_argument("Lote de escritura de RAM inválido.");
    std::array<Slot*, kMaxWritePages> locations;
    for (std::size_t i = 0; i < count; ++i) {
      const auto index = pages[i].index;
      check_index(index);
      auto& table = locations_[index / kPagesPerTable];
      if (!table) table = std::make_unique<LocationTable>();
      auto& location = (*table)[index % kPagesPerTable];
      const auto prefix_end = locations.begin() + i;
      if (location || std::find(locations.begin(), prefix_end, &location) != prefix_end)
        throw std::invalid_argument("El lote de respaldo requiere páginas sin ubicación previa.");
      locations[i] = &location;
    }

    std::array<Slot, kMaxWritePages> slots;
    std::array<TransientPages::PartWrite, kMaxWritePages> parts;
    std::size_t reserved = 0;
    try {
      for (; reserved < count; ++reserved) {
        const auto& page = pages[reserved];
        const auto slot = free_slot(page.packed);
        occupy(slot.block(), blocks_[slot.block()].occupied | slot.mask());
        slots[reserved] = slot;
        parts[reserved] = {slot.block(), slot.offset(), page.bytes, slot.size()};
      }
      disk_.write_parts(parts, count);
    } catch (...) {
      while (reserved) release(slots[--reserved]);
      throw;
    }
    for (std::size_t i = 0; i < count; ++i) {
      *locations[i] = slots[i];
      packed_pages_ += slots[i].packed();
    }
  }

  // Check whole blocks only. Walking the free list stops once the requested
  // capacity is known. The owner can include a relocation reserve in count.
  bool room_for_raw(std::size_t count) const noexcept {
    if (count > block_limit_) return false;
    auto available = block_limit_ - blocks_.size();
    auto block = free_;
    while (available < count && block != kNone) {
      ++available;
      block = blocks_[block].next;
    }
    return available >= count;
  }

  // Relocate to an unoccupied half, then retire the raw source. The owner
  // must serialize calls and validate that bytes still describe this page.
  bool pack_raw_copy(std::uint64_t index, const std::uint8_t* bytes) {
    check_index(index);
    auto& table = locations_[index / kPagesPerTable];
    if (!table) return false;
    auto& location = (*table)[index % kPagesPerTable];
    const auto previous = location;
    if (!previous || previous.packed()) return false;
    if (!bytes) throw std::invalid_argument("Copia compacta de RAM inválida.");

    // A live raw block is absent from both free lists. Partial I/O may
    // alter the destination, but never the authoritative raw page.
    const auto next = free_slot(true);
    disk_.write_part(next.block(), next.offset(), bytes, next.size());
    occupy(next.block(), blocks_[next.block()].occupied | next.mask());
    location = next;
    release(previous);
    ++packed_pages_;
    return true;
  }

  void discard(std::uint64_t index) noexcept {
    if (index >= page_count_) return;
    auto& table = locations_[index / kPagesPerTable];
    if (!table) return;
    auto& slot = (*table)[index % kPagesPerTable];
    if (!slot) return;
    release(slot);
    if (slot.packed()) --packed_pages_;
    slot = {};
  }

  void reset() noexcept {
    disk_.reset();
    for (auto& table : locations_) table.reset();
    blocks_.clear();
    free_ = partial_ = kNone;
    packed_pages_ = 0;
  }

  std::uint64_t file_payload_bytes() const noexcept { return disk_.file_payload_bytes(); }
  std::uint64_t packed_pages() const noexcept { return packed_pages_; }
  std::uint64_t read_us() const noexcept { return disk_.read_us(); }
  std::uint64_t write_us() const noexcept { return disk_.write_us(); }
  std::uint64_t write_calls() const noexcept { return disk_.write_calls(); }

 private:
  static constexpr std::uint32_t kFirstHalf = 1, kSecondHalf = 2, kFull = 3;
  static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::size_t kPagesPerTable = 4096;

  struct Slot {
    std::uint32_t code = 0;
    Slot() = default;
    Slot(std::uint32_t block, std::uint32_t mask) : code((block + 1) * 4 + mask) {}
    explicit operator bool() const noexcept { return code != 0; }
    std::uint32_t block() const noexcept { return code / 4 - 1; }
    std::uint32_t mask() const noexcept { return code % 4; }
    bool packed() const noexcept { return mask() != kFull; }
    std::size_t offset() const noexcept { return mask() == kSecondHalf ? kHalfPageSize : 0; }
    std::size_t size() const noexcept { return packed() ? kHalfPageSize : kPageSize; }
  };

  // Two lists track empty blocks and blocks with one free half. Linking in
  // the blocks themselves keeps publication and discard allocation-free.
  struct Block {
    std::uint32_t previous = kNone, next = kNone;
    std::uint32_t occupied = 0;
  };

  using LocationTable = std::array<Slot, kPagesPerTable>;

  static std::uint64_t checked_limit(std::uint64_t pages, std::uint64_t bytes) {
    const auto limit = std::min(pages, bytes / kPageSize);
    if (!limit || pages > kNone / 4)
      throw std::invalid_argument("Tamaño inválido para el respaldo compacto de RAM.");
    return limit;
  }

  void check_index(std::uint64_t index) const {
    if (index >= page_count_) throw std::out_of_range("Página de respaldo fuera de la RAM invitada.");
  }

  std::uint32_t* available_list(std::uint32_t occupied) noexcept {
    if (occupied == kFull) return nullptr;
    return occupied == 0 ? &free_ : &partial_;
  }

  void link_available(std::uint32_t id) noexcept {
    auto& block = blocks_[id];
    auto* head = available_list(block.occupied);
    if (!head) return;
    block.next = *head;
    if (*head != kNone) blocks_[*head].previous = id;
    *head = id;
  }

  void occupy(std::uint32_t id, std::uint32_t mask) noexcept {
    auto& block = blocks_[id];
    if (auto* head = available_list(block.occupied)) {
      if (block.previous == kNone) *head = block.next;
      else blocks_[block.previous].next = block.next;
      if (block.next != kNone) blocks_[block.next].previous = block.previous;
    }
    block = {kNone, kNone, mask};
    link_available(id);
  }

  void release(Slot slot) noexcept {
    occupy(slot.block(), blocks_[slot.block()].occupied & ~slot.mask());
  }

  Slot free_slot(bool packed) {
    if (packed && partial_ != kNone)
      return Slot(partial_, kFull ^ blocks_[partial_].occupied);
    if (free_ == kNone) {
      if (blocks_.size() == block_limit_) throw CapacityExceeded();
      const auto id = static_cast<std::uint32_t>(blocks_.size());
      blocks_.emplace_back();
      link_available(id);
    }
    return Slot(free_, packed ? kFirstHalf : kFull);
  }

  const std::uint64_t page_count_, block_limit_;
  TransientPages disk_;
  std::vector<std::unique_ptr<LocationTable>> locations_;
  std::deque<Block> blocks_;
  std::uint32_t free_ = kNone, partial_ = kNone;
  std::uint64_t packed_pages_ = 0;
};

} // namespace qssd
