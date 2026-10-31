#include "self_model/model_texture_decode.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <thread>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

namespace ft::self_model {
DecodedModelTexture decodeModelTexture(const std::vector<uint8_t>& encoded) {
    DecodedModelTexture out;
    if(encoded.empty() || encoded.size()>maxModelBytes || encoded.size()>size_t(std::numeric_limits<int>::max())) return out;
    const auto* data=encoded.data(); const int size=int(encoded.size());
    int width=0,height=0,channels=0;
    if(!stbi_info_from_memory(data,size,&width,&height,&channels) ||
       width<1 || height<1 || width>4096 || height>4096 || stbi_is_16_bit_from_memory(data,size)) return out;
    auto* pixels=stbi_load_from_memory(data,size,&width,&height,&channels,4);
    if(!pixels) return out;
    out.pixels=std::shared_ptr<ModelPixel>(reinterpret_cast<ModelPixel*>(pixels),[](ModelPixel* p){stbi_image_free(p);});
    out.width=width; out.height=height;


    for(int y=0;y<height/2;++y) {
        auto* top=out.pixels.get()+size_t(y)*width;
        auto* bottom=out.pixels.get()+size_t(height-1-y)*width;
        std::swap_ranges(top,top+width,bottom);
    }
    return out;
}

class ModelTextureWorker {
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::weak_ptr<ModelTextureRequest>> queue_;
    void run() {
        for(;;) {
            std::shared_ptr<ModelTextureRequest> request;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock,[&]{return !queue_.empty();});
                request=queue_.front().lock(); queue_.pop_front();
            }
            if(!request) continue;
            DecodedModelTexture result;
            const auto start=std::chrono::steady_clock::now();
            try { result=decodeModelTexture(request->model_?modelTextureBytes(*request->model_,request->source_):request->encoded_); }
            catch(...) {                                                         }
            result.decodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            {
                std::lock_guard lock(request->mutex_);
                request->result_=std::move(result); request->ready_=true;
                request->model_.reset();
                std::vector<uint8_t>().swap(request->encoded_);
            }
        }
    }
public:
    ModelTextureWorker() { std::thread([this]{run();}).detach(); }
    void submit(const std::shared_ptr<ModelTextureRequest>& request) {
        std::lock_guard lock(mutex_);
        queue_.erase(std::remove_if(queue_.begin(),queue_.end(),[](const auto& job){return job.expired();}),queue_.end());
        queue_.push_back(request); wake_.notify_one();
    }
};
static ModelTextureWorker& textureWorker() {

    static auto* worker=new ModelTextureWorker;return *worker;
}
std::shared_ptr<ModelTextureRequest> requestModelTexture(std::shared_ptr<const Model> model,size_t source) {
    if(!model || source>=modelTextureCount(*model)) return {};

    auto request=std::make_shared<ModelTextureRequest>();
    request->model_=std::move(model); request->source_=source;
    textureWorker().submit(request); return request;
}
std::shared_ptr<ModelTextureRequest> requestEncodedTexture(std::vector<uint8_t> encoded) {
    if(encoded.empty() || encoded.size()>maxModelBytes)return {};
    auto request=std::make_shared<ModelTextureRequest>();request->encoded_=std::move(encoded);
    textureWorker().submit(request);return request;
}
}
