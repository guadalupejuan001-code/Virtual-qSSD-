#pragma once
#include "qssd/quantum_transfer.hpp"
#include "estado_gpu.hpp"
#include "integration/state_io.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>

namespace qssd {
using Field = xgpu::Field;
using FieldPtr = std::shared_ptr<const Field>;
constexpr std::size_t max_objects = 1024, max_ports = 4096;
constexpr std::size_t max_descriptor_bytes = 12u * 1024 * 1024;
constexpr std::size_t max_checkpoint_bytes = 64u * 1024 * 1024;

inline void valid_name(const std::string& name) {
  if (name.empty() || name.size() > 128) throw std::invalid_argument("invalid qSSD identity name");
  for (unsigned char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.')) throw std::invalid_argument("invalid qSSD identity character");
}
inline void valid_port(const std::string& port) {
  const auto colon = port.find(':');
  if (colon == std::string::npos || (port.substr(0, colon) != "cpu" && port.substr(0, colon) != "gpu" && port.substr(0, colon) != "ssd"))
    throw std::invalid_argument("qSSD port requires cpu:, gpu: or ssd:");
  valid_name(port.substr(colon + 1));
}
struct Object {
  std::uint64_t id, revision;
  FieldPtr field;
  std::string quantum_address;
};
struct State {
  std::uint64_t revision = 0, next_id = 1;
  Runtime cpu;
  std::map<std::string, Object> objects;
  std::map<std::string, std::string> ports;
};
struct Lease {
  std::uint64_t identity, revision, state_revision;
  FieldPtr field;
  std::string quantum_address;
  std::shared_ptr<const State> epoch;
  xgpu::Word at(xgpu::Index index) const {
    if (!field) throw std::runtime_error("qSSD quantum view requires an explicit quantum observation");
    return field->at(index);
  }
  mundo_x::InverseMemory::ReadResult quantum_byte() const {
    if (quantum_address.empty()) throw std::runtime_error("qSSD field is not a quantum byte view");
    return epoch->cpu.memory().read(quantum_address, epoch->cpu.core());
  }
};

// One controller, one immutable committed root, and explicit CPU/GPU/SSD ports.
// A lease pins an old state for in-flight work; resolving a port sees the latest
// committed version. No per-device dense buffer is the data authority.
class Space {
  struct Control {
    std::mutex mutex;
    std::shared_ptr<const State> head = std::make_shared<const State>();
    std::atomic<std::uint64_t> observed_words{0}, projected_cpu_bytes{0}, routed_references{0}, coefficient_bytes{0};
  };
  std::shared_ptr<Control> control_ = std::make_shared<Control>();
  static const Object& resolve(const State& state, const std::string& port) {
    valid_port(port);
    const auto link = state.ports.find(port);
    if (link == state.ports.end()) throw std::out_of_range("qSSD port is not bound");
    return state.objects.at(link->second);
  }
  static void budget(const State& state) {
    if (state.objects.size() > max_objects || state.ports.size() > max_ports)
      throw std::length_error("qSSD identity/port budget exceeded");
    std::set<const Field*> seen;
    std::size_t bytes = 0;
    for (const auto& [name, object] : state.objects) {
      (void)name;
      if (!object.field || !seen.insert(object.field.get()).second) continue;
      bytes += 24 * object.field->bands.size() + 12 * object.field->details.size();
      for (const auto& band : object.field->bands) bytes += 4 * band.mode.coefficients.size();
      if (bytes > max_descriptor_bytes) throw std::length_error("qSSD compact-description budget exceeded");
    }
  }
  static void publish(State& state, const std::string& name, FieldPtr field, const std::string& quantum_address = {}) {
    valid_name(name);
    if (bool(field) == !quantum_address.empty()) throw std::invalid_argument("qSSD identity must have one representation");
    if (field) field->validate();
    auto found = state.objects.find(name);
    if (found == state.objects.end()) {
      if (state.next_id == UINT64_MAX) throw std::overflow_error("qSSD identity overflow");
      state.objects.emplace(name, Object{state.next_id++, 1, std::move(field), quantum_address});
    } else {
      if (found->second.revision == UINT64_MAX) throw std::overflow_error("qSSD object revision overflow");
      found->second.field = std::move(field); found->second.quantum_address = quantum_address; ++found->second.revision;
    }
  }
  void transition(const std::function<bool(State&)>& operation) {
    std::lock_guard<std::mutex> lock(control_->mutex);
    auto next = std::make_shared<State>(*control_->head);
    if (!operation(*next)) return;
    budget(*next);
    if (next->revision == UINT64_MAX) throw std::overflow_error("qSSD space revision overflow");
    ++next->revision;
    std::atomic_store(&control_->head, std::shared_ptr<const State>(std::move(next)));
  }
 public:
  // Copies of Space are ports into ONE authority, not independent SSD copies.
  std::shared_ptr<const State> epoch() const { return std::atomic_load(&control_->head); }
  Lease acquire(const std::string& port) const {
    const auto state = epoch(); const auto& object = resolve(*state, port);
    return {object.id, object.revision, state->revision, object.field, object.quantum_address, state};
  }
  xgpu::Word observe(const std::string& port, xgpu::Index index) const {
    auto view = acquire(port); const auto word = view.at(index);
    control_->observed_words.fetch_add(1); return word;
  }
  std::uint64_t observed_words() const { return control_->observed_words.load(); }
  std::uint64_t projected_cpu_bytes() const { return control_->projected_cpu_bytes.load(); }
  std::uint64_t routed_references() const { return control_->routed_references.load(); }
  std::uint64_t cpu_rule_input_bytes() const { return control_->coefficient_bytes.load(); }

  // A reviewed read-only accessor shares the original immutable Field itself.
  // The producer can continue; no program or other registers are retained here.
  // Import an explicitly classical producer descriptor into this component.
  // Ownership is transferred into an immutable object; C12 consumers acquire
  // that SAME object through the existing CPU/SSD/GPU ports. No dense output
  // expansion, implicit quantum measurement, or duplicate device store.
  void publish_field(const std::string& name, Field field) {
    valid_name(name); field.validate();
    auto immutable = std::make_shared<const Field>(std::move(field));
    transition([&](State& state) { publish(state, name, immutable); return true; });
  }
  void publish_gpu(const std::string& name, const xgpu::Machine& source) {
    FieldPtr field = source.output_view();
    transition([&](State& state) { publish(state, name, field); return true; });
  }
  std::string execute_cpu(const std::string& program) {
    std::string output;
    transition([&](State& state) {
      const auto before = reverse_pc::rvpc2::save(state.cpu);
      output = state.cpu.execute(program);
      return reverse_pc::rvpc2::save(state.cpu) != before;
    });
    return output;
  }
  // Fast path for a caller that already knows the program must publish a new
  // CPU revision. It preserves the same transactional copy/rollback boundary as
  // execute_cpu(), but deliberately skips serializing the complete XI/Ether
  // runtime before and after the program merely to rediscover that it changed.
  // Do not use this for speculative/no-op programs: this API always commits one
  // fresh qSSD revision when execution succeeds.
  std::string execute_cpu_committed(const std::string& program) {
    std::string output;
    transition([&](State& state) {
      output = state.cpu.execute(program);
      return true;
    });
    return output;
  }
  // CPU-produced coefficients remain small: only 4*(degree+1) requested bytes
  // are observed in its original InverseMemory. No logical array is generated.
  void publish_cpu_rule(const std::string& name, xgpu::Index count, const std::string& base, unsigned coefficients) {
    if (!coefficients || coefficients > xgpu::max_degree + 1) throw std::length_error("qSSD rule degree");
    transition([&](State& state) {
      std::vector<xgpu::Word> values(coefficients, 0);
      for (unsigned i = 0; i < 4 * coefficients; ++i) {
        const auto result = state.cpu.memory().read(address_offset(base, i), state.cpu.core());
        if (!result.deterministic) throw std::runtime_error("qSSD CPU coefficient is a quantum handle: " + result.handle);
        values[i / 4] |= xgpu::Word(result.value) << (8 * (i % 4));
      }
      publish(state, name, std::make_shared<const Field>(count, xgpu::Mode(std::move(values))));
      return true;
    });
    control_->coefficient_bytes.fetch_add(4 * coefficients);
  }
  void bind(const std::string& port, const std::string& object) {
    valid_port(port); valid_name(object);
    transition([&](State& state) {
      if (!state.objects.count(object)) throw std::out_of_range("qSSD identity does not exist");
      const auto old = state.ports.find(port);
      if (old != state.ports.end() && old->second == object) return false;
      state.ports[port] = object; return true;
    });
  }
  void publish_quantum_byte(const std::string& name, const std::string& address, const std::string& qbase) {
    const auto logical = address_offset(address, 0), qubits = address_offset(qbase, 0);
    transition([&](State& state) {
      state.cpu.execute("mem_bind " + logical + " " + qubits + "\n");
      (void)reverse_pc::rvpc2::save(state.cpu);
      publish(state, name, {}, logical); return true;
    });
  }
  void route(const std::string& source, const std::string& destination, bool consume_source = false) {
    valid_port(source); valid_port(destination);
    if (source == destination && consume_source) throw std::invalid_argument("qSSD handoff needs distinct ports");
    bool routed = false;
    transition([&](State& state) {
      (void)resolve(state, source);
      const auto name = state.ports.at(source);
      if (source == destination) return false;
      const auto old = state.ports.find(destination);
      if (old != state.ports.end() && old->second == name && !consume_source) return false;
      state.ports[destination] = name;
      if (consume_source) state.ports.erase(source);
      routed = true;
      return true;
    });
    if (routed) control_->routed_references.fetch_add(1);
  }
  void gpu_binary(const std::string& output, const std::string& left, const std::string& right, bool multiply) {
    transition([&](State& state) {
      if (!resolve(state, left).field || !resolve(state, right).field)
        throw std::runtime_error("GPU uint32 operation cannot implicitly measure a quantum handle");
      auto field = Field::combine(*resolve(state, left).field, *resolve(state, right).field, multiply);
      publish(state, output, std::make_shared<const Field>(std::move(field))); return true;
    });
  }
  void patch(const std::string& port, xgpu::Index index, xgpu::Word value) {
    transition([&](State& state) {
      const auto name = state.ports.at(port); const auto& old = resolve(state, port);
      if (!old.field) throw std::runtime_error("qSSD quantum data requires a quantum operation");
      if (old.field->at(index) == value) return false;
      publish(state, name, std::make_shared<const Field>(old.field->patched(index, value))); return true;
    });
  }
  // This is the explicit observation boundary into the existing XI CPU. It
  // projects FOUR bytes and then loads/assembles the word with real XI opcodes.
  std::string cpu_load_word(const std::string& port, xgpu::Index index, const std::string& mailbox) {
    std::string output;
    transition([&](State& state) {
      if (!resolve(state, port).field) throw std::runtime_error("CPU word projection cannot implicitly measure a quantum handle");
      const auto word = resolve(state, port).field->at(index);
      std::string program = "#!reverse-pc-machine-v1\nset r0 0\n";
      for (unsigned i = 0; i < 4; ++i) {
        const auto address = address_offset(mailbox, i);
        program += "store8 " + address + " " + std::to_string((word >> (8 * i)) & 255) + "\n";
        program += "load8 r1 " + address + "\nmul r1 " + std::to_string(UINT64_C(1) << (8 * i)) + "\nadd r0 r1\n";
      }
      program += "print r0\n";
      output = state.cpu.execute(program); (void)reverse_pc::rvpc2::save(state.cpu); return true;
    });
    control_->projected_cpu_bytes.fetch_add(4); return output;
  }
  BellReceipt teleport_qubit(const std::string& source, const std::string& auxiliary, const std::string& destination) {
    BellReceipt receipt{};
    transition([&](State& state) {
      receipt = bell_transfer(state.cpu, source, auxiliary, destination);
      (void)reverse_pc::rvpc2::save(state.cpu); return true;
    });
    return receipt;
  }
  // C05: one atomic simulated Bell transfer for the eight selected qubits
  // backing an InverseMemory byte. Existing CPU/GPU/SSD ports remain bound to
  // the SAME logical object and observe its new revision and destination.
  // The 16 classical Bell outcomes are returned explicitly; no physical
  // entanglement, zero-time transport or implicit quantum measurement is claimed.
  std::array<BellReceipt, 8> teleport_quantum_byte(
      const std::string& port, const std::string& source_base,
      const std::string& auxiliary_base, const std::string& destination_base) {
    valid_port(port);
    const std::array<std::string, 3> bases{
        address_offset(source_base, 0), address_offset(auxiliary_base, 0),
        address_offset(destination_base, 0)};
    std::array<std::array<std::string, 8>, 3> addresses;
    std::set<std::string> distinct;
    for (std::size_t group = 0; group < bases.size(); ++group) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto address = address_offset(bases[group], bit);
        if (!distinct.insert(address).second)
          throw std::invalid_argument("QSSD Bell byte ranges overlap");
        addresses[group][bit] = std::move(address);
      }
    }
    std::array<BellReceipt, 8> receipts{};
    transition([&](State& state) {
      const auto& object = resolve(state, port);
      if (object.field || object.quantum_address.empty())
        throw std::invalid_argument("QSSD Bell byte requires a quantum port");
      const auto name = state.ports.at(port);
      const auto logical = object.quantum_address;
      if (bound_quantum_base(state.cpu, logical) != bases[0])
        throw std::invalid_argument("QSSD Bell source differs from the bound quantum byte");
      for (unsigned bit = 0; bit < 8; ++bit)
        receipts[bit] = bell_transfer(state.cpu,
            addresses[0][bit], addresses[1][bit], addresses[2][bit]);
      state.cpu.execute("mem_bind " + logical + " " + bases[2] + "\n");
      // Revisions and in-flight leases are preserved by the immutable root.
      publish(state, name, {}, logical);
      (void)reverse_pc::rvpc2::save(state.cpu);
      return true;
    });
    return receipts;
  }
  // Full current state, compact fields and alias topology; explicit checkpoint
  // only. Routing and computation do not serialize or rewrite this transport.
  std::string checkpoint() const;
  static Space restore(const std::string& bytes);
};
}  // namespace qssd

#include "qssd/checkpoint.hpp"
