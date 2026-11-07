#pragma once
#include "qssd/state_space.hpp"
#include "ejecucion_gpu.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace qssd {

// C06: a revision-pinned handoff from QSSD's original immutable xgpu::Field
// to the original GPU-VIRTUAL C12 executor. No dense field, byte array,
// shader, or physical GPU transfer is created by this interface.
// The pinned revision remains valid during concurrent CPU state changes.
class C12Window {
 public:
  explicit C12Window(Lease source)
      : pinned_(std::move(source)), engine_(valid_count(pinned_), {},
                            xgpu::ExecutionProfile::C12) {
    engine_.initialize_global_shared(pinned_.field);
  }

  std::size_t enqueue(xgpu::Kernel kernel, xgpu::Index domain,
                      unsigned local_size = 64,
                      unsigned shared_words = 0,
                      std::vector<std::size_t> dependencies = {}) {
    if (domain != pinned_.field->count)
      throw std::invalid_argument("QSSD GPU C12 domain must match pinned field");
    return engine_.enqueue(std::move(kernel), domain, local_size,
                           shared_words, std::move(dependencies));
  }
  bool run(std::uint64_t step_budget=1000000) { return engine_.run(step_budget); }
  const xgpu::Execution& engine() const noexcept { return engine_; }
  const Lease& pinned() const noexcept { return pinned_; }
  bool source_pointer_preserved() const {
    return &engine_.global().field() == pinned_.field.get();
  }

 private:
  static xgpu::Index valid_count(const Lease& lease) {
    if (!lease.field || !lease.epoch || !lease.quantum_address.empty())
      throw std::invalid_argument("GPU C12 arithmetic requires an explicit classical Field");
    return lease.field->count;
  }
  Lease pinned_;
  xgpu::Execution engine_;
};

// This provides the GPU-VIRTUAL *port* with a read-only observation on a
// quantum state already owned by the original XI/Ether core. The observable
// is evaluated on the host by XI/Ether; C12 does NOT execute quantum gates,
// and this is NOT a physical quantum GPU/teleportation channel.
class QuantumGpuView {
 public:
  explicit QuantumGpuView(Lease source) : pinned_(std::move(source)) {
    if (!pinned_.epoch || pinned_.field || pinned_.quantum_address.empty())
      throw std::invalid_argument("QSSD quantum GPU view requires a quantum port");
  }
  const Lease& pinned() const noexcept { return pinned_; }
  mundo_x::InverseMemory::ReadResult quantum_byte() const {
    return pinned_.quantum_byte();
  }
  double expectation(const std::vector<mundo_x::InfiniteCore::Term>& terms) const {
    return pinned_.epoch->cpu.core().expectation(terms);
  }

 private:
  Lease pinned_;
};
}  // namespace qssd
