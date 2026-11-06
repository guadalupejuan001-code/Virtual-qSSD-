#include "qssd/state_bus.hpp"
#include "qssd/state_archive.hpp"
#include <atomic>
#include <iostream>
#include <thread>

struct Graph {
  std::uint64_t cpu=0,gpu=0;
  qssd::StateArchive::Blob resource=std::make_shared<const std::string>(1024*1024,'x');
};
int main(){try{
 std::uint64_t checks=0;
 auto need=[&](bool ok){if(!ok)throw std::runtime_error("complete state bus contract failed");++checks;};
 auto invalid=[&](auto operation){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}need(rejected);};
 qssd::StateBus<Graph> bus(Graph{},8,[](const Graph& state){if(state.gpu!=state.cpu*3)throw std::invalid_argument("torn CPU/GPU graph");});
 auto first=bus.snapshot();bus.bookmark("origin",first);auto client=bus;
 bus.update([](Graph& state){state.cpu=1;state.gpu=3;return true;},"cpu+gpu");
 need(client.snapshot()==bus.snapshot()&&first->value->cpu==0);
 need(bus.snapshot()->value->resource==first->value->resource);
 invalid([&]{bus.publish(Graph{},"stale",first);});
 auto before=bus.snapshot();invalid([&]{bus.update([](Graph& state){++state.cpu;return true;},"invalid");});need(bus.snapshot()==before);
 auto branch=bus.fork();branch.update([](Graph& state){state.cpu=7;state.gpu=21;return true;},"branch");
 need(branch.snapshot()->value->cpu==7&&bus.snapshot()->value->cpu==1&&branch.snapshot()->value->resource==first->value->resource);
 invalid([&]{branch.bookmark("foreign",first);});
 std::atomic<bool> bad{false},stop{false};
 std::thread reader([&]{while(!stop){auto v=client.snapshot();if(v->value->gpu!=v->value->cpu*3)bad=true;}});
 std::vector<std::thread> writers;
 for(unsigned t=0;t<4;t++)writers.emplace_back([&]{for(unsigned i=0;i<1000;i++)bus.update([](Graph& state){++state.cpu;state.gpu=state.cpu*3;return true;},"step");});
 for(auto& writer:writers)writer.join();
 stop=true;reader.join();need(!bad&&bus.snapshot()->value->cpu==4001);
 need(bus.stats().history==8&&bus.stats().bookmarks==1&&bus.bookmark("origin")==first);
 auto restored=bus.rewind(first->serial);need(restored->value==first->value&&restored->serial>4000&&restored->origin==first->serial);
 need(before->value->cpu==1);bus.eraseBookmark("origin");invalid([&]{bus.at(first->serial);});
 qssd::StateArchive archive;
 archive.add("cpu.native",std::string("RVPC2\0state",11));archive.add("gpu.native",archive.blob("cpu.native"));
 archive.add("resource.a",first->value->resource);archive.add("resource.b",*first->value->resource);
 auto encoded=archive.encode();auto recovered=qssd::StateArchive::decode(encoded);
 need(encoded.size()<1024*1024+1024&&recovered.blob("resource.a")==recovered.blob("resource.b"));
 need(recovered.get("cpu.native")==archive.get("cpu.native")&&recovered.blob("cpu.native")==recovered.blob("gpu.native"));
 need(recovered.encode()==encoded);invalid([&]{archive.add("cpu.native","duplicate");});
 for(std::size_t index:{std::size_t(0),encoded.size()/2,encoded.size()-1}){auto badBytes=encoded;badBytes[index]^=1;invalid([&]{qssd::StateArchive::decode(badBytes);});}
 for(std::size_t size:{std::size_t(0),std::size_t(27),encoded.size()-1})invalid([&]{qssd::StateArchive::decode(encoded.substr(0,size));});
 qssd::StateBus<Graph> growing(Graph{},0);need(growing.stats().historyCapacity==0);
 std::cout<<"{\"state_bus\":\"PASS\",\"checks\":"<<checks<<",\"concurrent_writes\":4000,\"full_graph_consistent\":true,\"archive_deduplicated\":true}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
