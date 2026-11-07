#include "qssd/c12_bridge.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks=0;
void need(bool ok,const char* name) {
  ++checks;
  if (!ok) throw std::runtime_error(name);
}
xgpu::Kernel compute_kernel() {
  using Op=xgpu::StateOp;
  return {{ {Op::Index,0},
            {Op::LoadGlobal,1,0},
            {Op::Constant,2,0,0,0,5},
            {Op::Add,3,1,2},
            {Op::Output,3},
            {Op::Halt} }};
}
xgpu::Kernel float_kernel() {
  using Op=xgpu::StateOp;
  return {{ {Op::Index,0},
            {Op::LoadGlobal,1,0},
            {Op::FloatFromUnsigned,2,1},
            {Op::Constant,3,0,0,0,2},
            {Op::FloatFromUnsigned,4,3},
            {Op::FloatAdd,5,2,4},
            {Op::Output,5},
            {Op::Halt} }};
}
xgpu::Word bits_of(float f) {
  xgpu::Word out=0;static_assert(sizeof(out)==sizeof(f),"binary32 host expected");
  std::memcpy(&out,&f,sizeof(out));return out;
}
void classical() {
  constexpr xgpu::Index domain=65536;
  qssd::Space space;
  // Actual XI machine instructions generate two exact GPU coefficients.
  space.execute_cpu("#!reverse-pc-machine-v1\n"
                    "set r0 3\nmul r0 2\nadd r0 1\n"
                    "store8 100 r0\nset r1 3\nstore8 104 r1\n");
  space.publish_cpu_rule("xi-rule",domain,"100",2);
  space.bind("cpu:source","xi-rule");
  space.route("cpu:source","gpu:source");
  space.route("gpu:source","ssd:source");
  const auto before=space.acquire("gpu:source");
  const auto identity=before.identity;
  const auto initial_rev=before.revision;
  need(before.field && before.field->bands.size()==1 &&
       before.field->bands[0].mode.coefficients==std::vector<xgpu::Word>({7,3}),
       "XI did not produce the expected exact GPU rule");
  need(space.acquire("cpu:source").field==before.field &&
       space.acquire("ssd:source").field==before.field,
       "QSSD CPU/GPU/SSD do not share one original field");

  qssd::C12Window old(before);
  need(old.engine().profile()==xgpu::ExecutionProfile::C12 &&
       old.source_pointer_preserved(), "C12 copied QSSD input field");
  old.enqueue(compute_kernel(),domain);
  need(old.run(), "C12 relation kernel did not complete");
  need(old.source_pointer_preserved() &&
       old.engine().dispatch(0).output_valid.full(),
       "C12 lost the pinned input or produced an incomplete output");
  for(const xgpu::Index index:{xgpu::Index(0),xgpu::Index(1),xgpu::Index(42),
       xgpu::Index(100),xgpu::Index(1024),domain-1}) {
    need(old.engine().dispatch(0).output->at(index)==xgpu::Word(3*index+12),
         "C12 relation result differs from independent XI field formula");
  }

  qssd::C12Window fp(before);
  fp.enqueue(float_kernel(),domain);
  need(fp.run(),"C12 binary32 kernel did not finish");
  for(const xgpu::Index index:{xgpu::Index(0),xgpu::Index(42),
       xgpu::Index(999),domain-1}) {
    const auto expected=bits_of(static_cast<float>(3*index+9));
    need(fp.engine().dispatch(0).output->at(index)==expected,
         "C12 binary32 result differed from independent host float32");
  }

  // Source changes are published by XI/QSSD; the old C12 execution remains
  // pinned to its original revision, while new clients observe the change.
  space.patch("cpu:source",42,9000);
  const auto after=space.acquire("gpu:source");
  need(after.identity==identity && after.revision==initial_rev+1 &&
       after.field!=before.field,"QSSD new revision not published");
  need(space.acquire("ssd:source").field==after.field &&
       space.acquire("cpu:source").field==after.field,
       "CPU/GPU/SSD lost coherence after QSSD patch");
  need(old.engine().dispatch(0).output->at(42)==138 &&
       old.source_pointer_preserved(),"in-flight C12 result changed");
  qssd::C12Window updated(after);
  updated.enqueue(compute_kernel(),domain);
  need(updated.run() && updated.engine().dispatch(0).output->at(42)==9005,
       "new C12 job did not consume changed QSSD state");
  const auto bytes=space.checkpoint();
  auto recovered=qssd::Space::restore(bytes);
  const auto r=recovered.acquire("gpu:source");
  need(r.field==recovered.acquire("cpu:source").field &&
       r.field==recovered.acquire("ssd:source").field,"QSSD checkpoint lost aliases");
  qssd::C12Window continued(r);
  continued.enqueue(compute_kernel(),domain);
  need(continued.run() && continued.source_pointer_preserved() &&
       continued.engine().dispatch(0).output->at(42)==9005,
       "C12 could not continue after original QSSD checkpoint");

  // Same-size, same-semantics initialization comparison: copy *compact*
  // descriptor into the original C12 API vs handing it a shared pointer.
  constexpr unsigned samples=256;
  auto bench=[&](bool shared) {
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<samples;++i) {
      xgpu::Execution e(domain,{},xgpu::ExecutionProfile::C12);
      if(shared) e.initialize_global_shared(after.field);
      else e.initialize_global(*after.field);
      need(e.global().at(42)==9000,"C12 initializer changed data");
      if(shared) need(&e.global().field()==after.field.get(),"C12 lost shared pointer");
    }
    return std::chrono::duration<double,std::micro>(
        std::chrono::steady_clock::now()-start).count()/samples;
  };
  const auto copy_us=bench(false), shared_us=bench(true);

  std::cout << "{\"c12_shared_field\":\"PASS\",\"cpu_xi\":true"
    << ",\"gpu_c12\":true,\"qssd_c05\":true"
    << ",\"classical_logical_words\":" << domain
    << ",\"direct_dense_words_materialized\":0"
    << ",\"qssd_checkpoint_bytes\":" << bytes.size()
    << ",\"descriptor_copy_initialization_mean_us\":" << copy_us
    << ",\"shared_initialization_mean_us\":" << shared_us
    << ",\"benchmark_samples\":" << samples
    << ",\"physical_gpu_tested\":false}\n";
}
void quantum() {
  qssd::Space space;
  space.execute_cpu("seed 19\nh 100\nt 100\ncx 100 200\n");
  space.publish_quantum_byte("xi-quantum","5000","100");
  space.bind("cpu:quantum","xi-quantum");
  space.route("cpu:quantum","gpu:quantum");
  qssd::QuantumGpuView old(space.acquire("gpu:quantum"));
  need(!old.quantum_byte().deterministic &&
       old.quantum_byte().handle=="qbyte:5000@100",
       "quantum input collapsed or lost original XI memory binding");
  need(std::abs(old.expectation({{"100",'Z'},{"200",'Z'}})-1.0)<1e-12,
       "quantum GPU view lost the entangled correlation");
  const auto receipts=space.teleport_quantum_byte("gpu:quantum","100","300","400");
  need(receipts.size()==8,"C05 Bell receipt missing qubits");
  qssd::QuantumGpuView current(space.acquire("gpu:quantum"));
  need(!current.quantum_byte().deterministic &&
       current.quantum_byte().handle=="qbyte:5000@400",
       "Bell handoff did not update GPU quantum port");
  need(std::abs(current.expectation({{"400",'Z'},{"200",'Z'}})-1.0)<1e-12,
       "simulated Bell handoff lost entanglement");
  need(old.quantum_byte().handle=="qbyte:5000@100" &&
       std::abs(old.expectation({{"100",'Z'},{"200",'Z'}})-1.0)<1e-12,
       "old quantum GPU view changed after state relocation");
  const auto saved=space.checkpoint();
  const auto restored=qssd::Space::restore(saved);
  qssd::QuantumGpuView resumed(restored.acquire("gpu:quantum"));
  need(resumed.quantum_byte().handle=="qbyte:5000@400" &&
       std::abs(resumed.expectation({{"400",'Z'},{"200",'Z'}})-1.0)<1e-12,
       "restored simulated quantum state lost original XI correlations");
  std::cout << "{\"quantum_gpu_port\":\"PASS\",\"bell_qubits\":8,"
    << "\"measurements_classical\":16,"
    << "\"quantum_observer_execution\":\"host-XI-Ether\","
    << "\"quantum_gpu_gate_execution\":false,"
    << "\"physical_teleportation\":false}\n";
}
}
int main() {
  try{
    classical();quantum();
    std::cout << "{\"integration\":\"PASS\",\"checks\":"<<checks<<"}\n";
    return 0;
  }catch(const std::exception& e){
    std::cerr<<"QSSD_C12_INTEGRATION_FAIL: "<<e.what()<<'\n';
    return 1;
  }
}
