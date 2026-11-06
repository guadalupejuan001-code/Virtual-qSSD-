#include "qssd/state_space.hpp"
#include "qssd/state_bus.hpp"
#include "qssd/state_archive.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

namespace {
std::size_t checks = 0;
void need(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
  bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
  need(rejected, "invalid state transition accepted");
}
void near(double a, double b) { need(std::abs(a - b) < 2e-12, "quantum correlation changed"); }
void classical() {
  constexpr std::uint64_t n = 257;
  qssd::Space space;
  space.execute_cpu("#!reverse-pc-machine-v1\nset r0 3\nmul r0 2\nadd r0 1\nstore8 100 r0\nstore8 104 3\n");
  space.publish_cpu_rule("rule", n, "100", 2); space.bind("cpu:rule", "rule"); space.route("cpu:rule", "gpu:rule");
  xgpu::Machine machine(xgpu::example(n)); machine.run();
  space.publish_gpu("native", machine); space.bind("gpu:native", "native");
  need(space.acquire("gpu:native").field.get() == &machine.output(), "native GPU field was copied");
  space.gpu_binary("sum", "gpu:rule", "gpu:native", false);
  space.bind("ssd:data", "sum"); space.route("ssd:data", "cpu:data"); space.route("ssd:data", "gpu:data");
  const auto cpu = space.acquire("cpu:data"), gpu = space.acquire("gpu:data"), ssd = space.acquire("ssd:data");
  need(cpu.identity == gpu.identity && cpu.identity == ssd.identity && cpu.field == gpu.field && cpu.field == ssd.field, "three ports do not share one field");
  std::vector<std::uint32_t> dense(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    auto scalar = std::uint32_t(3 * i + 12);
    if (i == n - 1) scalar = 4;
    const auto native = i < n / 2 ? scalar : std::uint32_t(std::uint64_t(scalar) * scalar);
    dense[i] = std::uint32_t(std::uint64_t(3 * i + 7) + native);
    need(space.observe("gpu:data", i) == dense[i], "joint native CPU/GPU result differs from dense oracle");
  }
  auto old = space.acquire("gpu:data");
  qssd::Space cpu_port = space, gpu_port = space;
  std::mt19937_64 random(99);
  for (unsigned i = 0; i < 80; ++i) {
    const auto index = random() % n; const auto value = static_cast<std::uint32_t>(random());
    cpu_port.patch("cpu:data", index, value); dense[index] = value;
    need(gpu_port.observe("gpu:data", index) == value && space.observe("ssd:data", index) == value, "shared mutation was not coherent");
  }
  need(old.field == cpu.field && old.revision == cpu.revision, "in-flight lease mutated");
  need(space.acquire("gpu:data").identity == cpu.identity && space.acquire("gpu:data").revision > cpu.revision, "mutation changed identity or lost version");
  for (std::uint64_t i = 0; i < n; ++i) need(space.observe("ssd:data", i) == dense[i], "details lost");
  const auto expected = dense[200];
  const auto output = space.cpu_load_word("cpu:data", 200, "999999999999999999999999999999999");
  need(output.find("\"value\":" + std::to_string(expected)) != std::string::npos, "real XI load did not receive word");
  need(space.projected_cpu_bytes() == 4, "CPU observation expanded the logical domain");
  auto snapshot = space.checkpoint(); auto restored = qssd::Space::restore(snapshot);
  need(restored.checkpoint() == snapshot, "mixed checkpoint changed bytes");
  need(restored.acquire("cpu:data").field == restored.acquire("gpu:data").field, "restore duplicated aliased state");
  const auto rev = restored.epoch()->revision; restored.observe("cpu:data", 3);
  restored.route("cpu:data", "gpu:data"); restored.bind("gpu:data", "sum");
  need(restored.epoch()->revision == rev, "observation/no-op route mutated state");
  const auto saved = space.checkpoint();
  rejects([&] { space.patch("cpu:data", n, 3); });
  rejects([&] { space.bind("gpu:invalid", "missing"); });
  rejects([&] { space.route("cpu:data", "cpu:data", true); });
  rejects([&] { space.execute_cpu("x 999\nmem_store 42 1\ninvalid_opcode\n"); });
  need(space.checkpoint() == saved, "failed transaction changed shared authority");
  for (std::size_t at : {std::size_t(0), std::size_t(13), snapshot.size() / 2, snapshot.size() - 1}) {
    auto bad = snapshot; bad[at] ^= 0x5a; rejects([&] { qssd::Space::restore(bad); });
  }
  rejects([&] { qssd::Space::restore(snapshot.substr(0, snapshot.size() - 1)); });

  // The producer may replace every register/output or be destroyed after
  // publication. qSSD must retain only the emitted native immutable Field.
  {
    xgpu::Program program{32, {{xgpu::Op::Index, 0}, {xgpu::Op::Store, 0},
      {xgpu::Op::Constant, 0, 0, 0, 0, 77}, {xgpu::Op::Store, 0}, {xgpu::Op::Halt}}};
    xgpu::Machine producer(program); producer.step(); producer.step();
    space.publish_gpu("in-flight", producer); space.bind("gpu:in-flight", "in-flight");
    need(space.acquire("gpu:in-flight").field.get() == &producer.output(), "publication copied native field");
    producer.run(); need(producer.output().at(9) == 77, "producer continuation failed");
  }
  need(space.observe("gpu:in-flight", 9) == 9, "producer replacement/destruction invalidated shared field");
}
void quantum() {
  unsigned branches = 0;
  for (unsigned seed = 0; seed < 32; ++seed) {
    qssd::Space space;
    space.execute_cpu("seed " + std::to_string(seed) + "\nh 100\nt 100\ncx 100 200\n");
    space.publish_quantum_byte("quantum", "5000", "100"); space.bind("cpu:q", "quantum");
    space.route("cpu:q", "gpu:q"); space.route("gpu:q", "ssd:q");
    auto before = space.acquire("gpu:q");
    need(!before.quantum_byte().deterministic, "superposition was silently measured");
    const auto saved = space.checkpoint();
    rejects([&] { space.cpu_load_word("gpu:q", 0, "6000"); });
    rejects([&] { space.gpu_binary("bad", "gpu:q", "gpu:q", false); });
    need(space.checkpoint() == saved, "invalid GPU quantum projection changed state");
    const auto receipt = space.teleport_qubit("100", "300", "400");
    branches |= 1u << (2 * unsigned(receipt.m0) + unsigned(receipt.m1));
    need(space.acquire("cpu:q").quantum_byte().deterministic, "source not consumed");
    need(!before.quantum_byte().deterministic, "old quantum lease mutated");
    space.publish_quantum_byte("destination", "6000", "400"); space.bind("ssd:destination", "destination");
    space.route("ssd:destination", "gpu:q");
    need(!space.acquire("gpu:q").quantum_byte().deterministic, "destination quantum handle lost");
    auto recovered = qssd::Space::restore(space.checkpoint());
    auto state = recovered.epoch();
    near(state->cpu.core().expectation({{"400", 'Z'}, {"200", 'Z'}}), 1);
    near(state->cpu.core().expectation({{"400", 'X'}, {"200", 'X'}}), std::sqrt(0.5));
    near(state->cpu.core().expectation({{"400", 'Y'}, {"200", 'X'}}), std::sqrt(0.5));
    need(recovered.acquire("gpu:q").quantum_byte().handle == space.acquire("gpu:q").quantum_byte().handle,
         "native InverseMemory handle changed after restore");
  }
  need(branches == 15, "not all Bell branches tested");
}
void scale_and_concurrency() {
  qssd::Space space;
  xgpu::Machine native(xgpu::example(xgpu::domain_limit)); native.run();
  space.publish_gpu("huge", native); space.bind("ssd:huge", "huge"); space.route("ssd:huge", "cpu:huge"); space.route("ssd:huge", "gpu:huge");
  need(space.observed_words() == 0 && space.projected_cpu_bytes() == 0, "routing materialized data");
  need(space.checkpoint().size() < 20000, "rule domain expanded");
  const auto data = space.acquire("gpu:huge");
  space.route("gpu:huge", "ssd:handoff", true);
  need(space.acquire("ssd:handoff").field == data.field, "handoff copied field");
  rejects([&] { space.acquire("gpu:huge"); });
  space.route("ssd:handoff", "gpu:huge", true);
  std::vector<std::thread> writers;
  for (unsigned writer = 0; writer < 8; ++writer) writers.emplace_back([&, writer] {
    qssd::Space port = space;
    for (unsigned i = 0; i < 32; ++i) port.patch("cpu:huge", writer * 32 + i, 100000 + writer * 32 + i);
  });
  for (auto& writer : writers) writer.join();
  for (unsigned i = 0; i < 256; ++i) need(space.observe("gpu:huge", i) == 100000 + i, "concurrent state transition lost update");
  std::string ghz = "h 0\n";
  for (unsigned i = 1; i < 1000; ++i) ghz += "cx 0 " + std::to_string(i) + "\n";
  space.execute_cpu(ghz);
  need(space.epoch()->cpu.core().stats().active_qubits == 1000, "1000-qubit authority not active");
  const auto saved = space.checkpoint(); auto recovered = qssd::Space::restore(saved);
  need(recovered.checkpoint() == saved, "large common authority did not restore exactly");
  need(recovered.acquire("cpu:huge").field == recovered.acquire("gpu:huge").field, "large restored ports diverged");
  recovered.teleport_qubit("0", "1000", "1001");
  near(recovered.epoch()->cpu.core().expectation({{"1001", 'Z'}, {"999", 'Z'}}), 1);
  std::vector<mundo_x::InfiniteCore::Term> all_x{{"1001", 'X'}};
  for (unsigned i = 1; i < 1000; ++i) all_x.push_back({std::to_string(i), 'X'});
  near(recovered.epoch()->cpu.core().expectation(all_x), 1);
  need(recovered.epoch()->cpu.core().stats().active_qubits == 1002, "Bell auxiliaries not accounted for");
  qssd::StateBus<qssd::State> complete(*space.epoch());auto pinned=complete.snapshot();auto branch=complete.fork();
  branch.update([](qssd::State& state){state.cpu.execute("seed 73\nh 1500\nt 1500\ncx 1500 1501\nmem_bind 9999 1500\n");return true;},"quantum-branch");
  need(complete.snapshot()==pinned&&branch.snapshot()->value->cpu.core().stats().active_qubits==1002,"full-state fork corrupted original CPU graph");
  need(complete.snapshot()->value->objects.at("huge").field==branch.snapshot()->value->objects.at("huge").field,"full-state fork copied compact GPU field");
  auto original=branch.snapshot()->value->cpu;const auto nativeCpuBytes=reverse_pc::rvpc2::save(original);
  qssd::StateArchive archive;archive.add("cpu",std::string(nativeCpuBytes.begin(),nativeCpuBytes.end()));archive.add("resources",std::string("source\0binary",13));
  const auto recoveredArchive=qssd::StateArchive::decode(archive.encode());const auto& cpuBytes=recoveredArchive.get("cpu");
  auto replay=reverse_pc::rvpc2::restore({cpuBytes.begin(),cpuBytes.end()});
  need(reverse_pc::rvpc2::save(replay)==nativeCpuBytes,"complete CPU native archive lost phases, bindings or RNG");
  need(replay.execute("measure 1500\n")==original.execute("measure 1500\n"),"restored RNG future diverged");
  complete.rewind(pinned->serial);need(complete.snapshot()->value==pinned->value,"rewind failed to reuse the full original state");

}
}
int main() {
  try {
    classical(); quantum(); scale_and_concurrency();
    std::cout << "{\"shared_state\":\"PASS\",\"checks\":" << checks
      << ",\"original_cpu_xi\":true,\"original_gpu_c01\":true,\"quantum_handle_views\":true"
      << ",\"logical_domain_words\":4294967296,\"logical_routing_dense_words\":0"
      << ",\"explicit_cpu_projection_bytes\":4,\"concurrent_writers\":8,\"stored_ghz_qubits\":1000"
      << ",\"bell_auxiliaries\":2,\"entangled_non_clifford_trials\":32}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "QSSD_TEST_FAIL " << error.what() << '\n'; return 1; }
}
