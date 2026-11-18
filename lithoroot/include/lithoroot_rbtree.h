/* lithoroot_rbtree.h ... LithoRoot internal (S4, 2026-11-16): the one red-black
 * tree behind std::map and std::set (plan L6). Unique keys only (the census
 * finds no multimap/multiset). Node-based: references and iterators stay
 * valid until their element is erased. Ordered, so iteration order is a
 * pure function of the keys: safe for determinism hashes.
 *
 * Layout as in the classic header-sentinel design: header.parent = root,
 * header.left = leftmost, header.right = rightmost; end() is the header. */
#ifndef LITHOROOT_RBTREE_H
#define LITHOROOT_RBTREE_H
#if !defined(__wasm__) || !defined(__cplusplus)
#error "LithoRoot std headers are wasm32-only C++; native builds use their own C++ library"
#endif
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>

namespace std {
namespace __lk {

struct rb_node {
  rb_node* parent = nullptr;
  rb_node* left = nullptr;
  rb_node* right = nullptr;
  bool red = true;
};
inline rb_node* rb_min(rb_node* x) noexcept { while (x->left) x = x->left; return x; }
inline rb_node* rb_max(rb_node* x) noexcept { while (x->right) x = x->right; return x; }
/* Successor of a node that is not the rightmost (the iterator handles rightmost -> end). */
inline rb_node* rb_next(rb_node* x) noexcept {
  if (x->right) return rb_min(x->right);
  rb_node* p = x->parent;
  while (p && x == p->right) { x = p; p = p->parent; }
  return p;
}
inline rb_node* rb_prev(rb_node* x, rb_node* header) noexcept {
  if (x == header) return header->right;  // --end() is the rightmost
  if (x->left) return rb_max(x->left);
  rb_node* p = x->parent;
  while (p != header && x == p->left) { x = p; p = p->parent; }
  return p;
}

struct rb_core {
  rb_node header;  // header.parent: root; left: leftmost; right: rightmost
  size_t count = 0;
  rb_core() noexcept { reset(); }
  void reset() noexcept { header.parent = nullptr; header.left = header.right = &header; header.red = true; count = 0; }
  rb_node*& root() noexcept { return header.parent; }
  const rb_node* root() const noexcept { return header.parent; }

  void rotate_left(rb_node* x) noexcept {
    rb_node* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (x == root()) root() = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x; x->parent = y;
  }
  void rotate_right(rb_node* x) noexcept {
    rb_node* y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (x == root()) root() = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x; x->parent = y;
  }
  /* Link z as the left/right child of p (p == &header for the first node), then rebalance. */
  void insert_at(rb_node* z, rb_node* p, bool as_left) noexcept {
    z->left = z->right = nullptr; z->red = true;
    if (p == &header) { root() = z; z->parent = &header; header.left = header.right = z; }
    else {
      z->parent = p;
      if (as_left) { p->left = z; if (p == header.left) header.left = z; }
      else { p->right = z; if (p == header.right) header.right = z; }
    }
    ++count;
    rb_node* x = z;
    while (x != root() && x->parent->red) {
      rb_node* g = x->parent->parent;
      if (x->parent == g->left) {
        rb_node* u = g->right;
        if (u && u->red) { x->parent->red = false; u->red = false; g->red = true; x = g; }
        else {
          if (x == x->parent->right) { x = x->parent; rotate_left(x); }
          x->parent->red = false; g->red = true; rotate_right(g);
        }
      } else {
        rb_node* u = g->left;
        if (u && u->red) { x->parent->red = false; u->red = false; g->red = true; x = g; }
        else {
          if (x == x->parent->left) { x = x->parent; rotate_right(x); }
          x->parent->red = false; g->red = true; rotate_left(g);
        }
      }
    }
    root()->red = false;
  }
  /* Unlink z (not freed). CLRS delete with explicit parent tracking for a null x. */
  void erase_node(rb_node* z) noexcept {
    if (z == header.left) header.left = (z->right ? rb_min(z->right) : z->parent);
    if (z == header.right) header.right = (z->left ? rb_max(z->left) : z->parent);
    rb_node* y = z;
    rb_node* x;
    rb_node* xp;
    bool y_red = y->red;
    if (!z->left) { x = z->right; xp = z->parent; transplant(z, z->right); }
    else if (!z->right) { x = z->left; xp = z->parent; transplant(z, z->left); }
    else {
      y = rb_min(z->right);
      y_red = y->red;
      x = y->right;
      if (y->parent == z) xp = y;
      else { xp = y->parent; transplant(y, y->right); y->right = z->right; y->right->parent = y; }
      transplant(z, y);
      y->left = z->left; y->left->parent = y; y->red = z->red;
    }
    --count;
    if (count == 0) { reset(); return; }
    if (!y_red) erase_fixup(x, xp);
  }

 private:
  void transplant(rb_node* u, rb_node* v) noexcept {
    if (u == root()) root() = v;
    else if (u == u->parent->left) u->parent->left = v;
    else u->parent->right = v;
    if (v) v->parent = u->parent;
  }
  static bool is_red(rb_node* n) noexcept { return n && n->red; }
  void erase_fixup(rb_node* x, rb_node* xp) noexcept {
    while (x != root() && !is_red(x)) {
      if (x == xp->left) {
        rb_node* w = xp->right;
        if (is_red(w)) { w->red = false; xp->red = true; rotate_left(xp); w = xp->right; }
        if (!is_red(w->left) && !is_red(w->right)) { w->red = true; x = xp; xp = x->parent; }
        else {
          if (!is_red(w->right)) { w->left->red = false; w->red = true; rotate_right(w); w = xp->right; }
          w->red = xp->red; xp->red = false; if (w->right) w->right->red = false;
          rotate_left(xp); x = root(); break;
        }
      } else {
        rb_node* w = xp->left;
        if (is_red(w)) { w->red = false; xp->red = true; rotate_right(xp); w = xp->left; }
        if (!is_red(w->right) && !is_red(w->left)) { w->red = true; x = xp; xp = x->parent; }
        else {
          if (!is_red(w->left)) { w->right->red = false; w->red = true; rotate_left(w); w = xp->left; }
          w->red = xp->red; xp->red = false; if (w->left) w->left->red = false;
          rotate_right(xp); x = root(); break;
        }
      }
    }
    if (x) x->red = false;
  }
};

template <class V> struct rb_vnode : rb_node { V val; template <class... A> explicit rb_vnode(A&&... a) : val(std::forward<A>(a)...) {} };

template <class V, bool Const> class rb_iter {
  template <class, bool> friend class rb_iter;
  template <class, class, class, class> friend class rb_tree;
  rb_node* n_ = nullptr;
  rb_node* h_ = nullptr;  // header, for --end()
 public:
  using iterator_category = bidirectional_iterator_tag;
  using value_type = V;
  using difference_type = ptrdiff_t;
  using pointer = conditional_t<Const, const V*, V*>;
  using reference = conditional_t<Const, const V&, V&>;
  rb_iter() = default;
  rb_iter(rb_node* n, rb_node* h) noexcept : n_(n), h_(h) {}
  template <bool C = Const, class = enable_if_t<C>> rb_iter(const rb_iter<V, false>& o) noexcept : n_(o.n_), h_(o.h_) {}
  reference operator*() const noexcept { return static_cast<rb_vnode<V>*>(n_)->val; }
  pointer operator->() const noexcept { return addressof(static_cast<rb_vnode<V>*>(n_)->val); }
  rb_iter& operator++() noexcept { n_ = n_ == h_->right ? h_ : rb_next(n_); return *this; }
  rb_iter operator++(int) noexcept { auto t = *this; ++*this; return t; }
  rb_iter& operator--() noexcept { n_ = rb_prev(n_, h_); return *this; }
  rb_iter operator--(int) noexcept { auto t = *this; --*this; return t; }
  friend bool operator==(const rb_iter& a, const rb_iter& b) noexcept { return a.n_ == b.n_; }
};

/* KeyOf maps a stored value to its key (identity for set, .first for map). */
template <class K, class V, class KeyOf, class Cmp>
class rb_tree {
 public:
  using node = rb_vnode<V>;
  using iterator = rb_iter<V, false>;
  using const_iterator = rb_iter<V, true>;

  rb_tree() = default;
  explicit rb_tree(const Cmp& c) : cmp_(c) {}
  rb_tree(const rb_tree& o) : cmp_(o.cmp_) { for (const auto& v : o) emplace_hint_unique(end(), v); }
  rb_tree(rb_tree&& o) noexcept : cmp_(o.cmp_) { take(o); }
  rb_tree& operator=(const rb_tree& o) { if (this != &o) { clear(); cmp_ = o.cmp_; for (const auto& v : o) emplace_hint_unique(end(), v); } return *this; }
  rb_tree& operator=(rb_tree&& o) noexcept { if (this != &o) { clear(); cmp_ = o.cmp_; take(o); } return *this; }
  ~rb_tree() { clear(); }

  iterator begin() noexcept { return iterator(c_.header.left, &c_.header); }
  const_iterator begin() const noexcept { return const_iterator(const_cast<rb_node*>(c_.header.left), hdr()); }
  iterator end() noexcept { return iterator(&c_.header, &c_.header); }
  const_iterator end() const noexcept { return const_iterator(hdr(), hdr()); }
  size_t size() const noexcept { return c_.count; }
  bool empty() const noexcept { return c_.count == 0; }
  const Cmp& cmp() const noexcept { return cmp_; }

  void clear() noexcept { destroy(c_.root()); c_.reset(); }
  void swap(rb_tree& o) noexcept { rb_tree t(std::move(o)); o.take(*this); take(t); std::swap(cmp_, o.cmp_); }

  template <class Q> iterator lower_bound(const Q& k) { return iterator(lb(k), &c_.header); }
  template <class Q> const_iterator lower_bound(const Q& k) const { return const_iterator(const_cast<rb_tree*>(this)->lb(k), hdr()); }
  template <class Q> iterator upper_bound(const Q& k) { return iterator(ub(k), &c_.header); }
  template <class Q> const_iterator upper_bound(const Q& k) const { return const_iterator(const_cast<rb_tree*>(this)->ub(k), hdr()); }
  template <class Q> iterator find(const Q& k) { rb_node* n = lb(k); return n != &c_.header && !cmp_(k, key(n)) ? iterator(n, &c_.header) : end(); }
  template <class Q> const_iterator find(const Q& k) const { return const_cast<rb_tree*>(this)->find(k); }

  /* Insert a node if its key is absent; the node is freed otherwise. */
  template <class... A> pair<iterator, bool> emplace_unique(A&&... a) { return insert_node(make(std::forward<A>(a)...)); }
  /* Link z if its key is absent; otherwise free z and return the existing element. */
  pair<iterator, bool> insert_node(node* z) {
    rb_node* p = &c_.header;
    rb_node* x = c_.root();
    bool left = true;
    while (x) { p = x; left = cmp_(key(z), key(x)); x = left ? x->left : x->right; }
    /* Duplicate check: the candidate equal node is p (went right) or prev(p) (went left). */
    rb_node* j = p;
    if (left) {
      if (j == c_.header.left || j == &c_.header) { c_.insert_at(z, p, true); return {iterator(z, &c_.header), true}; }
      j = rb_prev(j, &c_.header);
    }
    if (cmp_(key(j), key(z))) { c_.insert_at(z, p, left); return {iterator(z, &c_.header), true}; }
    drop(z);
    return {iterator(j, &c_.header), false};
  }
  /* Find-or-insert by key without building a node when present (map::operator[], try_emplace). */
  template <class Q, class Make> pair<iterator, bool> find_or_make(const Q& k, Make&& make_node) {
    rb_node* p = &c_.header;
    rb_node* x = c_.root();
    bool left = true;
    while (x) {
      p = x;
      if (cmp_(k, key(x))) { left = true; x = x->left; }
      else if (cmp_(key(x), k)) { left = false; x = x->right; }
      else return {iterator(x, &c_.header), false};
    }
    node* z = make_node();
    c_.insert_at(z, p, left);
    return {iterator(z, &c_.header), true};
  }
  template <class... A> iterator emplace_hint_unique(const_iterator hint, A&&... a) {
    if (hint.n_ == &c_.header && !empty()) {  // fast path: appending in order (copying a tree)
      node* z = make(std::forward<A>(a)...);
      if (cmp_(key(c_.header.right), key(z))) { c_.insert_at(z, c_.header.right, false); return iterator(z, &c_.header); }
      return insert_node(z).first;  // not past the end: ordinary insert of the built node
    }
    return emplace_unique(std::forward<A>(a)...).first;
  }
  iterator erase(const_iterator it) noexcept {
    rb_node* n = it.n_;
    iterator next(n, &c_.header); ++next;
    c_.erase_node(n);
    drop(static_cast<node*>(n));
    return next;
  }
  iterator erase(const_iterator f, const_iterator l) noexcept {
    if (f == begin() && l == end()) { clear(); return end(); }
    while (f != l) f = erase(f);
    return iterator(l.n_, &c_.header);
  }
  template <class Q> size_t erase_key(const Q& k) { auto it = find(k); if (it == end()) return 0; erase(it); return 1; }
  /* Debug check used by the tests: red-black invariants and order. Returns black height or -1. */
  int verify() const noexcept { return verify(c_.root()); }

  static const K& key(const rb_node* n) noexcept { return KeyOf()(static_cast<const node*>(n)->val); }

 private:
  rb_core c_;
  [[no_unique_address]] Cmp cmp_{};

  rb_node* hdr() const noexcept { return const_cast<rb_node*>(&c_.header); }
  template <class... A> static node* make(A&&... a) { return new node(std::forward<A>(a)...); }
  static void drop(node* n) noexcept { delete n; }
  void destroy(rb_node* x) noexcept {
    while (x) { destroy(x->right); rb_node* l = x->left; drop(static_cast<node*>(x)); x = l; }
  }
  void take(rb_tree& o) noexcept {
    if (o.empty()) { c_.reset(); return; }
    c_.header = o.c_.header; c_.count = o.c_.count;
    c_.root()->parent = &c_.header;
    o.c_.reset();
  }
  template <class Q> rb_node* lb(const Q& k) {
    rb_node* r = &c_.header; rb_node* x = c_.root();
    while (x) { if (!cmp_(key(x), k)) { r = x; x = x->left; } else x = x->right; }
    return r;
  }
  template <class Q> rb_node* ub(const Q& k) {
    rb_node* r = &c_.header; rb_node* x = c_.root();
    while (x) { if (cmp_(k, key(x))) { r = x; x = x->left; } else x = x->right; }
    return r;
  }
  int verify(const rb_node* x) const noexcept {
    if (!x) return 1;
    if (x->red && ((x->left && x->left->red) || (x->right && x->right->red))) return -1;
    if (x->left && (x->left->parent != x || !cmp_(key(x->left), key(x)))) return -1;
    if (x->right && (x->right->parent != x || !cmp_(key(x), key(x->right)))) return -1;
    const int l = verify(x->left), r = verify(x->right);
    if (l < 0 || r < 0 || l != r) return -1;
    return l + (x->red ? 0 : 1);
  }
};

}  // namespace __lk
}  // namespace std
#endif
