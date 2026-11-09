#include "qssd/graphics_authority.hpp"
#include <cassert>
#include <iostream>
int main(){
  qssd::GraphicsAuthorityBus bus(qssd::GraphicsAuthorityState{},8);
  auto a=*bus.snapshot()->value;
  a.source_revision=1;a.geometry_hash=41;a.relation_structure_revision=7;
  a.compact_descriptor_bytes=32;
  auto v1=bus.publish(a,"frame-1");
  auto b=*v1->value;
  b.source_revision=2;b.camera_hash=99;b.relation_reused=true;b.reused_state_bytes=32;
  auto v2=bus.publish(b,"camera-only");
  assert(v2->value->geometry_hash==41);
  assert(v2->value->relation_structure_revision==7);
  assert(v2->value->relation_reused&&v2->value->reused_state_bytes==32);
  assert(bus.stats().serial>=2);
  std::cout<<"graphics authority delta 6/6 passed\n";
}
