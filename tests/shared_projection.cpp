#include "qssd/shared_projection.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
  try {
    qssd::SharedProjection<std::string> space;
    auto need = [](bool ok) { if (!ok) throw std::runtime_error("shared projection contract failed"); };
    std::string data(256 * 1024, 'a'); data[17] = 0;
    const auto first = space.publish(data);
    for (auto port : {qssd::ProjectionPort::CPU, qssd::ProjectionPort::SSD, qssd::ProjectionPort::GPU})
      need(space.acquire(port).value == first.value);
    need(space.publish(data).value == first.value);
    data[60001] = 'b'; const auto changed = space.publish(data);
    need(changed.identity == first.identity && changed.revision == first.revision + 1);
    need(first.value->at(60001) == 'a' && changed.value->at(60001) == 'b');
    std::vector<std::thread> readers;
    for (unsigned t = 0; t < 4; ++t) readers.emplace_back([&] {
      for (unsigned i = 0; i < 2000; ++i) {
        auto view = space.acquire(qssd::ProjectionPort::GPU);
        if (view.value->size() != data.size()) std::terminate();
      }
    });
    for (auto& reader : readers) reader.join();
    space.reset(); auto next = space.publish(data);
    need(next.identity != first.identity && next.revision == 1 && first.value->at(60001) == 'a');
    const auto stats = space.stats(); need(stats.publications == 3 && stats.unchanged == 1 && stats.acquisitions == 8003);
    std::cout << "{\"shared_projection\":\"PASS\",\"three_port_same_value\":true,\"retained_old_versions\":true,\"concurrent_acquisitions\":8000}\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
