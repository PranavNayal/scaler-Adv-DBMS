#include "ClockSweep.h"
#include <iostream>
#include <string>

int main() {
  ClockSweep<int, std::string> cache(3);
  cache.putKey(1, "A");
  cache.putKey(2, "B");
  cache.putKey(3, "C");
  cache.printState();

  std::string value;
  if (cache.getKey(2, value)) {
    std::cout << "Accessed key 2 -> " << value << "\n";
  }

  cache.putKey(4, "D");
  cache.printState();

  if (!cache.getKey(1, value)) {
    std::cout << "Key 1 was evicted by Clock Sweep replacement.\n";
  }

  cache.putKey(5, "E");
  cache.printState();

  if (cache.getKey(3, value)) {
    std::cout << "Key 3 is still in the cache -> " << value << "\n";
  }

  return 0;
}
