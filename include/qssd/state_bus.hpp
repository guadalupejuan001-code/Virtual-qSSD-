#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace qssd {
class StateConflict : public std::runtime_error {
 public: StateConflict():std::runtime_error("qSSD state changed before commit"){}
};

// The committed value is the COMPLETE logical component graph. Nested
// immutable handles share large resources; publishing does not serialize it.
// Copies connect clients to one authority. fork() creates an independent one.
// History grows on demand by default. A nonzero retention is caller-selected.
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
    std::deque<View> history;
    std::size_t retention;
    std::map<std::string,View> bookmarks;
    Validator validate;
    std::uint64_t authority;
    Control(std::shared_ptr<const State> state,std::size_t capacity,Validator check,std::uint64_t origin)
      :retention(capacity),validate(std::move(check)),authority(nextAuthority()) {
      if(validate)validate(*state);
      head=std::make_shared<const Version>(Version{authority,1,0,origin,"open",std::move(state)});
      history.push_back(head);
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
    if(text.empty())throw std::invalid_argument("qSSD bookmark name");
    for(unsigned char c:text)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))
      throw std::invalid_argument("qSSD bookmark character");
  }
  View commitLocked(std::shared_ptr<const State> value,std::string reason,std::uint64_t origin=0){
    if(reason.empty())throw std::invalid_argument("qSSD transition reason");
    if(control_->head->serial>=INT64_MAX)throw std::overflow_error("qSSD state counter exhausted");
    if(control_->validate)control_->validate(*value);
    auto next=std::make_shared<const Version>(Version{control_->authority,control_->head->serial+1,
      control_->head->serial,origin,std::move(reason),std::move(value)});
    // Allocate before changing the authority. Allocation/validation failure
    // leaves the previous committed state and retained history intact.
    control_->history.push_back(next);
    if(control_->retention&&control_->history.size()>control_->retention)
      control_->history.pop_front();
    std::atomic_store(&control_->head,next);return next;
  }
  View findLocked(std::uint64_t serial)const{
    const auto found=std::lower_bound(control_->history.begin(),control_->history.end(),serial,
      [](const View& view,std::uint64_t target){return view->serial<target;});
    if(found!=control_->history.end()&&(*found)->serial==serial)return *found;
    for(const auto& item:control_->bookmarks)if(item.second->serial==serial)return item.second;
    throw std::out_of_range("qSSD version is no longer retained");
  }
  StateBus(std::shared_ptr<const State> value,std::size_t capacity,Validator check,std::uint64_t origin)
    :control_(std::make_shared<Control>(std::move(value),capacity,std::move(check),origin)){}
 public:
  explicit StateBus(State initial,std::size_t history=0,Validator check={}){
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
    return StateBus(retained->value,control_->retention,control_->validate,retained->serial);
  }
  Stats stats()const{
    std::lock_guard<std::mutex> lock(control_->mutex);
    return {control_->authority,control_->head->serial,control_->history.size(),control_->bookmarks.size(),control_->retention};
  }
};
} // namespace qssd
