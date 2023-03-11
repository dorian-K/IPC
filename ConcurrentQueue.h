#pragma once

#include <mutex>
#include <queue>

template <typename T>
class ConcurrentQueue {
   public:
    bool push(T const& value) {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->q.push(value);
        return true;
    }
    bool pop(T& v) {
        std::unique_lock<std::mutex> lock(this->mutex);
        if (this->q.empty()) return false;
        v = this->q.front();
        this->q.pop();
        return true;
    }
    bool empty() {
        std::unique_lock<std::mutex> lock(this->mutex);
        return this->q.empty();
    }
    size_t size() {
        std::unique_lock<std::mutex> lock(this->mutex);
        return this->q.size();
    }
    void clear() {
        std::unique_lock<std::mutex> lock(this->mutex);
        std::queue<T> temp;
        this->q.swap(temp);
    }

   private:
    std::queue<T> q;
    std::mutex mutex;
};
