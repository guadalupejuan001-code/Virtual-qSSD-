#pragma once
#include "qssd/state_bus.hpp"
#include <cstdint>

namespace qssd {
// Versioned graphics dependency authority. Stores only dependency identities,
// compact-description sizes and reuse evidence; physical driver resources stay
// owned by the renderer.
struct GraphicsAuthorityState {
  std::uint64_t source_revision=0;
  std::uint64_t relation_structure_revision=0;
  std::uint64_t relation_value_revision=0;
  std::uint64_t camera_hash=0,light_hash=0,material_hash=0,geometry_hash=0;
  std::uint64_t baseline_materialized_bytes=0;
  std::uint64_t compact_descriptor_bytes=0;
  std::uint64_t reused_state_bytes=0;
  bool relation_reused=false;
};
using GraphicsAuthorityBus=StateBus<GraphicsAuthorityState>;
} // namespace qssd
