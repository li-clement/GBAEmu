#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace Core {

template <typename T> class RingBuffer {
public:
  explicit RingBuffer(size_t capacity)
      : capacity_(capacity), buffer_(capacity) {}

  // 尝试批量写入，返回实际写入的数据个数
  size_t push(const T *data, size_t count) {
    size_t writePos = writeIndex_.load(std::memory_order_relaxed);
    size_t readPos = readIndex_.load(std::memory_order_acquire);

    size_t available = capacity_ - (writePos - readPos);
    if (available == 0)
      return 0;

    size_t toWrite = std::min(count, available);
    size_t writePosMasked = writePos % capacity_;

    size_t firstPart = std::min(toWrite, capacity_ - writePosMasked);
    std::memcpy(&buffer_[writePosMasked], data, firstPart * sizeof(T));

    if (firstPart < toWrite) {
      std::memcpy(&buffer_[0], data + firstPart,
                  (toWrite - firstPart) * sizeof(T));
    }

    writeIndex_.store(writePos + toWrite, std::memory_order_release);
    return toWrite;
  }

  // 尝试批量读取，返回实际读取的数据个数
  size_t pop(T *outData, size_t count) {
    size_t writePos = writeIndex_.load(std::memory_order_acquire);
    size_t readPos = readIndex_.load(std::memory_order_relaxed);

    size_t available = writePos - readPos;
    if (available == 0)
      return 0;

    size_t toRead = std::min(count, available);
    size_t readPosMasked = readPos % capacity_;

    size_t firstPart = std::min(toRead, capacity_ - readPosMasked);
    std::memcpy(outData, &buffer_[readPosMasked], firstPart * sizeof(T));

    if (firstPart < toRead) {
      std::memcpy(outData + firstPart, &buffer_[0],
                  (toRead - firstPart) * sizeof(T));
    }

    readIndex_.store(readPos + toRead, std::memory_order_release);
    return toRead;
  }

  size_t size() const {
    return writeIndex_.load(std::memory_order_acquire) -
           readIndex_.load(std::memory_order_acquire);
  }

  void clear() {
    writeIndex_.store(0, std::memory_order_release);
    readIndex_.store(0, std::memory_order_release);
  }

  size_t capacity() const { return capacity_; }

private:
  size_t capacity_;
  std::vector<T> buffer_;
  // alignas 避免伪共享 (False sharing)
  alignas(64) std::atomic<size_t> writeIndex_{0};
  alignas(64) std::atomic<size_t> readIndex_{0};
};

} // namespace Core
