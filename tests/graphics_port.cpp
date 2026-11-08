#include "qssd/graphics_port.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <stdexcept>
static unsigned checks=0;
static void need(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F> static void reject(F f,const char* why){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,why);}
int main(){
  qssd::GraphicsPort device;
  qssd::GraphicsPalette colors{};colors.fill(0x335577);
  qssd::GraphicsAtlas atlas{};atlas[2*256+4*16+3]=2;
  auto first=device.publish({100,10,1},colors,atlas.data(),atlas.size());
  auto p=device.plan(first);
  need(p.geometry_dirty&&p.palette_spans.size()==1&&p.palette_spans[0].count==256&&p.atlas_rows==65535,"initial full projection");
  need((*first->value->device_atlas)[4*128+2*16+3]==2,"native atlas layout");
  device.confirm(p);
  auto repeated=device.publish({100,10,1},colors,atlas.data(),atlas.size());
  need(first==repeated,"unchanged source must reuse original native StateBus view");
  p=device.plan(repeated);need(!p.geometry_dirty&&p.palette_spans.empty()&&!p.atlas_rows,"unchanged frame has no upload");
  auto moved=device.publish({100,10,2},colors,atlas.data(),atlas.size());
  need(moved->value->device_atlas==first->value->device_atlas,"moving geometry copied static atlas");
  need(device.counters().atlas_relayouts==1,"moving geometry relaid static atlas");
  colors[5]=0xabcdef;colors[6]=0x123456;colors[15]=0x998877;
  atlas[2*256+4*16+3]=3;
  auto changed=device.publish({100,10,3},colors,atlas.data(),atlas.size());
  need((*first->value->palette)[5]==0x335577,"old native lease changed");
  p=device.plan(changed);
  need(p.palette_spans.size()==2&&p.palette_spans[0].first==5&&p.palette_spans[0].count==2&&p.palette_spans[1].first==15,"native sparse palette spans");
  need(p.atlas_rows==(1u<<4),"native one row delta");
  device.invalidate_device();
  reject([&]{device.confirm(p);},"accepted receipt from failed context");
  p=device.plan(changed);
  need(p.geometry_dirty&&p.palette_spans.size()==1&&p.palette_spans[0].count==256&&p.atlas_rows==65535,"failed transfer retry was not full");
  device.confirm(p);
  reject([&]{device.confirm(p);},"accepted duplicate acknowledgment");
  reject([&]{device.publish({100,10,2},colors,atlas.data(),atlas.size());},"accepted stale native revision");
  auto restored=device.publish({101,10,1},colors,atlas.data(),atlas.size());
  need(device.plan(restored).geometry_dirty,"restored session reused old geometry");
  qssd::GraphicsPort other;
  reject([&]{other.plan(restored);},"accepted foreign QSSD view");
  std::atomic<bool> ok{true};
  std::thread reader([&]{for(unsigned i=0;i<10000;++i)if((*first->value->palette)[5]!=0x335577)ok=false;});
  for(unsigned i=2;i<102;++i)device.publish({101,10,i},colors,atlas.data(),atlas.size());
  reader.join();need(ok,"lease torn under concurrent publication");
  need(device.counters().atlas_relayouts==2,"static atlas rebuilt after repeated scene revisions");
  std::cout<<"QSSD_NATIVE_GRAPHICS_PASS checks="<<checks<<" source=original_StateBus failed_upload_full_retry=true concurrent_immutable=true\n";
}
