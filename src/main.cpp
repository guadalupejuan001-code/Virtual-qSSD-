#include "qssd/state_space.hpp"
#include "qssd/files.hpp"
#include "programa.hpp"
#include <chrono>
#include <iostream>

namespace {
void info(const qssd::Space& space) {
  const auto state = space.epoch();
  std::set<const qssd::Field*> fields;
  std::uint64_t words = 0;
  for (const auto& [name, object] : state->objects) {
    (void)name;
    if (object.field && fields.insert(object.field.get()).second) words += object.field->count;
  }
  std::cout << "{\"profile\":\"qSSD-shared-state-C01\",\"revision\":" << state->revision
    << ",\"objects\":" << state->objects.size() << ",\"ports\":" << state->ports.size()
    << ",\"unique_fields\":" << fields.size() << ",\"logical_field_words\":" << words
    << ",\"checkpoint_bytes\":" << space.checkpoint().size()
    << ",\"active_qubits\":" << state->cpu.core().stats().active_qubits
    << ",\"physical_gpu_tested\":false,\"host_execution\":\"CPU\"}\n";
}
qssd::Space demo() {
  qssd::Space space;
  space.execute_cpu("#!reverse-pc-machine-v1\nset r0 3\nmul r0 2\nadd r0 1\nstore8 100 r0\nset r1 3\nstore8 104 r1\n");
  space.publish_cpu_rule("cpu-rule", xgpu::domain_limit, "100", 2);
  space.bind("cpu:rule", "cpu-rule"); space.route("cpu:rule", "gpu:rule");
  xgpu::Machine gpu(xgpu::example(xgpu::domain_limit)); gpu.run();
  space.publish_gpu("gpu-output", gpu); space.bind("gpu:output", "gpu-output");
  space.route("gpu:output", "ssd:source");
  const bool native_gpu_field_shared = space.acquire("ssd:source").field.get() == &gpu.output();
  space.gpu_binary("joint", "gpu:rule", "gpu:output", false);
  space.bind("ssd:joint", "joint"); space.route("ssd:joint", "cpu:joint"); space.route("ssd:joint", "gpu:joint");
  const auto old = space.acquire("gpu:joint");
  space.patch("cpu:joint", 42, 0x12345678);
  const auto cpu = space.acquire("cpu:joint"), rendered = space.acquire("gpu:joint"), stored = space.acquire("ssd:joint");
  const bool coherent = cpu.identity == rendered.identity && cpu.revision == stored.revision && cpu.field == rendered.field && cpu.field == stored.field;
  if (!native_gpu_field_shared || !coherent || old.at(42) == cpu.at(42)) throw std::runtime_error("shared-state demo failed");
  auto loaded = space.cpu_load_word("cpu:joint", 42, "1000");
  while (!loaded.empty() && (loaded.back() == '\n' || loaded.back() == '\r')) loaded.pop_back();
  space.execute_cpu("h 100\nt 100\ncx 100 200\n");
  space.publish_quantum_byte("q-source", "5000", "100"); space.bind("cpu:q", "q-source");
  space.route("cpu:q", "ssd:q"); space.route("ssd:q", "gpu:q");
  const auto source = space.acquire("gpu:q");
  const bool unresolved = !source.quantum_byte().deterministic;
  const auto bell = space.teleport_qubit("100", "300", "400");
  space.publish_quantum_byte("q-destination", "6000", "400"); space.bind("ssd:q-destination", "q-destination");
  space.route("ssd:q-destination", "gpu:q");
  std::cout << "{\"demo\":\"PASS\",\"same_native_gpu_field\":" << (native_gpu_field_shared ? "true" : "false")
    << ",\"cpu_gpu_ssd_same_identity_and_revision\":" << (coherent ? "true" : "false")
    << ",\"logical_domain_words\":" << xgpu::domain_limit
    << ",\"routing_dense_words_materialized\":0,\"explicit_cpu_projection_bytes\":" << space.projected_cpu_bytes()
    << ",\"cpu_rule_input_bytes\":" << space.cpu_rule_input_bytes()
    << ",\"cpu_output\":" << loaded << ",\"quantum_handle_preserved\":" << (unresolved ? "true" : "false")
    << ",\"bell_classical_bits\":2,\"bell_m0\":" << bell.m0 << ",\"bell_m1\":" << bell.m1
    << ",\"bell_preserved_partner_ZZ\":" << space.epoch()->cpu.core().expectation({{"400", 'Z'}, {"200", 'Z'}})
    << ",\"logical_aliases_are_bell_teleportation\":false}\n";
  return space;
}
void measure() {
  constexpr unsigned handoffs = 10000;
  std::cout << "{\"profile\":\"logical-state-handoff\",\"host\":\"CPU\",\"cases\":[";
  bool first = true;
  for (const std::uint64_t domain : {UINT64_C(1024), UINT64_C(1048576), UINT64_C(4294967296)}) {
    qssd::Space space; xgpu::Machine gpu(xgpu::example(domain)); gpu.run();
    space.publish_gpu("data", gpu); space.bind("gpu:transfer", "data");
    auto original = space.acquire("gpu:transfer"); const auto revision = space.epoch()->revision;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < handoffs / 2; ++i) {
      space.route("gpu:transfer", "ssd:holding", true); space.route("ssd:holding", "gpu:transfer", true);
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    const auto last = space.acquire("gpu:transfer");
    const bool shared = last.field == original.field && last.identity == original.identity && last.revision == original.revision;
    if (!shared || space.observed_words() || space.projected_cpu_bytes()) throw std::runtime_error("handoff expanded or changed data");
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"logical_words\":" << domain << ",\"handoffs\":" << space.routed_references()
      << ",\"state_transitions\":" << space.epoch()->revision - revision << ",\"dense_words_materialized\":0"
      << ",\"native_gpu_field_pointer_preserved\":true,\"checkpoint_bytes\":" << space.checkpoint().size()
      << ",\"host_elapsed_us\":" << elapsed << '}';
  }
  std::cout << "],\"physical_gpu_benchmark\":false,\"instantaneous_io_demonstrated\":false}\n";
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "measure") { measure(); return 0; }
    if (argc >= 2 && std::string(argv[1]) == "demo" && argc <= 3) {
      const auto space = demo();
      if (argc == 3) qssd::save_new(argv[2], space.checkpoint());
      info(space); return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "inspect") { info(qssd::Space::restore(qssd::read_file(argv[2]))); return 0; }
    if (argc == 5 && std::string(argv[1]) == "observe") {
      const auto space = qssd::Space::restore(qssd::read_file(argv[2]));
      const auto value = space.observe(argv[3], xgpu::decimal(argv[4], xgpu::domain_limit - 1));
      std::cout << "{\"value\":" << value << "}\n"; return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "run-xi") {
      auto space = qssd::Space::restore(qssd::read_file(argv[2]));
      const auto output = space.execute_cpu(qssd::read_file(argv[3]));
      qssd::save_new(argv[4], space.checkpoint()); std::cout << output; return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "export-cpu") {
      const auto space = qssd::Space::restore(qssd::read_file(argv[2]));
      const auto native = reverse_pc::rvpc2::save(space.epoch()->cpu);
      qssd::save_new(argv[3], {native.begin(), native.end()}); return 0;
    }
    std::cout << "qssd_space demo [NEW_CHECKPOINT.xqs]\nqssd_space measure\nqssd_space inspect CHECKPOINT.xqs\n"
      << "qssd_space observe CHECKPOINT.xqs cpu:PORT|gpu:PORT|ssd:PORT INDEX\n"
      << "qssd_space run-xi INPUT.xqs PROGRAM.xi NEW_OUTPUT.xqs\n"
      << "qssd_space export-cpu INPUT.xqs NEW_STATE.rvp\n";
    return argc == 1 || (argc == 2 && std::string(argv[1]) == "--help") ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << "QSSD_FAIL " << error.what() << '\n'; return 1; }
}
