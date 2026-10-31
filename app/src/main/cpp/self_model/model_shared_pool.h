#pragma once
#include <list>
#include <memory>
#include <cstddef>

namespace ft::self_model {
// Unity resources are acquired/released only from the existing main-thread hook.
// Asset identity includes directory reloads; a numeric menu index is not enough.
template<class Asset,class Resources> class ModelSharedPool {
    struct Entry { std::shared_ptr<const Asset> asset; std::shared_ptr<Resources> resources; };
    std::list<Entry> entries_;
    size_t retained_;
public:
    explicit ModelSharedPool(size_t retained):retained_(retained) {}
    template<class Factory> std::shared_ptr<Resources> acquire(std::shared_ptr<const Asset> asset,Factory create) {
        for(auto it=entries_.begin();it!=entries_.end();++it) if(it->asset==asset) {
            auto result=it->resources; entries_.splice(entries_.begin(),entries_,it); return result;
        }
        auto result=create();
        entries_.push_front({std::move(asset),result}); trim(); return result;
    }
    void trim() {
        for(auto it=entries_.end();entries_.size()>retained_ && it!=entries_.begin();) {
            --it;
            if(it->resources.use_count()==1) it=entries_.erase(it);
        }
    }
    size_t size() const { return entries_.size(); }
};
}
