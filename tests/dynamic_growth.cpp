#include "qssd/state_bus.hpp"
#include "qssd/state_archive.hpp"
#include "qssd/files.hpp"
#include <iostream>

namespace {
std::size_t checks=0;
void need(bool condition){++checks;if(!condition)throw std::runtime_error("qSSD dynamic growth contract failed");}
template<class Operation>void rejects(Operation operation){
  bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}need(rejected);
}
// Recompute the wire checksum so malformed-count cases reach the parser.
void set_count(std::string& bytes,std::size_t offset,std::uint64_t count){
  for(unsigned i=0;i<8;i++)bytes[offset+i]=static_cast<char>(count>>(8*i));
  std::uint64_t digest=14695981039346656037ULL;
  for(std::size_t i=0;i<bytes.size()-8;i++){digest^=static_cast<unsigned char>(bytes[i]);digest*=1099511628211ULL;}
  for(unsigned i=0;i<8;i++)bytes[bytes.size()-8+i]=static_cast<char>(digest>>(8*i));
}
void history(){
  qssd::StateBus<std::uint64_t> bus(0);
  const auto first=bus.snapshot();
  for(unsigned i=0;i<10000;i++)bus.update([](auto& state){++state;return true;},"advance");
  need(bus.stats().history==10001&&bus.stats().historyCapacity==0);
  need(bus.at(first->serial)==first&&*bus.at(4097)->value==4096);
  for(unsigned i=0;i<1000;i++)bus.bookmark("point-"+std::to_string(i),bus.at(i+1));
  need(bus.stats().bookmarks==1000&&*bus.bookmark("point-999")->value==999);
  auto branch=bus.fork(first);
  for(unsigned i=0;i<5000;i++)branch.update([](auto& state){++state;return true;},"branch");
  need(branch.stats().history==5001&&branch.stats().historyCapacity==0);
  need(branch.at(1)->value==first->value&&*bus.snapshot()->value==10000);
  const auto rewound=bus.rewind(first->serial);
  need(rewound->value==first->value&&bus.stats().history==10002&&bus.at(10001)->serial==10001);
  const std::string long_name(256,'x');bus.bookmark(long_name,first);
  need(bus.bookmark(long_name)==first);
  bus.update([](auto& state){++state;return true;},std::string(256,'r'));
  auto before=bus.snapshot();
  rejects([&]{bus.update([](auto& state)->bool{++state;throw std::bad_alloc();},"allocation failure");});
  need(bus.snapshot()==before&&bus.stats().history==10003);

  // Existing consumers may explicitly request bounded retention. There is no
  // upper qSSD quota and no upfront allocation proportional to the request.
  qssd::StateBus<unsigned> selected(0,5000);
  auto retained=selected.snapshot();
  for(unsigned i=0;i<5100;i++)selected.update([](auto& state){++state;return true;},"caller retention");
  need(selected.stats().history==5000&&selected.stats().historyCapacity==5000);
  rejects([&]{selected.at(1);});need(*retained->value==0);
  auto bounded_branch=selected.fork();need(bounded_branch.stats().historyCapacity==5000);
  qssd::StateBus<unsigned> no_preallocation(0,std::numeric_limits<std::size_t>::max());
  need(no_preallocation.stats().history==1);
}
void archive(){
  qssd::StateArchive archive;
  const std::string long_name(256,'k');archive.add(long_name,"long-key");
  for(unsigned i=0;i<600;i++)archive.add("section-"+std::to_string(i),"unique-value-"+std::to_string(i));
  auto small=qssd::StateArchive::decode(archive.encode());
  need(small.entries().size()==601&&small.get("section-599")=="unique-value-599"&&small.get(long_name)=="long-key");
  rejects([&]{archive.add("null",qssd::StateArchive::Blob{});});
  auto malformed=qssd::StateArchive{}.encode();set_count(malformed,4,UINT64_MAX);
  rejects([&]{qssd::StateArchive::decode(malformed);});
  malformed=qssd::StateArchive{}.encode();set_count(malformed,12,UINT64_MAX);
  rejects([&]{qssd::StateArchive::decode(malformed);});
  qssd::StateArchive one;one.add("a","b");
  malformed=one.encode();set_count(malformed,12,UINT64_MAX);
  rejects([&]{qssd::StateArchive::decode(malformed);});

  constexpr std::size_t old_cap=64u*1024*1024;
  auto data=std::make_shared<const std::string>(old_cap+1,'z');
  archive.add("large",data);archive.add("large-alias",data);
  const auto encoded=archive.encode();
  need(encoded.size()>old_cap&&encoded.size()<data->size()+100000);
  auto recovered=qssd::StateArchive::decode(encoded);
  need(recovered.get("large")==*data&&recovered.blob("large")==recovered.blob("large-alias"));
  need(recovered.entries().size()==603&&recovered.get("section-599")=="unique-value-599");
  // Exercise the default disk path as well as the in-memory codec.
  char pattern[]="/tmp/qssd-growth-XXXXXX";
  const auto directory=::mkdtemp(pattern);if(!directory)throw std::runtime_error("temporary directory");
  const auto path=std::filesystem::path(directory)/"large.qsb";
  try{
    qssd::save_new(path,encoded);
    need(qssd::read_file(path)==encoded);
    rejects([&]{qssd::read_file(path,old_cap);});
    std::filesystem::remove_all(directory);
  }catch(...){std::filesystem::remove_all(directory);throw;}
}
}
int main(){try{
  history();archive();
  std::cout<<"{\"dynamic_growth\":\"PASS\",\"checks\":"<<checks
    <<",\"retained_revisions\":10003,\"bookmarks\":1001,\"archive_sections\":603"
    <<",\"archive_payload_bytes\":67108865,\"arbitrary_qssd_quotas\":false}\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
