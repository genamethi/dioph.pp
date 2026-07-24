#include "primeparts/graph/pp_graph_store.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <istream>
#include <thread>

namespace primeparts::graph {

GiNaC::ex He(int n, const GiNaC::symbol& x) {
  GiNaC::ex a = 1;
  GiNaC::ex b = x;
  if (n == 0) return a;
  for (int k = 1; k < n; ++k) {
    GiNaC::ex c = GiNaC::expand(x * b - k * a);
    a = b;
    b = c;
  }
  return b;
}

std::map<int, GiNaC::ex> ToHermite(GiNaC::ex P, const GiNaC::symbol& x) {
  std::map<int, GiNaC::ex> out;
  P = GiNaC::expand(P);
  while (!P.is_zero()) {
    int d = P.degree(x);
    GiNaC::ex c = P.lcoeff(x);
    out[d] = c;
    P = GiNaC::expand(P - c * He(d, x));
    if (d == 0) break;
  }
  return out;
}

std::string JsonInts(const std::vector<int64_t>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ", ";
    s += std::to_string(v[i]);
  }
  s += "]";
  return s;
}

Features WordFeatures(int64_t a0,
                      const std::vector<std::pair<int32_t, int64_t>>& segs,
                      const GiNaC::symbol& x) {
  GiNaC::ex p = x + GiNaC::numeric(static_cast<long>(a0));
  std::vector<int64_t> sk;
  std::vector<int64_t> tr{a0};
  int64_t degree = 1;
  for (const auto& [n, c] : segs) {
    p = GiNaC::pow(p, n) + GiNaC::numeric(static_cast<long>(c));
    sk.push_back(n);
    tr.push_back(c);
    degree *= n;
  }
  auto hd = ToHermite(p, x);
  Features f;
  f.degree = degree;
  f.skeleton = JsonInts(sk);
  f.translations = JsonInts(tr);
  std::string h = "{";
  bool first = true;
  for (const auto& [k, coef] : hd) {
    if (!first) h += ",";
    first = false;
    std::ostringstream cs;
    cs << coef;
    h += "\"" + std::to_string(k) + "\":" + cs.str();
  }
  h += "}";
  f.hermite = h;
  std::ostringstream ss;
  ss << GiNaC::expand(p);
  f.symbolic = ss.str();
  return f;
}

int32_t ComposeOnto(ConcStore& tgt, const LoadedStore& src, int32_t base,
                    int32_t upper) {
  int64_t a0;
  std::vector<std::pair<int32_t, int64_t>> segs;
  src.Reconstruct(upper, &a0, &segs);
  int32_t t = base;
  if (a0 != 0) t = tgt.Push(t, 1, a0);
  for (const auto& [n, c] : segs) t = tgt.Push(t, n, c);
  return t;
}

namespace {

constexpr uint32_t kMagic = 0x50504c47;
constexpr uint32_t kVersion = 1;

template <class T>
void WPod(std::ostream& o, const T& v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T>
void WVec(std::ostream& o, const std::vector<T>& v) {
  uint64_t n = v.size();
  WPod(o, n);
  if (n) o.write(reinterpret_cast<const char*>(v.data()), n * sizeof(T));
}
template <class T>
bool RPod(std::istream& i, T* v) {
  return static_cast<bool>(i.read(reinterpret_cast<char*>(v), sizeof(T)));
}
template <class T>
bool RVec(std::istream& i, std::vector<T>* v) {
  uint64_t n = 0;
  if (!RPod(i, &n)) return false;
  v->resize(n);
  if (n)
    return static_cast<bool>(
        i.read(reinterpret_cast<char*>(v->data()), n * sizeof(T)));
  return true;
}

}  // namespace

std::string SliceLogPath(const std::string& dir, int64_t slice) {
  return dir + "/slice-" + std::to_string(slice) + ".pplog";
}

bool SpillSlice(const std::string& dir, int64_t slice, ConcStore& store,
                const std::unordered_map<int64_t, std::vector<MemoEntry>>& memo,
                std::string* error) {
  std::unordered_map<int32_t, int32_t> seg_map;
  std::vector<int32_t> seg_parent, seg_n;
  std::vector<int64_t> seg_c;
  for (int s = 0; s < ConcStore::SH; ++s) {
    auto& sh = store.sseg[s];
    for (size_t local = 0; local < sh.parent.size(); ++local) {
      int32_t gid = (s << 23) | static_cast<int32_t>(local);
      seg_map.emplace(gid, static_cast<int32_t>(seg_parent.size()));
      seg_parent.push_back(sh.parent[local]);
      seg_n.push_back(sh.n[local]);
      seg_c.push_back(sh.c[local]);
    }
  }
  for (auto& p : seg_parent)
    if (p >= 0) p = seg_map[p];

  std::unordered_map<int32_t, int32_t> word_map;
  std::vector<int64_t> word_a0;
  std::vector<int32_t> word_tail;
  for (int s = 0; s < ConcStore::SH; ++s) {
    auto& sh = store.sword[s];
    for (size_t local = 0; local < sh.a0.size(); ++local) {
      int32_t gid = (s << 23) | static_cast<int32_t>(local);
      word_map.emplace(gid, static_cast<int32_t>(word_a0.size()));
      word_a0.push_back(sh.a0[local]);
      word_tail.push_back(sh.tail[local]);
    }
  }
  for (auto& t : word_tail)
    if (t >= 0) t = seg_map[t];
  int32_t seed_dense = word_map[store.seed];

  std::ofstream out(SliceLogPath(dir, slice), std::ios::binary);
  if (!out) {
    if (error) *error = "spill: cannot open " + SliceLogPath(dir, slice);
    return false;
  }
  WPod(out, kMagic);
  WPod(out, kVersion);
  WPod(out, slice);
  WPod(out, seed_dense);
  WVec(out, seg_parent);
  WVec(out, seg_n);
  WVec(out, seg_c);
  WVec(out, word_a0);
  WVec(out, word_tail);

  uint64_t nodes = memo.size();
  WPod(out, nodes);
  for (const auto& [node, entries] : memo) {
    WPod(out, node);
    uint64_t cnt = entries.size();
    WPod(out, cnt);
    for (const auto& e : entries) {
      int32_t w = word_map[e.word];
      WPod(out, w);
      WPod(out, e.conn);
    }
  }
  out.flush();
  if (!out) {
    if (error) *error = "spill: write failed for slice " + std::to_string(slice);
    return false;
  }
  return true;
}

bool LoadSlice(const std::string& dir, int64_t slice, SliceData* out,
               std::string* error) {
  out->store = LoadedStore{};
  out->memo.clear();
  std::ifstream in(SliceLogPath(dir, slice), std::ios::binary);
  if (!in) return true;

  uint32_t magic = 0, version = 0;
  int64_t got_slice = 0;
  if (!RPod(in, &magic) || magic != kMagic || !RPod(in, &version) ||
      version != kVersion || !RPod(in, &got_slice)) {
    if (error) *error = "load: bad header in slice " + std::to_string(slice);
    return false;
  }
  if (!RPod(in, &out->store.seed) || !RVec(in, &out->store.seg_parent) ||
      !RVec(in, &out->store.seg_n) || !RVec(in, &out->store.seg_c) ||
      !RVec(in, &out->store.word_a0) || !RVec(in, &out->store.word_tail)) {
    if (error) *error = "load: truncated store in slice " + std::to_string(slice);
    return false;
  }
  uint64_t nodes = 0;
  if (!RPod(in, &nodes)) {
    if (error) *error = "load: missing memo in slice " + std::to_string(slice);
    return false;
  }
  out->memo.reserve(nodes);
  for (uint64_t i = 0; i < nodes; ++i) {
    int64_t node = 0;
    uint64_t cnt = 0;
    if (!RPod(in, &node) || !RPod(in, &cnt)) {
      if (error) *error = "load: truncated memo in slice " + std::to_string(slice);
      return false;
    }
    auto& v = out->memo[node];
    v.resize(cnt);
    for (uint64_t j = 0; j < cnt; ++j) {
      if (!RPod(in, &v[j].word) || !RPod(in, &v[j].conn)) {
        if (error)
          *error = "load: truncated entry in slice " + std::to_string(slice);
        return false;
      }
    }
  }
  return true;
}

bool SweepAndSpill(const std::vector<int64_t>& nodes,
                   const std::unordered_map<int64_t, std::vector<Edge>>& parents,
                   int64_t slice_width, const std::string& dir,
                   SweepResult* out, std::string* error) {
  auto slice_of = [&](int64_t p) { return p / slice_width; };
  const int64_t num_slices = nodes.empty() ? 0 : slice_of(nodes.back()) + 1;
  out->num_slices = num_slices;
  size_t ni = 0;
  for (int64_t s = 0; s < num_slices; ++s) {
    ConcStore store;
    std::unordered_map<int64_t, std::vector<MemoEntry>> memo;
    while (ni < nodes.size() && slice_of(nodes[ni]) == s) {
      const int64_t v = nodes[ni++];
      std::unordered_set<MemoEntry, MemoEntryHash> res;
      auto it = parents.find(v);
      if (it == parents.end()) {
        res.insert({store.seed, -1});
      } else {
        for (const auto& e : it->second) {
          const int64_t c = int64_t{1} << e.m;
          if (slice_of(e.q) < s) {
            int64_t& lr = out->last_ref_slice[e.q];
            if (s > lr) lr = s;
            res.insert({store.Push(store.seed, e.n, c), e.q});
          } else {
            auto pit = memo.find(e.q);
            if (pit == memo.end()) continue;
            for (const auto& me : pit->second)
              res.insert({store.Push(me.word, e.n, c), me.conn});
          }
        }
      }
      memo[v].assign(res.begin(), res.end());
    }
    if (!SpillSlice(dir, s, store, memo, error)) return false;

    for (const auto& [node, entries] : memo) {
      out->memo_entries += static_cast<int64_t>(entries.size());
      for (const auto& e : entries)
        if (e.conn >= 0) ++out->connections;
    }
    for (int i = 0; i < ConcStore::SH; ++i) {
      out->words += static_cast<int64_t>(store.sword[i].a0.size());
      out->segs += static_cast<int64_t>(store.sseg[i].parent.size());
    }
    std::error_code ec;
    out->spill_bytes +=
        static_cast<int64_t>(std::filesystem::file_size(SliceLogPath(dir, s), ec));
  }
  return true;
}

SliceSweeper::SliceSweeper(std::string dir, int64_t slice_width)
    : dir_(std::move(dir)), slice_width_(slice_width) {}

SliceSweeper::~SliceSweeper() = default;

bool SliceSweeper::FlushSlice(std::string* error) {
  if (!store_) return true;
  if (!SpillSlice(dir_, cur_slice_, *store_, memo_, error)) return false;
  for (const auto& [node, entries] : memo_) {
    result_.memo_entries += static_cast<int64_t>(entries.size());
    for (const auto& e : entries)
      if (e.conn >= 0) ++result_.connections;
  }
  for (int i = 0; i < ConcStore::SH; ++i) {
    result_.words += static_cast<int64_t>(store_->sword[i].a0.size());
    result_.segs += static_cast<int64_t>(store_->sseg[i].parent.size());
  }
  std::error_code ec;
  result_.spill_bytes += static_cast<int64_t>(
      std::filesystem::file_size(SliceLogPath(dir_, cur_slice_), ec));
  store_.reset();
  memo_.clear();
  return true;
}

bool SliceSweeper::AddNode(int64_t p, bool is_root,
                           const std::vector<Edge>& edges, std::string* error) {
  if (p <= last_p_) {
    if (error) *error = "SliceSweeper: nodes must arrive in ascending p";
    return false;
  }
  last_p_ = p;
  const int64_t s = p / slice_width_;
  if (s != cur_slice_) {
    if (!FlushSlice(error)) return false;
    store_ = std::make_unique<ConcStore>();
    cur_slice_ = s;
  }
  ++result_.nodes;
  std::unordered_set<MemoEntry, MemoEntryHash> res;
  if (is_root || edges.empty()) {
    res.insert({store_->seed, -1});
  } else {
    for (const auto& e : edges) {
      result_.has_children.insert(e.q);
      const int64_t c = int64_t{1} << e.m;
      if (e.q / slice_width_ < s) {
        int64_t& lr = result_.last_ref_slice[e.q];
        if (s > lr) lr = s;
        res.insert({store_->Push(store_->seed, e.n, c), e.q});
      } else {
        auto pit = memo_.find(e.q);
        if (pit == memo_.end()) continue;
        for (const auto& me : pit->second)
          res.insert({store_->Push(me.word, e.n, c), me.conn});
      }
    }
  }
  memo_[p].assign(res.begin(), res.end());
  return true;
}

bool SliceSweeper::Finish(std::string* error) {
  if (!FlushSlice(error)) return false;
  result_.num_slices = cur_slice_ + 1;
  return true;
}

namespace {

std::unordered_map<int64_t, std::vector<int64_t>> EvictBuckets(
    const std::unordered_map<int64_t, int64_t>& last_ref_slice) {
  std::unordered_map<int64_t, std::vector<int64_t>> evict_at;
  for (const auto& [node, s] : last_ref_slice) evict_at[s].push_back(node);
  return evict_at;
}

}  // namespace

int32_t SkeletonDict::Intern(const std::vector<int32_t>& sk) {
  auto it = ix.find(sk);
  if (it != ix.end()) return it->second;
  int32_t id = static_cast<int32_t>(tuples.size());
  ix.emplace(sk, id);
  tuples.push_back(sk);
  return id;
}

bool BuildCoproductSlice(const std::string& dir, int64_t slice,
                         SkeletonDict* dict, CoproductColumns* out,
                         std::string* error) {
  SliceData sd;
  if (!LoadSlice(dir, slice, &sd, error)) return false;
  const int64_t base = slice << kWordSliceShift;

  const size_t nwords = sd.store.word_a0.size();
  int64_t a0;
  std::vector<std::pair<int32_t, int64_t>> segs;
  std::vector<int32_t> skel;
  for (size_t w = 0; w < nwords; ++w) {
    sd.store.Reconstruct(static_cast<int32_t>(w), &a0, &segs);
    if (static_cast<int>(segs.size()) > kMaxSegments) {
      if (error)
        *error = "coproduct: word with r=" + std::to_string(segs.size()) +
                 " exceeds kMaxSegments in slice " + std::to_string(slice);
      return false;
    }
    skel.clear();
    int64_t cvals[kMaxSegments] = {0, 0, 0, 0};
    for (size_t i = 0; i < segs.size(); ++i) {
      skel.push_back(segs[i].first);
      cvals[i] = segs[i].second;
    }
    out->word_id.push_back(base | static_cast<int64_t>(w));
    out->a0.push_back(a0);
    out->skeleton_id.push_back(dict->Intern(skel));
    for (int i = 0; i < kMaxSegments; ++i) out->c[i].push_back(cvals[i]);
    out->word_slice.push_back(slice);
  }

  for (const auto& [node, entries] : sd.memo) {
    for (const auto& e : entries) {
      out->nw_node.push_back(node);
      out->nw_word.push_back(base | static_cast<int64_t>(e.word));
      out->nw_conn.push_back(e.conn);
    }
  }
  return true;
}

ParResult ReassembleFromDisk(
    const std::string& dir, int64_t num_slices,
    const std::unordered_set<int64_t>& has_children,
    const std::unordered_map<int64_t, int64_t>& last_ref_slice,
    std::string* error) {
  ConcStore global;
  std::unordered_map<int64_t, std::vector<int32_t>> cache;
  std::unordered_set<int32_t> distinct;
  ParResult r;
  auto evict_at = EvictBuckets(last_ref_slice);

  for (int64_t s = 0; s < num_slices; ++s) {
    SliceData sd;
    if (!LoadSlice(dir, s, &sd, error)) return r;
    for (const auto& [node, entries] : sd.memo) {
      std::unordered_set<int32_t> comps;
      for (const auto& me : entries) {
        if (me.conn < 0) {
          comps.insert(ComposeOnto(global, sd.store, global.seed, me.word));
        } else {
          auto it = cache.find(me.conn);
          if (it == cache.end()) continue;
          for (int32_t cw : it->second)
            comps.insert(ComposeOnto(global, sd.store, cw, me.word));
        }
      }
      if (has_children.count(node)) {
        cache[node].assign(comps.begin(), comps.end());
      } else {
        for (int32_t w : comps) {
          if (w == global.seed) continue;
          distinct.insert(w);
          ++r.node_word_pairs;
          int64_t d = global.Degree(w);
          if (d > r.max_degree) r.max_degree = d;
        }
      }
    }
    auto ev = evict_at.find(s);
    if (ev != evict_at.end())
      for (int64_t node : ev->second) cache.erase(node);
  }
  r.distinct_words = static_cast<int64_t>(distinct.size());
  return r;
}

bool ReassembleForMaterialize(
    const std::string& dir, int64_t num_slices,
    const std::unordered_set<int64_t>& has_children,
    const std::unordered_map<int64_t, int64_t>& last_ref_slice, int workers,
    ReassembleOutput* out, std::string* error) {
  ConcStore global;
  std::unordered_map<int64_t, std::vector<int32_t>> cache;
  std::vector<std::pair<int64_t, int32_t>> sink_gid;
  std::unordered_set<int32_t> distinct;
  int64_t maxdeg = 0;
  auto evict_at = EvictBuckets(last_ref_slice);
  const int nthreads = workers < 1 ? 1 : workers;

  for (int64_t s = 0; s < num_slices; ++s) {
    SliceData sd;
    if (!LoadSlice(dir, s, &sd, error)) return false;

    std::vector<std::pair<int64_t, const std::vector<MemoEntry>*>> items;
    items.reserve(sd.memo.size());
    for (const auto& kv : sd.memo) items.emplace_back(kv.first, &kv.second);

    std::vector<std::vector<int32_t>> comps_out(items.size());
    std::atomic<size_t> next{0};
    auto worker = [&]() {
      for (;;) {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= items.size()) return;
        std::unordered_set<int32_t> comps;
        for (const auto& me : *items[i].second) {
          if (me.conn < 0) {
            comps.insert(ComposeOnto(global, sd.store, global.seed, me.word));
          } else {
            auto it = cache.find(me.conn);
            if (it == cache.end()) continue;
            for (int32_t cw : it->second)
              comps.insert(ComposeOnto(global, sd.store, cw, me.word));
          }
        }
        comps_out[i].assign(comps.begin(), comps.end());
      }
    };
    if (nthreads == 1 || items.size() < 2) {
      worker();
    } else {
      std::vector<std::thread> pool;
      for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
      for (auto& th : pool) th.join();
    }

    for (size_t i = 0; i < items.size(); ++i) {
      const int64_t node = items[i].first;
      if (has_children.count(node)) {
        cache[node] = std::move(comps_out[i]);
      } else {
        for (int32_t w : comps_out[i]) {
          if (w == global.seed) continue;
          sink_gid.emplace_back(node, w);
          distinct.insert(w);
          int64_t d = global.Degree(w);
          if (d > maxdeg) maxdeg = d;
        }
      }
    }
    auto ev = evict_at.find(s);
    if (ev != evict_at.end())
      for (int64_t node : ev->second) cache.erase(node);
  }

  std::vector<int32_t> uniq(distinct.begin(), distinct.end());
  const size_t U = uniq.size();
  std::vector<int64_t> a0s(U);
  std::vector<std::vector<std::pair<int32_t, int64_t>>> segss(U);
  for (size_t i = 0; i < U; ++i) global.Reconstruct(uniq[i], &a0s[i], &segss[i]);
  std::vector<size_t> order(U);
  for (size_t i = 0; i < U; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
    if (a0s[x] != a0s[y]) return a0s[x] < a0s[y];
    return segss[x] < segss[y];
  });

  std::unordered_map<int32_t, int64_t> wid;
  wid.reserve(U);
  out->word_a0.resize(U);
  out->word_segs.resize(U);
  for (size_t r = 0; r < U; ++r) {
    size_t i = order[r];
    wid.emplace(uniq[i], static_cast<int64_t>(r));
    out->word_a0[r] = a0s[i];
    out->word_segs[r] = std::move(segss[i]);
  }

  out->node_word.reserve(sink_gid.size());
  for (const auto& [node, gid] : sink_gid)
    out->node_word.emplace_back(node, wid[gid]);
  std::sort(out->node_word.begin(), out->node_word.end());

  out->distinct_words = static_cast<int64_t>(U);
  out->node_word_pairs = static_cast<int64_t>(sink_gid.size());
  out->max_degree = maxdeg;
  return true;
}

}  // namespace primeparts::graph
