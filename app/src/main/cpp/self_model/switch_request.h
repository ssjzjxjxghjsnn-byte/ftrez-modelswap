#pragma once
#include <cstdint>
#include <utility>

namespace ft::self_model {


template<class Options> class SwitchRequest {
    Options requested_{},applied_{};
    uint64_t version_=0,appliedVersion_=0;
public:
    bool submit(Options value) {
        if(value==requested_) return false;
        requested_=std::move(value); ++version_; return true;
    }
    bool consume() {
        if(version_==appliedVersion_) return false;
        applied_=requested_; appliedVersion_=version_; return true;
    }
    const Options& requested() const { return requested_; }
    const Options& applied() const { return applied_; }
};
}
