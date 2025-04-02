#include "parlay/sequence.h"
#include <flock/flock.h>
#include <parlay/primitives.h>
#include <parlay/random.h>

const int LRANGE = 0, RRANGE = 1e5 + 10, SEED = 42;

template <typename K, typename V> struct Set {
  static inline parlay::random_generator generator =
      parlay::random_generator(SEED);

  struct update_execution {
    parlay::sequence<update_execution *> pioneers;

    update_execution() { pioneers = parlay::sequence<update_execution *>(); }
  };

  struct version_node {
    version_node *left;
    version_node *right;
    update_execution *created_by;
    V value;
    long size;

    version_node(update_execution *created_by, version_node *left,
                 version_node *right)
        : left(left), right(right), size(0), created_by(created_by) {};

    version_node(update_execution *created_by, int size, version_node *left,
                 version_node *right)
        : left(left), right(right), size(size), created_by(created_by) {};

    version_node(update_execution *created_by)
        : size(0), created_by(created_by) {};

    version_node(update_execution *created_by, V v)
        : value(v), size(1), created_by(created_by) {}
  };

  struct node {
    bool is_leaf;
    flck::atomic<version_node *> version;
    int l_range, r_range;
    node *left_child, *right_child, *parent;

    node(node *parent, int l_range, int r_range)
        : parent(parent), is_leaf((r_range - l_range) == 1), l_range(l_range),
          r_range(r_range), version(nullptr) {};

    bool in_range(K k) { return l_range <= k && k < r_range; }
  };

  flck::memory_pool<node> nodes_pool;
  flck::memory_pool<version_node> versions_pool;
  flck::memory_pool<update_execution> updates_pool;

  node *empty(node *parent, int l_range, int r_range) {
    return nodes_pool.new_obj(parent, l_range, r_range);
  }

  node *empty(size_t n) {
    node *root = empty(nullptr, LRANGE, RRANGE);
    initialize(root);
    return root;
  }

  void initialize(node *n) {
    if (n->is_leaf) {
      n->version = versions_pool.new_obj();
      return;
    }

    int mid = (n->l_range + n->r_range) / 2;
    n->left_child = empty(n, n->l_range, mid);
    initialize(n->left_child);
    n->right_child = empty(n, mid, n->r_range);
    initialize(n->right_child);

    version_node *v = versions_pool.new_obj(n->left_child->version.read(),
                                            n->right_child->version.read());
    n->version = v;
  }

  node *find_location(node *root, K key) {
    if (!root->in_range(key)) {
      return nullptr;
    }

    node *n = root;
    while (!n->is_leaf) {
      if (n->left_child->in_range(key)) {
        n = n->left_child;
      } else {
        n = n->right_child;
      }
    }

    return n;
  }

  bool insert(node *root, K key, V val) {
    return flck::with_epoch([=] {
      update_execution *update = updates_pool.new_obj();
      node *n = find_location(root, key);

      version_node *old_version_node = n->version.read();
      if (old_version_node->size != 0) {
        return false;
      }

      version_node *new_version_node = versions_pool.new_obj(val);

      if (!n->version.cas(old_version_node, new_version_node)) {
        versions_pool.retire(new_version_node);
        return false;
      }

      versions_pool.retire(old_version_node);
      propagate(n->parent, update);

      return true;
    });
  }

  bool remove(node *root, K key) {
    return flck::with_epoch([=] {
      update_execution *update = updates_pool.new_obj();
      node *n = find_location(root, key);
      if (n == nullptr) {
        return false;
      }

      version_node *old_version = n->version.read();
      if (old_version->size == 0) {
        return false;
      }

      version_node *new_version = versions_pool.new_obj();
      if (!n->version.cas(old_version, new_version)) {
        versions_pool.retire(new_version);
        return false;
      }

      versions_pool.retire(old_version);
      propagate(n->parent, update);

      return true;
    });
  }

  void propagate(node *n, update_execution *created_by) {
    node *curr_n = (!n->is_leaf) ? n : n->parent;
    while (curr_n != nullptr) {
      if (!refresh(curr_n) && !refresh(curr_n, created_by)) {
        return; // fail fast if 2 refreshes fail
      }

      curr_n = curr_n->parent;
    }
  }

  bool refresh(node *n, update_execution *created_by) {
    version_node *old_version_node = n->version.read();

    version_node *left_child = n->left_child->version.read(),
                 *right_child = n->right_child->version.read();
    int size = left_child->size + right_child->size;

    version_node *new_version_node =
        versions_pool.new_obj(created_by, size, left_child, right_child);

    if (!n->version.cas(old_version_node, new_version_node)) {
      versions_pool.retire(new_version_node);
      return false;
    }

    versions_pool.retire(old_version_node);
    return true;
  }

  std::optional<V> find_(node *root, K k) {
    node *n = find_location(root, k);
    if (n == nullptr) {
      return std::nullopt;
    }

    version_node *version = n->version.read();

    return (version->size == 1) ? std::make_optional(version->value)
                                : std::nullopt;
  }

  std::optional<V> find(node *root, K k) {
    return flck::with_epoch([&] { return find_(root, k); });
  }

  void retire(node *p) {
    if (p == nullptr)
      return;
    else {
      parlay::par_do([&]() { retire(p->left_child); },
                     [&]() { retire(p->right_child); });
      nodes_pool.retire(p);
    }
  }

  void stats() {
    nodes_pool.stats();
    versions_pool.stats();
    updates_pool.stats();
  }

  void clear() {
    nodes_pool.clear();
    versions_pool.clear();
    updates_pool.clear();
  }

  void reserve(size_t n) {
    nodes_pool.reserve(n);
    versions_pool.reserve(n);
    updates_pool.reserve(n);
  }

  void shuffle(size_t n) {
    nodes_pool.shuffle(n);
    versions_pool.shuffle(n);
    updates_pool.reserve(n);
  }

  long check(node *root) { return root->version.load()->size; }
};
