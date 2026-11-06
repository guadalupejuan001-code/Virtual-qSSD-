#pragma once
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace qssd {
inline std::string read_file(const std::filesystem::path& path, std::size_t cap = std::numeric_limits<std::size_t>::max()) {
  const auto size = std::filesystem::file_size(path);
  if (size > cap || size > std::string().max_size() || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
    throw std::length_error("qSSD file size cannot be read with the selected capacity");
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read qSSD file");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (static_cast<std::size_t>(in.gcount()) != bytes.size() || in.peek() != EOF)
    throw std::runtime_error("qSSD file changed/truncated");
  return bytes;
}
// Explicit POSIX checkpoint. A new name is published only after its complete
// bytes reach fsync; an existing name is never overwritten by this interface.
inline void save_new(const std::filesystem::path& path, const std::string& bytes) {
  auto pattern = path.string() + ".pending.XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
  int fd = ::mkstemp(name.data());
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "qSSD temporary");
  bool published = false;
  try {
    for (std::size_t at = 0; at < bytes.size();) {
      const auto n = ::write(fd, bytes.data() + at, bytes.size() - at);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::system_error(n < 0 ? errno : EIO, std::generic_category(), "qSSD checkpoint write");
      at += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) throw std::system_error(errno, std::generic_category(), "qSSD checkpoint sync");
    const auto closing = fd; fd = -1;
    if (::close(closing) != 0) throw std::system_error(errno, std::generic_category(), "qSSD checkpoint close");
    std::filesystem::create_hard_link(name.data(), path); published = true;
    std::filesystem::remove(name.data());
    auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    int dir = ::open(parent.c_str(), O_DIRECTORY | O_RDONLY);
    if (dir < 0) throw std::system_error(errno, std::generic_category(), "qSSD published; directory sync unavailable");
    const auto sync = ::fsync(dir), error = errno; ::close(dir);
    if (sync != 0) throw std::system_error(error, std::generic_category(), "qSSD published; durability uncertain");
  } catch (...) {
    if (fd >= 0) ::close(fd);
    if (!published) { std::error_code ignored; std::filesystem::remove(name.data(), ignored); }
    throw;
  }
}
}  // namespace qssd
