#pragma once

#include <algorithm>
#include <iostream>
#include <vector>

template <typename Key, typename Row>
class BTree {
public:
  struct Entry {
    Key key;
    Row row;
  };

  explicit BTree(size_t degree)
      : minDegree(degree), root(new Node(true)) {}

  ~BTree() { delete root; }

  bool search(const Key &key, Row &outRow) const {
    return searchRecursive(root, key, outRow);
  }

  void insert(const Key &key, const Row &row) {
    if (root->keys.size() == maxKeys()) {
      Node *newRoot = new Node(false);
      newRoot->children.push_back(root);
      splitChild(newRoot, 0);
      root = newRoot;
    }
    insertNonFull(root, {key, row});
  }

  void print() const { printNode(root, 0); }

private:
  struct Node {
    bool leaf;
    std::vector<Entry> keys;
    std::vector<Node *> children;

    explicit Node(bool leafFlag) : leaf(leafFlag) {}

    ~Node() {
      for (Node *child : children) {
        delete child;
      }
    }
  };

  Node *root;
  size_t minDegree;

  size_t maxKeys() const { return 2 * minDegree - 1; }
  size_t minKeys() const { return minDegree - 1; }

  bool searchRecursive(Node *node, const Key &key, Row &outRow) const {
    size_t idx = 0;
    while (idx < node->keys.size() && key > node->keys[idx].key) {
      ++idx;
    }

    if (idx < node->keys.size() && node->keys[idx].key == key) {
      outRow = node->keys[idx].row;
      return true;
    }

    if (node->leaf) {
      return false;
    }

    return searchRecursive(node->children[idx], key, outRow);
  }

  void splitChild(Node *parent, size_t index) {
    Node *child = parent->children[index];
    Node *newChild = new Node(child->leaf);

    Entry median = child->keys[minDegree - 1];

    for (size_t j = 0; j < minDegree - 1; ++j) {
      newChild->keys.push_back(child->keys[j + minDegree]);
    }

    if (!child->leaf) {
      for (size_t j = 0; j < minDegree; ++j) {
        newChild->children.push_back(child->children[j + minDegree]);
      }
    }

    child->keys.resize(minDegree - 1);
    if (!child->leaf) {
      child->children.resize(minDegree);
    }

    parent->children.insert(parent->children.begin() + index + 1, newChild);
    parent->keys.insert(parent->keys.begin() + index, median);
  }

  void insertNonFull(Node *node, const Entry &entry) {
    size_t idx = node->keys.size();

    if (node->leaf) {
      node->keys.push_back(entry);
      while (idx > 0 && node->keys[idx].key < node->keys[idx - 1].key) {
        std::swap(node->keys[idx], node->keys[idx - 1]);
        --idx;
      }
      return;
    }

    while (idx > 0 && entry.key < node->keys[idx - 1].key) {
      --idx;
    }

    if (node->children[idx]->keys.size() == maxKeys()) {
      splitChild(node, idx);
      if (entry.key > node->keys[idx].key) {
        ++idx;
      }
    }
    insertNonFull(node->children[idx], entry);
  }

  void printNode(const Node *node, int depth) const {
    if (node == nullptr) {
      return;
    }

    std::cout << std::string(depth * 2, ' ');
    for (const Entry &entry : node->keys) {
      std::cout << '[' << entry.key << ':' << entry.row << "] ";
    }
    std::cout << '\n';

    if (!node->leaf) {
      for (Node *child : node->children) {
        printNode(child, depth + 1);
      }
    }
  }
};
