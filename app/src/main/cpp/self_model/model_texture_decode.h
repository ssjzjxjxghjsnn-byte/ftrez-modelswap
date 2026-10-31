#pragma once
#include "self_model/model_format.h"
#include <memory>
#include <mutex>

namespace ft::self_model {
struct ModelPixel { uint8_t r,g,b,a; };
static_assert(sizeof(ModelPixel)==4);
struct DecodedModelTexture {
    int width=0,height=0;
    double decodeMs=0;

    std::shared_ptr<ModelPixel> pixels;
    size_t count() const { return size_t(width)*size_t(height); }
};

DecodedModelTexture decodeModelTexture(const std::vector<uint8_t>& encoded);
class ModelTextureRequest {
    friend std::shared_ptr<ModelTextureRequest> requestModelTexture(std::shared_ptr<const Model>,size_t);
    friend std::shared_ptr<ModelTextureRequest> requestEncodedTexture(std::vector<uint8_t>);
    friend class ModelTextureWorker;
    std::shared_ptr<const Model> model_;
    size_t source_=0;
    std::vector<uint8_t> encoded_;
    std::mutex mutex_;
    bool ready_=false;
    DecodedModelTexture result_;
public:

    bool take(DecodedModelTexture& result) {
        std::lock_guard lock(mutex_);
        if(!ready_) return false;
        result=std::move(result_); return true;
    }
};


std::shared_ptr<ModelTextureRequest> requestModelTexture(std::shared_ptr<const Model> model,size_t source);

std::shared_ptr<ModelTextureRequest> requestEncodedTexture(std::vector<uint8_t> encoded);
}
