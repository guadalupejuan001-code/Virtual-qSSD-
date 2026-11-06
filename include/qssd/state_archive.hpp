#pragma once
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace qssd {
// Explicit cold checkpoint. Equality is byte-for-byte; hash buckets only speed
// lookup. Aliased sections restore to the same immutable blob. No hot-path I/O.
class StateArchive {
 public:
  // Compatibility names describe the machine's size representation, not a
  // qSSD quota. Containers allocate on demand and report allocation failures.
  static constexpr std::size_t max_bytes=std::numeric_limits<std::size_t>::max(),max_sections=max_bytes;
  using Blob=std::shared_ptr<const std::string>;
  using Entries=std::map<std::string,Blob>;
 private:
  Entries entries_;
  static void key(const std::string& name){
    if(name.empty())throw std::invalid_argument("qSSD archive key");
    for(unsigned char c:name)if(c<33||c>126)throw std::invalid_argument("qSSD archive key character");
  }
  static std::uint64_t digest(const std::string& bytes,std::size_t n){
    std::uint64_t h=14695981039346656037ULL;
    for(std::size_t i=0;i<n;++i){h^=static_cast<unsigned char>(bytes[i]);h*=1099511628211ULL;}return h;
  }
 public:
  void add(const std::string& name,Blob value){
    key(name);if(!value)throw std::invalid_argument("qSSD null archive blob");
    if(!entries_.emplace(name,std::move(value)).second)throw std::invalid_argument("qSSD duplicate archive section");
  }
  void add(const std::string& name,std::string value){add(name,std::make_shared<const std::string>(std::move(value)));}
  const std::string& get(const std::string& name)const{return *entries_.at(name);}
  Blob blob(const std::string& name)const{return entries_.at(name);}
  bool contains(const std::string& name)const{return entries_.count(name)!=0;}
  const Entries& entries()const{return entries_;}
  std::string encode()const{
    std::vector<Blob> blobs;std::vector<std::size_t> refs;
    std::unordered_map<std::uint64_t,std::vector<std::size_t>> buckets;
    for(const auto& item:entries_){
      const auto& value=item.second;auto& bucket=buckets[digest(*value,value->size())];std::size_t id=blobs.size();
      for(auto prior:bucket)if(*blobs[prior]==*value){id=prior;break;}
      if(id==blobs.size()){
        bucket.push_back(id);blobs.push_back(value);
      }refs.push_back(id);
    }
    std::string out="QSB3";
    auto u64=[&](std::uint64_t value){
      if(out.max_size()-out.size()<8)throw std::length_error("qSSD archive size representation exhausted");
      for(unsigned i=0;i<8;i++)out.push_back(static_cast<char>(value>>(8*i)));
    };
    auto text=[&](const std::string& value){
      if(out.max_size()-out.size()<8||value.size()>out.max_size()-out.size()-8)
        throw std::length_error("qSSD archive size representation exhausted");
      u64(value.size());out+=value;
    };
    u64(blobs.size());for(const auto& value:blobs)text(*value);
    u64(entries_.size());std::size_t i=0;
    for(const auto& item:entries_){text(item.first);u64(refs[i++]);}
    auto hash=digest(out,out.size());u64(hash);return out;
  }
  static StateArchive decode(const std::string& bytes){
    if(bytes.size()<28)throw std::length_error("qSSD truncated archive");
    const auto end=bytes.size()-8;std::size_t at=0;
    auto u64=[&](){
      if(at>end||end-at<8)throw std::invalid_argument("qSSD truncated archive");
      std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(static_cast<unsigned char>(bytes[at++]))<<(8*i);return v;
    };
    std::uint64_t checksum=0;for(unsigned i=0;i<8;++i)checksum|=std::uint64_t(static_cast<unsigned char>(bytes[end+i]))<<(8*i);
    if(checksum!=digest(bytes,end)||bytes.compare(0,4,"QSB3"))throw std::invalid_argument("qSSD archive signature/checksum");
    auto text=[&](){auto n=u64();if(at>end||n>end-at)throw std::length_error("qSSD archive field bounds");auto s=bytes.substr(at,static_cast<std::size_t>(n));at+=static_cast<std::size_t>(n);return s;};
    // Validate counts against the bytes actually present before allocating or
    // iterating. An encoded blob needs at least its eight-byte length; a
    // section needs a length, a nonempty name and an eight-byte reference.
    at=4;auto count=u64();
    if(count>(end-at)/8)throw std::length_error("qSSD archive truncated blob table");
    std::vector<Blob> blobs;for(std::uint64_t i=0;i<count;i++)blobs.push_back(std::make_shared<const std::string>(text()));
    count=u64();
    if(count>(end-at)/17)throw std::length_error("qSSD archive truncated section table");
    StateArchive result;std::set<std::uint64_t> used;
    for(std::uint64_t i=0;i<count;i++){
      auto name=text();auto id=u64();if(id>=blobs.size())throw std::invalid_argument("qSSD archive dangling blob");
      used.insert(id);result.add(name,blobs[static_cast<std::size_t>(id)]);
    }
    if(at!=end||used.size()!=blobs.size())throw std::invalid_argument("qSSD trailing/unreferenced archive data");
    return result;
  }
};
} // namespace qssd
