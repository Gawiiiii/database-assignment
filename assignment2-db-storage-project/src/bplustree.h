#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

// A compact in-memory B+ tree. Deletion removes leaf entries but intentionally
// does not rebalance nodes; this keeps the prototype simple while preserving
// correct point-query behavior for benchmark workloads.
template <typename Key, typename Value, typename Compare = std::less<Key>>
class BPlusTree {
public:
    explicit BPlusTree(std::size_t order = 64) : order_(order) {
        if (order_ < 4) {
            throw std::runtime_error("B+ tree order must be at least 4");
        }
        root_ = std::make_unique<Node>(true);
    }

    void insert(const Key& key, const Value& value) {
        if (root_->keys.size() == max_keys()) {
            auto old_root = std::move(root_);
            root_ = std::make_unique<Node>(false);
            root_->children.push_back(std::move(old_root));
            split_child(root_.get(), 0);
        }
        insert_non_full(root_.get(), key, value);
    }

    std::optional<Value> search(const Key& key) const {
        const Node* node = root_.get();
        while (node && !node->leaf) {
            const auto idx = upper_bound_index(node->keys, key);
            node = node->children[idx].get();
        }
        if (!node) {
            return std::nullopt;
        }
        const auto it = lower_bound(node->keys, key);
        if (it != node->keys.end() && equal(*it, key)) {
            return node->values[static_cast<std::size_t>(it - node->keys.begin())];
        }
        return std::nullopt;
    }

    bool remove(const Key& key) {
        Node* leaf = find_leaf(root_.get(), key);
        if (!leaf) {
            return false;
        }
        const auto it = lower_bound(leaf->keys, key);
        if (it == leaf->keys.end() || !equal(*it, key)) {
            return false;
        }
        const auto idx = static_cast<std::size_t>(it - leaf->keys.begin());
        leaf->keys.erase(leaf->keys.begin() + static_cast<long>(idx));
        leaf->values.erase(leaf->values.begin() + static_cast<long>(idx));
        return true;
    }

    void update(const Key& key, const Value& value) {
        Node* leaf = find_leaf(root_.get(), key);
        if (!leaf) {
            insert(key, value);
            return;
        }
        const auto it = lower_bound(leaf->keys, key);
        if (it == leaf->keys.end() || !equal(*it, key)) {
            insert(key, value);
            return;
        }
        leaf->values[static_cast<std::size_t>(it - leaf->keys.begin())] = value;
    }

private:
    struct Node {
        explicit Node(bool is_leaf) : leaf(is_leaf) {}
        bool leaf;
        std::vector<Key> keys;
        std::vector<Value> values;
        std::vector<std::unique_ptr<Node>> children;
        Node* next = nullptr;
    };

    std::size_t max_keys() const { return order_ - 1; }

    typename std::vector<Key>::const_iterator lower_bound(const std::vector<Key>& keys, const Key& key) const {
        return std::lower_bound(keys.begin(), keys.end(), key, comp_);
    }

    std::size_t upper_bound_index(const std::vector<Key>& keys, const Key& key) const {
        return static_cast<std::size_t>(std::upper_bound(keys.begin(), keys.end(), key, comp_) - keys.begin());
    }

    bool equal(const Key& a, const Key& b) const {
        return !comp_(a, b) && !comp_(b, a);
    }

    Node* find_leaf(Node* node, const Key& key) const {
        while (node && !node->leaf) {
            node = node->children[upper_bound_index(node->keys, key)].get();
        }
        return node;
    }

    void split_child(Node* parent, std::size_t child_index) {
        Node* child = parent->children[child_index].get();
        const std::size_t mid = child->keys.size() / 2;
        auto sibling = std::make_unique<Node>(child->leaf);

        if (child->leaf) {
            sibling->keys.assign(child->keys.begin() + static_cast<long>(mid), child->keys.end());
            sibling->values.assign(child->values.begin() + static_cast<long>(mid), child->values.end());
            child->keys.erase(child->keys.begin() + static_cast<long>(mid), child->keys.end());
            child->values.erase(child->values.begin() + static_cast<long>(mid), child->values.end());
            sibling->next = child->next;
            child->next = sibling.get();
            parent->keys.insert(parent->keys.begin() + static_cast<long>(child_index), sibling->keys.front());
        } else {
            const Key promoted = child->keys[mid];
            sibling->keys.assign(child->keys.begin() + static_cast<long>(mid + 1), child->keys.end());
            child->keys.erase(child->keys.begin() + static_cast<long>(mid), child->keys.end());

            sibling->children.reserve(child->children.size() - mid - 1);
            auto first_move = child->children.begin() + static_cast<long>(mid + 1);
            while (first_move != child->children.end()) {
                sibling->children.push_back(std::move(*first_move));
                first_move = child->children.erase(first_move);
            }
            parent->keys.insert(parent->keys.begin() + static_cast<long>(child_index), promoted);
        }
        parent->children.insert(parent->children.begin() + static_cast<long>(child_index + 1), std::move(sibling));
    }

    void insert_non_full(Node* node, const Key& key, const Value& value) {
        if (node->leaf) {
            auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key, comp_);
            const auto idx = static_cast<std::size_t>(it - node->keys.begin());
            if (it != node->keys.end() && equal(*it, key)) {
                node->values[idx] = value;
                return;
            }
            node->keys.insert(node->keys.begin() + static_cast<long>(idx), key);
            node->values.insert(node->values.begin() + static_cast<long>(idx), value);
            return;
        }

        std::size_t idx = upper_bound_index(node->keys, key);
        if (node->children[idx]->keys.size() == max_keys()) {
            split_child(node, idx);
            if (comp_(node->keys[idx], key)) {
                ++idx;
            }
        }
        insert_non_full(node->children[idx].get(), key, value);
    }

    std::size_t order_;
    Compare comp_;
    std::unique_ptr<Node> root_;
};
