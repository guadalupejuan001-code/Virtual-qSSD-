#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace qssd {
class StateConflict : public std::runtime_error {
 public: StateConflict():std::runtime_error("qSSD state changed before commit"){}
};

// C03: the committed value is the COMPLETE logical component graph. Nested
// immutable handles share large resources; publishing does not serialize it.
// Copies connect clients to one authority. fork() creates an independent one.
template<class State>
class StateBus {
 public:
  struct Version {
    std::uint64_t authority,serial,parent,origin;
    std::string reason;
    std::shared_ptr<const State> value;
  };
  using View=std::shared_ptr<const Version>;
  using Validator=std::function<void(const State&)>;
  struct Stats {std::uint64_t authority,serial;std::size_t history,bookmarks,historyCapacity;};
 private:
  struct Control {
    std::mutex mutex;
    View head;
    std::vector<View> history;
    std::size_t cursor=0;
    std::map<std::string,View> bookmarks;
    Validator validate;
    std::uint64_t authority;
    Control(std::shared_ptr<const State> state,std::size_t capacity,Validator check,std::uint64_t origin)
      :history(capacity),validate(std::move(check)),authority(nextAuthority()) {
      if(validate)validate(*state);
      head=std::make_shared<const Version>(Version{authority,1,0,origin,"open",std::move(state)});
      history[0]=head;cursor=1%capacity;
    }
    static std::uint64_t nextAuthority(){
      static std::atomic<std::uint64_t> next{1};
      auto n=next.fetch_add(1);
      if(!n||n>=INT64_MAX)throw std::overflow_error("qSSD authority counter exhausted");
      return n;
    }
  };
  std::shared_ptr<Control> control_;
  static void name(const std::string& text){
    if(text.empty()||text.size()>64)throw std::invalid_argument("qSSD bookmark name");
    for(unsigned char c:text)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))
      throw std::invalid_argument("qSSD bookmark character");
  }
  View commitLocked(std::shared_ptr<const State> value,std::string reason,std::uint64_t origin=0){
    if(reason.empty()||reason.size()>128)throw std::invalid_argument("qSSD transition reason");
    if(control_->head->serial>=INT64_MAX)throw std::overflow_error("qSSD state counter exhausted");
    if(control_->validate)control_->validate(*value);
    auto next=std::make_shared<const Version>(Version{control_->authority,control_->head->serial+1,
      control_->head->serial,origin,std::move(reason),std::move(value)});
    // All potentially throwing work is complete. The bounded ring never grows.
    control_->history[control_->cursor]=next;
    control_->cursor=(control_->cursor+1)%control_->history.size();
    std::atomic_store(&control_->head,next);return next;
  }
  View findLocked(std::uint64_t serial)const{
    for(const auto& view:control_->history)if(view&&view->serial==serial)return view;
    for(const auto& item:control_->bookmarks)if(item.second->serial==serial)return item.second;
    throw std::out_of_range("qSSD version is no longer retained");
  }
  StateBus(std::shared_ptr<const State> value,std::size_t capacity,Validator check,std::uint64_t origin)
    :control_(std::make_shared<Control>(std::move(value),capacity,std::move(check),origin)){}
 public:
  explicit StateBus(State initial,std::size_t history=32,Validator check={}){
    if(history<1||history>4096)throw std::invalid_argument("qSSD history capacity");
    control_=std::make_shared<Control>(std::make_shared<const State>(std::move(initial)),history,std::move(check),0);
  }
  View snapshot()const{return std::atomic_load(&control_->head);}
  View publish(State next,std::string reason,const View& expected={}){
    auto value=std::make_shared<const State>(std::move(next));
    std::lock_guard<std::mutex> lock(control_->mutex);
    if(expected&&expected!=control_->head)throw StateConflict();
    return commitLocked(std::move(value),std::move(reason));
  }
  template<class Operation> View update(Operation operation,std::string reason){
    std::lock_guard<std::mutex> lock(control_->mutex);
    auto next=std::make_shared<State>(*control_->head->value);
    if(!operation(*next))return control_->head;
    return commitLocked(std::move(next),std::move(reason));
  }
  View at(std::uint64_t serial)const{std::lock_guard<std::mutex> lock(control_->mutex);return findLocked(serial);}
  void bookmark(const std::string& label,const View& view){
    name(label);if(!view||view->authority!=control_->authority)throw std::invalid_argument("qSSD foreign bookmark");
    std::lock_guard<std::mutex> lock(control_->mutex);
    if(!control_->bookmarks.count(label)&&control_->bookmarks.size()>=16)throw std::length_error("qSSD bookmark budget");
    control_->bookmarks[label]=view;
  }
  View bookmark(const std::string& label)const{
    name(label);std::lock_guard<std::mutex> lock(control_->mutex);return control_->bookmarks.at(label);
  }
  void eraseBookmark(const std::string& label){name(label);std::lock_guard<std::mutex> lock(control_->mutex);control_->bookmarks.erase(label);}
  View rewind(std::uint64_t serial){
    std::lock_guard<std::mutex> lock(control_->mutex);auto target=findLocked(serial);
    return commitLocked(target->value,"rewind",target->serial);
  }
  StateBus fork(const View& view={})const{
    const auto retained=view?view:snapshot();
    if(retained->authority!=control_->authority)throw std::invalid_argument("qSSD foreign fork");
    return StateBus(retained->value,control_->history.size(),control_->validate,retained->serial);
  }
  Stats stats()const{
    std::lock_guard<std::mutex> lock(control_->mutex);std::size_t n=0;for(const auto& view:control_->history)n+=bool(view);
    return {control_->authority,control_->head->serial,n,control_->bookmarks.size(),control_->history.size()};
  }
};
} // namespace qssd
