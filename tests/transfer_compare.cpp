#include "qssd/state_space.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void verify(bool good, const char* message) {
  if (!good) throw std::runtime_error(message);
}
struct Case {
  std::uint64_t words;
  std::uint64_t handoffs;
  double ref_us;
  double copy_us;
  std::uint64_t dense_bytes_copied;
  std::size_t checkpoint_bytes;
};
Case run_case(std::uint64_t domain) {
  constexpr unsigned pairs=256;
  xgpu::Machine machine(xgpu::example(domain)); machine.run();
  qssd::Space space;
  space.publish_gpu("payload", machine);
  space.bind("gpu:source", "payload");
  const auto original = space.acquire("gpu:source");
  const auto* const original_address = original.field.get();
  std::vector<xgpu::Word> src, dst;
  src.reserve(domain);
  for(std::uint64_t i=0;i<domain;++i) src.push_back(original.at(i));
  const auto native = space.acquire("gpu:source");

  const auto started = std::chrono::steady_clock::now();
  for (unsigned i=0;i<pairs;++i) {
    space.route("gpu:source","ssd:destination",true);
    space.route("ssd:destination","gpu:source",true);
  }
  const auto routed_us = std::chrono::duration<double,std::micro>(
      std::chrono::steady_clock::now()-started).count();
  const auto latest = space.acquire("gpu:source");
  verify(latest.field.get()==original_address && latest.identity==native.identity,
         "QSSD reference handoff copied or replaced data");
  verify(space.observed_words()==0 && space.projected_cpu_bytes()==0,
         "QSSD routing materialized the domain");

  const auto started_copy = std::chrono::steady_clock::now();
  for (unsigned i=0;i<pairs;++i) {
    dst = src;
    src = dst;
  }
  const auto copy_us = std::chrono::duration<double,std::micro>(
      std::chrono::steady_clock::now()-started_copy).count();

  for (auto index : {std::uint64_t(0),std::uint64_t(1),domain/2,domain-1})
    verify(src.at(index)==latest.at(index), "dense and shared state mismatch");
  verify(space.routed_references()==2u*pairs, "wrong QSSD route count");

  return {domain,2u*pairs,routed_us,copy_us,
          2u*pairs*domain*sizeof(xgpu::Word),space.checkpoint().size()};
}
}
int main() {
  try {
    const auto small=run_case(1024);
    const auto large=run_case(65536);
    const auto print=[](const Case& c) {
      std::cout << "{\"logical_words\":" << c.words
        << ",\"handoffs\":" << c.handoffs
        << ",\"qssd_reference_us\":" << c.ref_us
        << ",\"dense_cpu_copy_us\":" << c.copy_us
        << ",\"dense_payload_bytes_copied\":" << c.dense_bytes_copied
        << ",\"qssd_dense_words_materialized\":0"
        << ",\"qssd_checkpoint_bytes\":" << c.checkpoint_bytes << "}";
    };
    std::cout << "{\"scope\":\"host-software-read-only-logical-handoff\""
      << ",\"physical_gpu_tested\":false,\"quantum_hardware_tested\":false"
      << ",\"different_ownership_semantics\":true,\"cases\":[";
    print(small);std::cout << ",";print(large);std::cout << "]}\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "QSSD_COMPARE_FAIL: " << error.what() << '\n';
    return 1;
  }
}
