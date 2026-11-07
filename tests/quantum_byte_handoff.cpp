#include "qssd/state_space.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Term = mundo_x::InfiniteCore::Term;
std::size_t checks = 0;
void need(bool condition, const char* failure) {
  ++checks;
  if (!condition) throw std::runtime_error(failure);
}
template<class F> void must_reject(F action, const qssd::Space& space) {
  const auto previous = space.checkpoint();
  const auto revision = space.epoch()->revision;
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  need(rejected, "invalid Bell handoff accepted");
  need(space.epoch()->revision == revision && space.checkpoint() == previous,
       "failed Bell handoff partially committed");
}
void same_pauli(const qssd::Runtime& original, const qssd::Runtime& destination,
                const std::vector<Term>& before, const std::vector<Term>& after) {
  const double lhs = original.core().expectation(before);
  const double rhs = destination.core().expectation(after);
  need(std::isfinite(lhs) && std::isfinite(rhs) && std::abs(lhs-rhs) < 1e-10,
       "Bell byte transfer changed a Pauli expectation");
}
std::uint32_t verify(unsigned seed, double& measured_microseconds) {
  qssd::Space space;
  space.execute_cpu("seed " + std::to_string(seed) + "\n"
                    "h 100\nt 100\ncx 100 101\n"
                    "h 102\ncx 102 200\n"
                    "h 105\nt 105\ncx 105 201\n");
  space.publish_quantum_byte("byte", "5000", "100");
  space.bind("cpu:byte", "byte");
  space.route("cpu:byte", "gpu:byte");
  space.route("gpu:byte", "ssd:byte");
  const auto prior = space.epoch()->cpu;
  const auto pinned = space.acquire("cpu:byte");
  need(!pinned.quantum_byte().deterministic, "source quantum byte was silently measured");
  need(pinned.quantum_byte().handle == "qbyte:5000@100", "source binding incorrect");
  must_reject([&]{space.teleport_quantum_byte("gpu:byte","101","300","400");}, space);
  must_reject([&]{space.teleport_quantum_byte("gpu:byte","100","104","400");}, space);
  space.execute_cpu("x 403\n");
  must_reject([&]{space.teleport_quantum_byte("gpu:byte","100","300","400");}, space);
  space.execute_cpu("x 403\n");

  const auto before_revision = space.acquire("cpu:byte").revision;
  const auto start = std::chrono::steady_clock::now();
  const auto receipts = space.teleport_quantum_byte("gpu:byte","100","300","400");
  measured_microseconds += std::chrono::duration<double,std::micro>(
      std::chrono::steady_clock::now()-start).count();
  std::uint32_t outcomes = 0;
  for (const auto& r : receipts)
    outcomes |= 1u << (2u*unsigned(r.m0)+unsigned(r.m1));

  const auto cpu = space.acquire("cpu:byte");
  const auto gpu = space.acquire("gpu:byte");
  const auto ssd = space.acquire("ssd:byte");
  need(cpu.identity == pinned.identity && gpu.identity == pinned.identity
       && ssd.identity == pinned.identity, "Bell transfer changed logical identity");
  need(cpu.revision == before_revision+1 && gpu.revision == cpu.revision
       && ssd.revision == cpu.revision, "Bell transfer did not publish one revision");
  need(cpu.epoch == gpu.epoch && cpu.epoch == ssd.epoch,
       "CPU GPU SSD ports acquired different authority versions");
  need(!cpu.quantum_byte().deterministic &&
       cpu.quantum_byte().handle == "qbyte:5000@400",
       "Bell destination not published through InverseMemory");
  need(!pinned.quantum_byte().deterministic &&
       pinned.quantum_byte().handle == "qbyte:5000@100",
       "old pinned lease changed after quantum handoff");

  const auto& final = cpu.epoch->cpu;
  for (unsigned bit=0; bit<8; ++bit) {
    const auto src=std::to_string(100+bit), dst=std::to_string(400+bit);
    for (const char a : {'X','Y','Z'}) {
      same_pauli(prior,final,{{src,a}},{{dst,a}});
      for (const auto& partner : {"200","201"})
        for (const char b : {'X','Y','Z'})
          same_pauli(prior,final,{{src,a},{partner,b}},{{dst,a},{partner,b}});
    }
  }
  for (unsigned first=0; first<8; ++first) {
    for (unsigned second=first+1; second<8; ++second) {
      for (const char a : {'X','Y','Z'}) for (const char b : {'X','Y','Z'})
        same_pauli(prior,final,
          {{std::to_string(100+first),a},{std::to_string(100+second),b}},
          {{std::to_string(400+first),a},{std::to_string(400+second),b}});
    }
  }
  for (const char a : {'X','Y','Z'})
    for (const char b : {'X','Y','Z'})
      for (const char c : {'X','Y','Z'})
        same_pauli(prior,final,
          {{"100",a},{"101",b},{"200",c}},
          {{"400",a},{"401",b},{"200",c}});

  const auto encoded=space.checkpoint();
  const auto restored=qssd::Space::restore(encoded);
  need(restored.checkpoint()==encoded,"Bell checkpoint was not reproducible");
  need(restored.acquire("cpu:byte").quantum_byte().handle=="qbyte:5000@400",
       "Bell quantum byte not restored");
  need(restored.acquire("gpu:byte").identity==cpu.identity,
       "Bell CPU GPU aliases were not restored");
  return outcomes;
}
}
int main() {
  try {
    std::uint32_t branches = 0;
    double elapsed_us = 0;
    constexpr unsigned trials=8;
    for (unsigned seed=0; seed<trials; ++seed)
      branches |= verify(seed,elapsed_us);
    need(branches==15,"not all four Bell measurement outcomes seen");
    std::cout << "{\"quantum_byte_bell\":\"PASS\",\"trials\":" << trials
      << ",\"qubits_per_handoff\":8,\"classical_bits_per_handoff\":16"
      << ",\"bell_branches_mask\":" << branches
      << ",\"pauli_and_atomic_checks\":" << checks
      << ",\"host_mean_handoff_us\":" << elapsed_us/trials
      << ",\"physical_quantum_hardware\":false"
      << ",\"physical_gpu_benchmark\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "QSSD_BELL_BYTE_FAIL: " << error.what() << "\n";
    return 1;
  }
}
