#pragma once
// Included after State/Space; codec uses only the original state_io primitives.
namespace qssd {
inline std::string Space::checkpoint() const {
  const auto state = epoch();
  reverse_pc::state_io::Writer w;
  w.string("XQSSD1"); w.u64(state->revision); w.u64(state->next_id);
  const auto cpu = reverse_pc::rvpc2::save(state->cpu); w.string(std::string(cpu.begin(), cpu.end()));
  std::vector<FieldPtr> fields;
  for (const auto& [name, object] : state->objects) {
    (void)name;
    if (object.field && std::find(fields.begin(), fields.end(), object.field) == fields.end()) fields.push_back(object.field);
  }
  w.u64(fields.size());
  for (const auto& field : fields) {
    w.u64(field->count); w.u64(field->bands.size());
    for (const auto& band : field->bands) {
      w.u64(band.begin); w.u64(band.end); w.u64(band.mode.coefficients.size());
      for (auto value : band.mode.coefficients) w.u64(value);
    }
    w.u64(field->details.size());
    for (const auto& [index, value] : field->details) { w.u64(index); w.u64(value); }
  }
  w.u64(state->objects.size());
  for (const auto& [name, object] : state->objects) {
    w.string(name); w.u64(object.id); w.u64(object.revision);
    w.byte(object.field ? 0 : 1);
    if (object.field) w.u64(static_cast<std::size_t>(std::find(fields.begin(), fields.end(), object.field) - fields.begin()));
    else w.string(object.quantum_address);
  }
  w.u64(state->ports.size());
  for (const auto& [port, object] : state->ports) { w.string(port); w.string(object); }
  return w.finish();
}
inline Space Space::restore(const std::string& bytes) {
  reverse_pc::state_io::Reader r(bytes, max_checkpoint_bytes);
  if (r.string(6) != "XQSSD1") throw std::invalid_argument("qSSD checkpoint version");
  auto state = std::make_shared<State>(); state->revision = r.u64(); state->next_id = r.u64();
  const auto cpu = r.string(reverse_pc::rvpc2::max_state + 29);
  if (cpu.compare(0, 5, "RVPC2") != 0) throw std::invalid_argument("qSSD missing native CPU authority");
  state->cpu = reverse_pc::rvpc2::restore({cpu.begin(), cpu.end()});
  auto bounded = [&](std::size_t cap) { auto n = r.u64(); if (n > cap) throw std::length_error("qSSD checkpoint dimensions"); return n; };
  auto records = [&](std::size_t minimum) {
    auto n = r.u64();
    if (n > (r.end - r.at) / minimum) throw std::length_error("qSSD truncated checkpoint table");
    return n;
  };
  // Bound table counts by their actual payload, never by an arbitrary quota.
  const auto nf = records(24);
  std::vector<FieldPtr> fields;
  for (std::size_t i = 0; i < nf; ++i) {
    const auto count = bounded(xgpu::domain_limit), bands = bounded(xgpu::max_bands);
    Field field(count); field.bands.clear();
    for (std::size_t b = 0; b < bands; ++b) {
      const auto begin = bounded(count), end = bounded(count), nc = bounded(xgpu::max_degree + 1);
      if (!nc) throw std::invalid_argument("qSSD empty coefficient list");
      std::vector<xgpu::Word> c;
      for (std::size_t j = 0; j < nc; ++j) c.push_back(static_cast<xgpu::Word>(bounded(UINT32_MAX)));
      if (c.size() > 1 && c.back() == 0) throw std::invalid_argument("qSSD noncanonical mode");
      field.bands.push_back({begin, end, xgpu::Mode(std::move(c))});
    }
    field.validate();
    const auto nd = bounded(xgpu::max_details);
    for (std::size_t j = 0; j < nd; ++j) {
      const auto index = bounded(count - 1), value = bounded(UINT32_MAX);
      if (!field.details.emplace(index, static_cast<xgpu::Word>(value)).second) throw std::invalid_argument("qSSD duplicate detail");
    }
    field.validate(); fields.push_back(std::make_shared<const Field>(std::move(field)));
  }
  const auto no = records(34);
  std::set<std::uint64_t> identities;
  std::set<std::size_t> used_fields;
  for (std::size_t i = 0; i < no; ++i) {
    const auto name = r.string(bytes.size()); valid_name(name);
    const auto id = r.u64(), revision = r.u64();
    const auto kind = r.byte();
    if (!id || id >= state->next_id || !revision || revision > state->revision || kind > 1 || !identities.insert(id).second)
      throw std::invalid_argument("qSSD invalid identity/revision/field");
    FieldPtr field;
    std::string quantum;
    if (kind == 0) {
      const auto index = r.u64();
      if (index >= fields.size()) throw std::invalid_argument("qSSD invalid field reference");
      field = fields[index]; used_fields.insert(index);
    } else {
      quantum = r.string(4096);
      if (address_offset(quantum, 0) != quantum) throw std::invalid_argument("qSSD noncanonical quantum address");
    }
    if (!state->objects.emplace(name, Object{id, revision, field, quantum}).second) throw std::invalid_argument("qSSD duplicate identity");
  }
  if (!state->next_id || used_fields.size() != fields.size()) throw std::invalid_argument("qSSD invalid field topology");
  const auto np = records(22);
  for (std::size_t i = 0; i < np; ++i) {
    const auto port = r.string(bytes.size()), object = r.string(bytes.size()); valid_port(port); valid_name(object);
    if (!state->objects.count(object) || !state->ports.emplace(port, object).second) throw std::invalid_argument("qSSD dangling/duplicate port");
  }
  r.done();
  Space result; std::atomic_store(&result.control_->head, std::shared_ptr<const State>(std::move(state))); return result;
}
}  // namespace qssd
