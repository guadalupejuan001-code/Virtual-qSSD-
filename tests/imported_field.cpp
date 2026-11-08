#include "qssd/c12_bridge.hpp"
#include <iostream>
#include <stdexcept>
static void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){
  qssd::Space component;
  xgpu::Field input(64,xgpu::Mode({7,3}));
  component.publish_field("producer",std::move(input));
  component.bind("cpu:producer","producer");
  component.route("cpu:producer","ssd:producer");
  component.route("cpu:producer","gpu:producer");
  const auto before=component.acquire("gpu:producer");
  need(before.field==component.acquire("cpu:producer").field &&
       before.field==component.acquire("ssd:producer").field,"original ports copied field");
  using Op=xgpu::StateOp;
  const xgpu::Kernel kernel{{{Op::Index,0},{Op::LoadGlobal,1,0},{Op::Constant,2,0,0,0,5},
                            {Op::Add,3,1,2},{Op::Output,3},{Op::Halt}}};
  qssd::C12Window old(before);old.enqueue(kernel,64);
  need(old.run()&&old.source_pointer_preserved(),"C12Window lost producer pointer");
  for(unsigned i=0;i<64;++i)need(old.engine().dispatch(0).output->at(i)==12+3*i,"original C12 output mismatch");
  component.publish_field("producer",xgpu::Field(64,xgpu::Mode({100,1})));
  const auto current=component.acquire("gpu:producer");
  need(current.identity==before.identity&&current.revision==before.revision+1,"producer update lost qSSD identity");
  need(before.at(5)==22&&current.at(5)==105,"in-flight qSSD lease mutated");
  auto restored=qssd::Space::restore(component.checkpoint());
  qssd::C12Window resumed(restored.acquire("gpu:producer"));resumed.enqueue(kernel,64);
  need(resumed.run()&&resumed.engine().dispatch(0).output->at(5)==110,"imported field lost on checkpoint");
  std::cout<<"QSSD_IMPORTED_FIELD_PASS words=64 shared_pointer=true old_revision_pinned=true checkpoint=true\n";
}
