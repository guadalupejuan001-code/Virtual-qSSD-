#pragma once
#include "state_bus.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace qssd {
// An API-device projection owned by the qSSD component. Geometry remains owned
// by the original native SharedProjection/XI authority; its complete key is
// retained here, not a second scene or a generated copy of the world's state.
struct GraphicsSource {
  std::uint64_t session=0, identity=0, revision=0;
  bool operator==(const GraphicsSource& b) const {
    return session==b.session && identity==b.identity && revision==b.revision;
  }
};
using GraphicsPalette=std::array<std::uint32_t,256>;
using GraphicsAtlas=std::array<std::uint8_t,2048>;
struct GraphicsResources {
  GraphicsSource source;
  std::shared_ptr<const GraphicsPalette> palette;
  std::shared_ptr<const GraphicsAtlas> tile_atlas, device_atlas;
  std::uint64_t palette_revision=0, atlas_revision=0;
};
class GraphicsPort {
 public:
  using Bus=StateBus<GraphicsResources>;
  using View=Bus::View;
  struct Span {std::uint32_t first,count;};
  struct Plan {
    View target,base;
    std::uint64_t context=0;
    bool geometry_dirty=false;
    std::uint16_t atlas_rows=0;
    std::vector<Span> palette_spans;
  };
  struct Counters {
    std::uint64_t publications=0,reused=0,confirmed=0,atlas_relayouts=0;
  };
 private:
  Bus bus_{GraphicsResources{}};
  mutable std::mutex mutex_;
  View resident_;
  std::uint64_t context_=1;
  Counters counters_;
  static void need(bool ok,const char* why){
    if(!ok)throw std::invalid_argument(why);
  }
  static std::uint64_t next(std::uint64_t n){
    if(n>=std::uint64_t(INT64_MAX))throw std::overflow_error("qSSD graphics revision exhausted");
    return n+1;
  }
 public:
  // Raw atlas is the original XFG4 tile-major 2048-byte window. Borrow it only
  // during this call. Allocate/reorder it ONLY after exact inequality is known.
  View publish(GraphicsSource source,const GraphicsPalette& palette,
               const std::uint8_t* tile_atlas,std::size_t size){
    need(source.session && source.identity && source.revision,"qSSD graphics source lacks its native lease");
    need(tile_atlas && size==2048,"qSSD graphics requires original XFG4 atlas");
    for(std::size_t i=0;i<size;++i)need(tile_atlas[i]<16,"qSSD atlas nibble outside XFG4");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto old=bus_.snapshot();const auto& previous=*old->value;
    if(previous.source.session){
      need(source.session>=previous.source.session,"qSSD graphics source from obsolete session");
      if(source.session==previous.source.session){
        need(source.identity>=previous.source.identity,"qSSD graphics source from obsolete identity");
        if(source.identity==previous.source.identity)
          need(source.revision>=previous.source.revision,"qSSD graphics revision moved backwards");
      }
    }
    const bool palette_changed=!previous.palette || palette!=*previous.palette;
    const bool atlas_changed=!previous.tile_atlas ||
      !std::equal(tile_atlas,tile_atlas+size,previous.tile_atlas->begin());
    if(source==previous.source && !palette_changed && !atlas_changed){++counters_.reused;return old;}
    GraphicsResources value=previous;value.source=source;
    if(palette_changed){
      value.palette=std::make_shared<const GraphicsPalette>(palette);
      value.palette_revision=next(previous.palette_revision);
    }
    if(atlas_changed){
      auto original=std::make_shared<GraphicsAtlas>();std::copy_n(tile_atlas,size,original->begin());
      auto device=std::make_shared<GraphicsAtlas>();
      for(std::size_t row=0;row<16;++row)for(std::size_t tile=0;tile<8;++tile)
        std::copy_n(tile_atlas+tile*256+row*16,16,device->begin()+row*128+tile*16);
      value.tile_atlas=std::move(original);value.device_atlas=std::move(device);
      value.atlas_revision=next(previous.atlas_revision);
    }
    const auto result=bus_.publish(std::move(value),"graphics-source",old);
    ++counters_.publications;if(atlas_changed)++counters_.atlas_relayouts;
    return result;
  }
  View at(std::uint64_t serial) const {return bus_.at(serial);}
  Plan plan(const View& target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    need(target && target->authority==bus_.snapshot()->authority && target->value->palette,
         "qSSD graphics foreign or uninitialized view");
    need(!resident_ || target->serial>=resident_->serial,"qSSD cannot upload older resource state");
    Plan p;p.target=target;p.base=resident_;p.context=context_;
    const auto& v=*target->value;
    p.geometry_dirty=!resident_ || !(v.source==resident_->value->source);
    if(!resident_ || v.palette!=resident_->value->palette){
      for(std::uint32_t i=0;i<256;){
        if(resident_ && (*v.palette)[i]==(*resident_->value->palette)[i]){++i;continue;}
        const auto begin=i++;
        while(i<256 && (!resident_ || (*v.palette)[i]!=(*resident_->value->palette)[i]))++i;
        p.palette_spans.push_back({begin,i-begin});
      }
    }
    if(!resident_ || v.device_atlas!=resident_->value->device_atlas){
      for(unsigned row=0;row<16;++row){
        const auto a=v.device_atlas->begin()+row*128;
        if(!resident_ || !std::equal(a,a+128,resident_->value->device_atlas->begin()+row*128))
          p.atlas_rows|=std::uint16_t(1u<<row);
      }
    }
    return p;
  }
  // Confirms host API acceptance only, not completion of GPU execution. A GPU
  // fence must be used separately if device-completion measurements are needed.
  void confirm(const Plan& plan){
    std::lock_guard<std::mutex> lock(mutex_);
    need(plan.target && plan.target->authority==bus_.snapshot()->authority,
         "qSSD confirmation belongs to another component");
    need(plan.context==context_ && plan.base==resident_,"qSSD stale upload receipt");
    need(!resident_ || plan.target->serial>=resident_->serial,"qSSD upload confirmation out of order");
    resident_=plan.target;++counters_.confirmed;
  }
  // An interrupted upload can have changed an unknown subset of GPU memory.
  // Invalidate the entire physical replica: retry must be FULL, not a diff
  // against the pre-failure copy. Logical native qSSD resources remain intact.
  void invalidate_device(){
    std::lock_guard<std::mutex> lock(mutex_);
    context_=next(context_);resident_.reset();
  }
  Counters counters() const {std::lock_guard<std::mutex> lock(mutex_);return counters_;}
};
} // namespace qssd
