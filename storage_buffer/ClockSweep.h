#pragma once

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

template <typename Key, typename Value>
class ClockSweep {
public:
  explicit ClockSweep(size_t maxNumber = 8u)
      : maxCacheSize(maxNumber), clockHand(0), stopThread(false) {
    frames.resize(maxCacheSize);
  }

  ~ClockSweep() {
    stopThread = true;
    if (bgClockThread.joinable()) {
      bgClockThread.join();
    }
  }

  bool getKey(const Key &key, Value &value) {
    std::lock_guard<std::mutex> guard(mutex);
    auto it = lookup.find(key);
    if (it == lookup.end()) {
      return false;
    }

    Frame &frame = frames[it->second];
    frame.used = true;
    value = frame.value;
    return true;
  }

  void putKey(const Key &key, const Value &value) {
    std::lock_guard<std::mutex> guard(mutex);
    auto it = lookup.find(key);

    if (it != lookup.end()) {
      Frame &frame = frames[it->second];
      frame.value = value;
      frame.used = true;
      return;
    }

    size_t victim = findVictim();
    if (frames[victim].occupied) {
      lookup.erase(frames[victim].key);
    }

    frames[victim].key = key;
    frames[victim].value = value;
    frames[victim].occupied = true;
    frames[victim].used = true;
    lookup[key] = victim;
  }

  void printState() const {
    std::lock_guard<std::mutex> guard(mutex);
    std::cout << "ClockSweep cache state: ";
    for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i].occupied) {
        std::cout << "{" << frames[i].key << ":" << frames[i].value
                  << (frames[i].used ? "*" : "") << "} ";
      } else {
        std::cout << "{empty} ";
      }
    }
    std::cout << "hand=" << clockHand << '\n';
  }

private:
  struct Frame {
    Key key{};
    Value value{};
    bool used{false};
    bool occupied{false};
  };

  std::vector<Frame> frames;
  size_t maxCacheSize;
  size_t clockHand;
  std::unordered_map<Key, size_t> lookup;
  mutable std::mutex mutex;
  std::thread bgClockThread;
  std::atomic<bool> stopThread;

  size_t findVictim() {
    while (true) {
      if (!frames[clockHand].occupied || !frames[clockHand].used) {
        size_t victim = clockHand;
        clockHand = (clockHand + 1) % maxCacheSize;
        return victim;
      }
      frames[clockHand].used = false;
      clockHand = (clockHand + 1) % maxCacheSize;
    }
  }
};
