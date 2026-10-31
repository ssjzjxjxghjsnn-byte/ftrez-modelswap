#pragma once
#include <list>
#include <memory>
#include <cstddef>

namespace ft::self_model {


template<class Asset> class ModelCache {
    struct Entry { int key; size_t bytes; std::shared_ptr<const Asset> asset; };
    std::list<Entry> entries_;
    size_t bytes_=0,maxBytes_,maxEntries_;
public:
    ModelCache(size_t bytes,size_t entries):maxBytes_(bytes),maxEntries_(entries) {}
    std::shared_ptr<const Asset> get(int key) {
        for(auto it=entries_.begin();it!=entries_.end();++it) if(it->key==key) {
            auto result=it->asset; entries_.splice(entries_.begin(),entries_,it); return result;
        }
        return {};
    }
    void erase(int key) {
        for(auto it=entries_.begin();it!=entries_.end();++it) if(it->key==key) {
            bytes_-=it->bytes; entries_.erase(it); return;
        }
    }
    void put(int key,std::shared_ptr<const Asset> asset,size_t bytes) {
        if(!asset || !maxEntries_ || bytes>maxBytes_) return;
        for(auto it=entries_.begin();it!=entries_.end();++it) if(it->key==key) {
            bytes_-=it->bytes; entries_.erase(it); break;
        }
        while(!entries_.empty() && (entries_.size()>=maxEntries_ || bytes_>maxBytes_-bytes)) {
            bytes_-=entries_.back().bytes; entries_.pop_back();
        }
        entries_.push_front({key,bytes,std::move(asset)}); bytes_+=bytes;
    }
};
}
