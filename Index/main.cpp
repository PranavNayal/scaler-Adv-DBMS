#include "BTree.h"
#include <iostream>

int main() {
  BTree<int, int> tree(3);
  int values[] = {10, 20, 5, 6, 12, 30, 7, 17};

  std::cout << "Inserting keys into B-tree index...\n";
  for (int value : values) {
    tree.insert(value, value * 10);
  }

  tree.print();

  int searchKeys[] = {6, 15, 30};
  for (int key : searchKeys) {
    int row = 0;
    if (tree.search(key, row)) {
      std::cout << "Found key " << key << " with row " << row << ".\n";
    } else {
      std::cout << "Key " << key << " not found.\n";
    }
  }

  return 0;
}
