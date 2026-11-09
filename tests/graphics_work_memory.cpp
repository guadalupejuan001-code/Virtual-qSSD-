#include "qssd/graphics_work_state.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
struct Plan{std::uint64_t revision=0,bytes=0;};
int main(){
 qssd::GraphicsWorkMemory<Plan> m(4);
 auto z=m.snapshot();assert(z&&z->value->completed_revision==0);
 Plan p{7,320};auto a=m.publish(p,900,0);
 assert(a->value->plan.revision==7&&a->value->dependency_revision==900);
 p={8,160};auto b=m.publish(p,901,42);
 assert(b->value->completed_revision==42&&b->value->plan.bytes==160);
 assert(a->value->plan.bytes==320);
 assert(m.stats().serial>=2);
 m.reset(8);assert(m.snapshot()->value->completed_revision==0);
 std::cout<<"graphics work memory passed\n";
}
