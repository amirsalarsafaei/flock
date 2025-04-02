#include <flock/flock.h>
#include <parlay/primitives.h>

const int LRANGE = 0, RRANGE = 1e5 + 10;

template <typename K, typename V> struct Set {

  struct version_node {
    version_node *left;
    version_node *right;
    V value;
    long size;

    version_node(version_node *left, version_node *right)
        : left(left), right(right), size(0) {};

    version_node(int size, version_node *left, version_node *right)
        : left(left), right(right), size(size) {};

    version_node() : size(0) {};

    version_node(V v) : value(v), size(1) {}
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

  flck::memory_pool<node> node_pool;
  flck::memory_pool<version_node> version_pool;

  node *empty(node *parent, int l_range, int r_range) {
    return node_pool.new_obj(parent, l_range, r_range);
  }

  node *empty(size_t n) {
    node *root = empty(nullptr, LRANGE, RRANGE);
    initialize(root);
    return root;
  }

  void initialize(node *n) {
    if (n->is_leaf) {
      n->version = version_pool.new_obj();
      return;
    }

    int mid = (n->l_range + n->r_range) / 2;
    n->left_child = empty(n, n->l_range, mid);
    initialize(n->left_child);
    n->right_child = empty(n, mid, n->r_range);
    initialize(n->right_child);

    version_node *v = version_pool.new_obj(n->left_child->version.read(),
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
      node *n = find_location(root, key);

      version_node *old_version_node = n->version.read();
      if (old_version_node->size != 0) {
        return false;
      }

      version_node *new_version_node = version_pool.new_obj(val);

      if (!n->version.cas(old_version_node, new_version_node)) {
        version_pool.retire(new_version_node);
        return false;
      }

      version_pool.retire(old_version_node);
      propagate(n->parent);

      return true;
    });
  }

  bool remove(node *root, K key) {
    return flck::with_epoch([=] {
      node *n = find_location(root, key);
      if (n == nullptr) {
        return false;
      }

      version_node *old_version = n->version.read();
      if (old_version->size == 0) {
        return false;
      }

      version_node *new_version = version_pool.new_obj();
      if (!n->version.cas(old_version, new_version)) {
        version_pool.retire(new_version);
        return false;
      }

      version_pool.retire(old_version);
      propagate(n->parent);

      return true;
    });
  }

  void propagate(node *n) {
    node *curr_n = (!n->is_leaf) ? n : n->parent;
    while (curr_n != nullptr) {
      if (!refresh(curr_n) && !refresh(curr_n)) {
        return; // fail fast if 2 refreshes fail
      }

      curr_n = curr_n->parent;
    }
  }

  bool refresh(node *n) {
    version_node *old_version_node = n->version.read();

    version_node *left_child = n->left_child->version.read(),
                 *right_child = n->right_child->version.read();
    int size = left_child->size + right_child->size;

    version_node *new_version_node =
        version_pool.new_obj(size, left_child, right_child);

    if (!n->version.cas(old_version_node, new_version_node)) {
      version_pool.retire(new_version_node);
      return false;
    }

    version_pool.retire(old_version_node);
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
      node_pool.retire(p);
    }
  }

  void stats() {
    node_pool.stats();
    version_pool.stats();
  }

  void clear() {
    node_pool.clear();
    version_pool.clear();
  }

  void reserve(size_t n) {
    node_pool.reserve(n / 8);
    version_pool.reserve(n);
  }

  void shuffle(size_t n) {
    node_pool.shuffle(n / 8);
    version_pool.shuffle(n);
  }

  long check(node *root) { return root->version.load()->size; }
};
