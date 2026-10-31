#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ft::self_model {



class ModelCollisionWorker {
    std::mutex mutex_;
    std::condition_variable wake_,done_;
    std::function<void()> job_;
    std::exception_ptr error_;
    bool stopping_=false;
    std::thread thread_;
public:
    ModelCollisionWorker():thread_([this] {
        std::unique_lock lock(mutex_);
        for(;;) {
            wake_.wait(lock,[&] { return stopping_ || bool(job_); });
            if(stopping_) return;
            lock.unlock();
            std::exception_ptr error;
            try { job_(); } catch(...) { error=std::current_exception(); }
            lock.lock();
            error_=error; job_={}; done_.notify_one();
        }
    }) {}
    ModelCollisionWorker(const ModelCollisionWorker&)=delete;
    ModelCollisionWorker& operator=(const ModelCollisionWorker&)=delete;
    ~ModelCollisionWorker() {
        { std::lock_guard lock(mutex_); stopping_=true; }
        wake_.notify_one(); thread_.join();
    }
    template<class Background,class Foreground>
    void run(Background background,Foreground foreground) {
        { std::lock_guard lock(mutex_); error_={}; job_=std::move(background); }
        wake_.notify_one();
        std::exception_ptr foregroundError;
        try { foreground(); } catch(...) { foregroundError=std::current_exception(); }
        std::unique_lock lock(mutex_);
        done_.wait(lock,[&] { return !job_; });
        if(foregroundError) std::rethrow_exception(foregroundError);
        if(error_) std::rethrow_exception(error_);
    }
};



class ModelCollisionWorkers {
    std::vector<std::unique_ptr<ModelCollisionWorker>> workers_;
    template<class Job> void runNext(size_t index,Job& job) {
        if(index==workers_.size()) { job(); return; }
        workers_[index]->run([&] { job(); },[&] { runNext(index+1,job); });
    }
public:
    explicit ModelCollisionWorkers(size_t count=0) {
        for(size_t i=0;i<count;++i) {
            try { workers_.push_back(std::make_unique<ModelCollisionWorker>()); }
            catch(const std::system_error&) { break; }
        }
    }
    size_t size() const { return workers_.size(); }
    template<class Job> void run(Job job) { runNext(0,job); }
};
}
