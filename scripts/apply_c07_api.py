#!/usr/bin/env python3
"""One-time reviewed C07 API migration; idempotent, no upstream core replacement."""
from pathlib import Path
p=Path('include/qssd/state_space.hpp')
s=p.read_text()
a='  void publish_gpu(const std::string& name, const xgpu::Machine& source) {'
addition='''  // Import an explicitly classical producer descriptor into this component.
  // Ownership is transferred into an immutable object; C12 consumers acquire
  // that SAME object through the existing CPU/SSD/GPU ports. No dense output
  // expansion, implicit quantum measurement, or duplicate device store.
  void publish_field(const std::string& name, Field field) {
    valid_name(name); field.validate();
    auto immutable = std::make_shared<const Field>(std::move(field));
    transition([&](State& state) { publish(state, name, immutable); return true; });
  }
'''
if addition not in s:
    if s.count(a)!=1: raise SystemExit('Unreviewed QSSD API anchor')
    p.write_text(s.replace(a,addition+a))
p=Path('CMakeLists.txt');s=p.read_text();anchor='if(BUILD_TESTING)\n'
addition='''  add_executable(qssd_graphics_tests tests/graphics_port.cpp)
  target_link_libraries(qssd_graphics_tests PRIVATE qssd)
  add_test(NAME qssd_native_graphics_port COMMAND qssd_graphics_tests)
  add_executable(qssd_imported_field_tests tests/imported_field.cpp)
  target_link_libraries(qssd_imported_field_tests PRIVATE qssd)
  add_test(NAME qssd_external_producer_c12 COMMAND qssd_imported_field_tests)
'''
if addition not in s:
    if s.count(anchor)!=1: raise SystemExit('Unreviewed QSSD CTest anchor')
    p.write_text(s.replace(anchor,anchor+addition))
