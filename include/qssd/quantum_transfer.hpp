#pragma once
#include "rvpc2.hpp"
#include <string>
#include <sstream>

namespace qssd {
using Runtime = mundo_x::QuantumRuntime;
// Uses the original runtime's address model, not a host-sized address cast.
inline std::string address_offset(std::string address, unsigned offset) {
  address = mundo_x::infinite_detail::canonical_address(address, 4096);
  for (unsigned k = 0; k < offset; ++k) {
    std::size_t i = address.size();
    while (i && address[i - 1] == '9') address[--i] = '0';
    if (i) ++address[i - 1]; else address.insert(address.begin(), '1');
    if (address.size() > 4096) throw std::length_error("qSSD address budget exceeded");
  }
  return address;
}
// Existing MXIM1 view inspection: confirms that an explicit logical byte
// currently aliases the claimed eight-qubit source register. The binding is
// authoritative in InverseMemory; it is not inferred from a QSSD port name.
inline std::string bound_quantum_base(const Runtime& runtime, const std::string& logical_address) {
  const auto canonical = address_offset(logical_address, 0);
  const std::string prefix = "Q " + canonical + " ";
  std::istringstream snapshot(runtime.memory().snapshot());
  std::string line;
  while (std::getline(snapshot, line)) {
    if (line.compare(0, prefix.size(), prefix) == 0)
      return line.substr(prefix.size());
  }
  throw std::invalid_argument("QSSD logical byte is not bound to a quantum register");
}
struct BellReceipt { bool m0, m1; };
// Transactional wrapper around the original H/CX/Born measurement/X/Z protocol.
// Logical field aliases below do not call or impersonate this operation.
inline BellReceipt bell_transfer(Runtime& runtime, std::string src, std::string aux, std::string dst) {
  src = address_offset(src, 0); aux = address_offset(aux, 0); dst = address_offset(dst, 0);
  if (src == aux || src == dst || aux == dst) throw std::invalid_argument("Bell addresses must be distinct");
  if (runtime.core().z_determinism(aux) != 1 || runtime.core().z_determinism(dst) != 1)
    throw std::invalid_argument("Bell auxiliaries must be exactly |0>");
  auto next = runtime;
  next.execute("h " + aux + "\ncx " + aux + " " + dst + "\ncx " + src + " " + aux + "\nh " + src + "\n");
  auto measure = [&](const std::string& address) {
    const auto output = next.execute("measure " + address + "\n");
    const auto prefix = "{\"type\":\"measurement\",\"address\":\"" + address + "\",\"outcome_one\":";
    if (output.compare(0, prefix.size(), prefix) != 0) throw std::logic_error("XI measurement contract changed");
    if (output.compare(prefix.size(), 5, "true,") == 0) return true;
    if (output.compare(prefix.size(), 6, "false,") == 0) return false;
    throw std::logic_error("XI measurement outcome invalid");
  };
  const BellReceipt receipt{measure(src), measure(aux)};
  if (receipt.m1) next.execute("x " + dst + "\n");
  if (receipt.m0) next.execute("z " + dst + "\n");
  runtime = std::move(next);
  return receipt;
}
}  // namespace qssd
