#pragma once
#include "qssd/state_bus.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace qssd {
// Persistent immutable graphics-work memory built on the original StateBus.
// The plan is a component state, not a file cache. Publishing creates a new
// revision; unchanged consumers may retain older immutable leases safely.
template<class Plan>
class GraphicsWorkMemory {
 public:
  struct State { Plan plan{}; std::uint64_t dependency_revision=0; std::uint64_t completed_revision=0; };
  explicit GraphicsWorkMemory(std::size_t history=4):bus_(State{},history){}
  void reset(std::size_t history=4){bus_=StateBus<State>(State{},history);}
  auto publish(Plan plan,std::uint64_t dependency,std::uint64_t completed=0){
    State s; s.plan=std::move(plan);s.dependency_revision=dependency;s.completed_revision=completed;
    return bus_.publish(std::move(s),"graphics-work-plan");
  }
  auto snapshot() const {return bus_.snapshot();}
  auto stats() const {return bus_.stats();}
 private: StateBus<State> bus_;
};
} // namespace qssd
