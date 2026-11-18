/* lithoroot_hashtable.h ... LithoRoot internal (S4, 2026-11-16): the one hash
 * table behind unordered_map and unordered_set (plan L6). Node-based
 * chaining, so references and pointers to elements stay valid across rehash
 * (iterators are invalidated by rehash, as the standard allows). Power-of-two
 * bucket counts; the hash is mixed (murmur3 finaliser) before masking, since
 * std::hash of integers is the identity. Max load factor 1.0 by default.
 *
 * DETERMINISM RULE (plan 3.7): iteration order depends on hash values and
 * insertion history and differs from MSVC's. Nothing that feeds a
 * determinism hash (ledger, frame hash, diamond, scene) may iterate an
 * unordered container. */
#ifndef LITHOROOT_HASHTABLE_H
#define LITHOROOT_HASHTABLE_H
#if !defined(__wasm__) || !defined(__cplusplus)
#error "LithoRoot std headers are wasm32-only C++; native builds use their own C++ library"
#endif
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace std {
namespace __lk {

struct ht_link { ht_link* next = nullptr; size_t hash = 0; };
template <class V> struct ht_node : ht_link { V val; template <class... A> explicit ht_node(A&&... a) : val(std::forward<A>(a)...) {} };

inline size_t ht_mix(size_t h) noexcept {
  h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
  return h;
}

template <class V, bool Const> class ht_iter {
  template <class, bool> friend class ht_iter;
  template <class, class, class, class, class> friend class hashtable;
  ht_link* n_ = nullptr;
  ht_link* const* b_ = nullptr;  // bucket array
  size_t bi_ = 0, bc_ = 0;       // current bucket, bucket count
  void skip() noexcept { while (!n_ && ++bi_ < bc_) n_ = b_[bi_]; }
 public:
  using iterator_category = forward_iterator_tag;
  using value_type = V;
  using difference_type = ptrdiff_t;
  using pointer = conditional_t<Const, const V*, V*>;
  using reference = conditional_t<Const, const V&, V&>;
  ht_iter() = default;
  ht_iter(ht_link* n, ht_link* const* b, size_t bi, size_t bc) noexcept : n_(n), b_(b), bi_(bi), bc_(bc) {}
  template <bool C = Const, class = enable_if_t<C>> ht_iter(const ht_iter<V, false>& o) noexcept : n_(o.n_), b_(o.b_), bi_(o.bi_), bc_(o.bc_) {}
  reference operator*() const noexcept { return static_cast<ht_node<V>*>(n_)->val; }
  pointer operator->() const noexcept { return addressof(static_cast<ht_node<V>*>(n_)->val); }
  ht_iter& operator++() noexcept { n_ = n_->next; skip(); return *this; }
  ht_iter operator++(int) noexcept { auto t = *this; ++*this; return t; }
  friend bool operator==(const ht_iter& a, const ht_iter& b) noexcept { return a.n_ == b.n_; }
};

template <class K, class V, class KeyOf, class H, class Eq>
class hashtable {
 public:
  using node = ht_node<V>;
  using iterator = ht_iter<V, false>;
  using const_iterator = ht_iter<V, true>;

  hashtable() = default;
  hashtable(size_t n, const H& h, const Eq& e) : h_(h), eq_(e) { if (n) rehash(n); }
  hashtable(const hashtable& o) : h_(o.h_), eq_(o.eq_), mlf_(o.mlf_) { reserve(o.n_); for (const auto& v : o) insert_unique(new node(v)); }
  hashtable(hashtable&& o) noexcept : b_(o.b_), bc_(o.bc_), n_(o.n_), h_(std::move(o.h_)), eq_(std::move(o.eq_)), mlf_(o.mlf_) { o.b_ = nullptr; o.bc_ = o.n_ = 0; }
  hashtable& operator=(const hashtable& o) { if (this != &o) { hashtable t(o); swap(t); } return *this; }
  hashtable& operator=(hashtable&& o) noexcept { if (this != &o) { hashtable t(std::move(o)); swap(t); } return *this; }
  ~hashtable() { clear(); ::operator delete(b_); }
  void swap(hashtable& o) noexcept {
    std::swap(b_, o.b_); std::swap(bc_, o.bc_); std::swap(n_, o.n_); std::swap(h_, o.h_); std::swap(eq_, o.eq_); std::swap(mlf_, o.mlf_);
  }

  iterator begin() noexcept { iterator it(bc_ ? b_[0] : nullptr, b_, 0, bc_); if (bc_) it.skip(); return it; }
  const_iterator begin() const noexcept { return const_cast<hashtable*>(this)->begin(); }
  iterator end() noexcept { return iterator(nullptr, b_, bc_, bc_); }
  const_iterator end() const noexcept { return const_cast<hashtable*>(this)->end(); }
  size_t size() const noexcept { return n_; }
  bool empty() const noexcept { return n_ == 0; }
  size_t bucket_count() const noexcept { return bc_; }
  float load_factor() const noexcept { return bc_ ? float(n_) / float(bc_) : 0.0f; }
  float max_load_factor() const noexcept { return mlf_; }
  void max_load_factor(float f) noexcept { mlf_ = f > 0.0f ? f : 1.0f; }
  const H& hasher_ref() const noexcept { return h_; }
  const Eq& eq_ref() const noexcept { return eq_; }

  template <class Q> iterator find(const Q& k) {
    if (!bc_) return end();
    const size_t h = hash_of(k), bi = h & (bc_ - 1);
    for (ht_link* p = b_[bi]; p; p = p->next)
      if (p->hash == h && eq_(key(p), k)) return iterator(p, b_, bi, bc_);
    return end();
  }
  template <class Q> const_iterator find(const Q& k) const { return const_cast<hashtable*>(this)->find(k); }

  /* Link z unless its key exists (then z is freed). */
  pair<iterator, bool> insert_unique(node* z) {
    z->hash = hash_of(key(z));
    if (bc_) {
      const size_t bi = z->hash & (bc_ - 1);
      for (ht_link* p = b_[bi]; p; p = p->next)
        if (p->hash == z->hash && eq_(key(p), key(z))) { delete z; return {iterator(p, b_, bi, bc_), false}; }
    }
    return {link(z), true};
  }
  template <class Q, class Make> pair<iterator, bool> find_or_make(const Q& k, Make&& make_node) {
    auto it = find(k);
    if (it != end()) return {it, false};
    node* z = make_node();
    z->hash = hash_of(key(z));
    return {link(z), true};
  }
  iterator erase(const_iterator it) noexcept {
    ht_link* z = it.n_;
    iterator next(z, b_, it.bi_, bc_); ++next;
    ht_link** pp = &b_[it.bi_];
    while (*pp != z) pp = &(*pp)->next;
    *pp = z->next;
    --n_;
    delete static_cast<node*>(z);
    return next;
  }
  iterator unconst(const_iterator it) noexcept { return iterator(it.n_, b_, it.bi_, bc_); }
  template <class Q> size_t erase_key(const Q& k) { auto it = find(k); if (it == end()) return 0; erase(it); return 1; }
  void clear() noexcept {
    for (size_t i = 0; i < bc_; ++i) {
      for (ht_link* p = b_[i]; p;) { ht_link* nx = p->next; delete static_cast<node*>(p); p = nx; }
      b_[i] = nullptr;
    }
    n_ = 0;
  }
  void rehash(size_t n) {
    size_t need = size_t(float(n_) / mlf_) + 1;
    if (n < need) n = need;
    size_t nb = 8;
    while (nb < n) nb <<= 1;
    if (nb == bc_) return;
    ht_link** b = static_cast<ht_link**>(::operator new(nb * sizeof(ht_link*)));
    for (size_t i = 0; i < nb; ++i) b[i] = nullptr;
    for (size_t i = 0; i < bc_; ++i)
      for (ht_link* p = b_[i]; p;) { ht_link* nx = p->next; size_t bi = p->hash & (nb - 1); p->next = b[bi]; b[bi] = p; p = nx; }
    ::operator delete(b_);
    b_ = b; bc_ = nb;
  }
  void reserve(size_t n) { if (float(n) > mlf_ * float(bc_)) rehash(size_t(float(n) / mlf_) + 1); }

 private:
  ht_link** b_ = nullptr;
  size_t bc_ = 0;
  size_t n_ = 0;
  [[no_unique_address]] H h_{};
  [[no_unique_address]] Eq eq_{};
  float mlf_ = 1.0f;

  static const K& key(const ht_link* n) noexcept { return KeyOf()(static_cast<const node*>(n)->val); }
  template <class Q> size_t hash_of(const Q& k) const { return ht_mix(h_(k)); }
  iterator link(node* z) {
    if (float(n_ + 1) > mlf_ * float(bc_)) rehash(bc_ ? bc_ * 2 : 8);
    const size_t bi = z->hash & (bc_ - 1);
    z->next = b_[bi]; b_[bi] = z;
    ++n_;
    return iterator(z, b_, bi, bc_);
  }
};

}  // namespace __lk
}  // namespace std
#endif
