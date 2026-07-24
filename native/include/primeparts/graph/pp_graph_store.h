#pragma once

#include <ginac/ginac.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace primeparts::graph {

struct Edge {
  int64_t p = 0;
  int64_t q = 0;
  int32_t m = 0;
  int32_t n = 0;
};

struct SegKey {
  int32_t parent;
  int32_t n;
  int64_t c;
  bool operator==(const SegKey& o) const {
    return parent == o.parent && n == o.n && c == o.c;
  }
};
struct SegKeyHash {
  size_t operator()(const SegKey& k) const {
    size_t h = std::hash<int32_t>{}(k.parent);
    h ^= std::hash<int32_t>{}(k.n) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(k.c) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};
struct WordKey {
  int64_t a0;
  int32_t tail;
  bool operator==(const WordKey& o) const {
    return a0 == o.a0 && tail == o.tail;
  }
};
struct WordKeyHash {
  size_t operator()(const WordKey& k) const {
    size_t h = std::hash<int64_t>{}(k.a0);
    h ^= std::hash<int32_t>{}(k.tail) + 0x9e3779b97f4a7c15ULL + (h << 6) +
         (h >> 2);
    return h;
  }
};

struct WordStore {
  std::vector<int32_t> seg_parent;
  std::vector<int32_t> seg_n;
  std::vector<int64_t> seg_c;
  std::vector<int64_t> seg_deg;
  std::unordered_map<SegKey, int32_t, SegKeyHash> seg_ix;

  std::vector<int64_t> word_a0;
  std::vector<int32_t> word_tail;
  std::unordered_map<WordKey, int32_t, WordKeyHash> word_ix;
  int32_t seed = 0;

  WordStore() { seed = InternWord(0, -1); }

  int32_t InternSeg(int32_t parent, int32_t n, int64_t c) {
    SegKey k{parent, n, c};
    auto it = seg_ix.find(k);
    if (it != seg_ix.end()) return it->second;
    int32_t id = static_cast<int32_t>(seg_parent.size());
    seg_parent.push_back(parent);
    seg_n.push_back(n);
    seg_c.push_back(c);
    seg_deg.push_back((parent < 0 ? 1 : seg_deg[parent]) * n);
    seg_ix.emplace(k, id);
    return id;
  }
  int32_t InternWord(int64_t a0, int32_t tail) {
    WordKey k{a0, tail};
    auto it = word_ix.find(k);
    if (it != word_ix.end()) return it->second;
    int32_t id = static_cast<int32_t>(word_a0.size());
    word_a0.push_back(a0);
    word_tail.push_back(tail);
    word_ix.emplace(k, id);
    return id;
  }
  int32_t Push(int32_t w, int32_t n, int64_t c) {
    int64_t a0 = word_a0[w];
    int32_t tail = word_tail[w];
    if (n == 1) {
      if (tail < 0) return InternWord(a0 + c, -1);
      return InternWord(a0,
                        InternSeg(seg_parent[tail], seg_n[tail], seg_c[tail] + c));
    }
    return InternWord(a0, InternSeg(tail, n, c));
  }
  int64_t Degree(int32_t w) const {
    int32_t tail = word_tail[w];
    return tail < 0 ? 1 : seg_deg[tail];
  }
  void Reconstruct(int32_t w, int64_t* a0,
                   std::vector<std::pair<int32_t, int64_t>>* segs) const {
    *a0 = word_a0[w];
    segs->clear();
    for (int32_t s = word_tail[w]; s >= 0; s = seg_parent[s])
      segs->emplace_back(seg_n[s], seg_c[s]);
    std::reverse(segs->begin(), segs->end());
  }
};

struct ConcStore {
  static constexpr int SH = 256;
  struct SegShard {
    std::mutex mu;
    std::unordered_map<SegKey, int32_t, SegKeyHash> ix;
    std::vector<int32_t> parent, n;
    std::vector<int64_t> c, deg;
  };
  struct WordShard {
    std::mutex mu;
    std::unordered_map<WordKey, int32_t, WordKeyHash> ix;
    std::vector<int64_t> a0, deg;
    std::vector<int32_t> tail;
  };
  SegShard sseg[SH];
  WordShard sword[SH];
  int32_t seed;

  ConcStore() { seed = InternWord(0, -1, 1); }

  int32_t InternSeg(int32_t parent, int32_t n, int64_t c, int64_t deg) {
    SegKey k{parent, n, c};
    int s = SegKeyHash{}(k) & (SH - 1);
    auto& sh = sseg[s];
    std::lock_guard<std::mutex> g(sh.mu);
    auto it = sh.ix.find(k);
    if (it != sh.ix.end()) return it->second;
    int32_t local = static_cast<int32_t>(sh.parent.size());
    sh.parent.push_back(parent);
    sh.n.push_back(n);
    sh.c.push_back(c);
    sh.deg.push_back(deg);
    int32_t gid = (s << 23) | local;
    sh.ix.emplace(k, gid);
    return gid;
  }
  void ReadSeg(int32_t gid, int32_t* parent, int32_t* n, int64_t* c) {
    auto& sh = sseg[gid >> 23];
    int32_t local = gid & 0x7FFFFF;
    std::lock_guard<std::mutex> g(sh.mu);
    *parent = sh.parent[local];
    *n = sh.n[local];
    *c = sh.c[local];
  }
  int32_t InternWord(int64_t a0, int32_t tail, int64_t deg) {
    WordKey k{a0, tail};
    int s = WordKeyHash{}(k) & (SH - 1);
    auto& sh = sword[s];
    std::lock_guard<std::mutex> g(sh.mu);
    auto it = sh.ix.find(k);
    if (it != sh.ix.end()) return it->second;
    int32_t local = static_cast<int32_t>(sh.a0.size());
    sh.a0.push_back(a0);
    sh.tail.push_back(tail);
    sh.deg.push_back(deg);
    int32_t gid = (s << 23) | local;
    sh.ix.emplace(k, gid);
    return gid;
  }
  void ReadWord(int32_t gid, int64_t* a0, int32_t* tail, int64_t* deg) {
    auto& sh = sword[gid >> 23];
    int32_t local = gid & 0x7FFFFF;
    std::lock_guard<std::mutex> g(sh.mu);
    *a0 = sh.a0[local];
    *tail = sh.tail[local];
    *deg = sh.deg[local];
  }
  int32_t Push(int32_t w, int32_t n, int64_t c) {
    int64_t a0, deg;
    int32_t tail;
    ReadWord(w, &a0, &tail, &deg);
    if (n == 1) {
      if (tail < 0) return InternWord(a0 + c, -1, deg);
      int32_t tp, tn;
      int64_t tc;
      ReadSeg(tail, &tp, &tn, &tc);
      return InternWord(a0, InternSeg(tp, tn, tc + c, deg), deg);
    }
    return InternWord(a0, InternSeg(tail, n, c, deg * n), deg * n);
  }
  int64_t Degree(int32_t w) {
    int64_t a0, deg;
    int32_t tail;
    ReadWord(w, &a0, &tail, &deg);
    return deg;
  }
  void Reconstruct(int32_t w, int64_t* a0,
                   std::vector<std::pair<int32_t, int64_t>>* segs) {
    int64_t deg;
    int32_t tail;
    ReadWord(w, a0, &tail, &deg);
    segs->clear();
    for (int32_t s = tail; s >= 0;) {
      int32_t p, n;
      int64_t c;
      ReadSeg(s, &p, &n, &c);
      segs->emplace_back(n, c);
      s = p;
    }
    std::reverse(segs->begin(), segs->end());
  }
};

struct ParResult {
  int64_t node_word_pairs = 0;
  int64_t distinct_words = 0;
  int64_t max_degree = 0;
};

struct MemoEntry {
  int32_t word;
  int64_t conn;
  bool operator==(const MemoEntry& o) const {
    return word == o.word && conn == o.conn;
  }
};
struct MemoEntryHash {
  size_t operator()(const MemoEntry& e) const {
    size_t h = std::hash<int32_t>{}(e.word);
    h ^= std::hash<int64_t>{}(e.conn) + 0x9e3779b97f4a7c15ULL + (h << 6) +
         (h >> 2);
    return h;
  }
};

template <class Store>
int32_t Compose(Store& s, int32_t base, int32_t upper) {
  int64_t a0;
  std::vector<std::pair<int32_t, int64_t>> segs;
  s.Reconstruct(upper, &a0, &segs);
  int32_t t = base;
  if (a0 != 0) t = s.Push(t, 1, a0);
  for (const auto& [n, c] : segs) t = s.Push(t, n, c);
  return t;
}

template <class Store>
void Expand(Store& s,
            const std::unordered_map<int64_t, std::vector<MemoEntry>>& memo,
            int32_t word, int64_t conn, std::unordered_set<int32_t>& out) {
  if (conn < 0) {
    out.insert(word);
    return;
  }
  auto it = memo.find(conn);
  if (it == memo.end()) return;
  for (const auto& e : it->second)
    Expand(s, memo, Compose(s, e.word, word), e.conn, out);
}

GiNaC::ex He(int n, const GiNaC::symbol& x);
std::map<int, GiNaC::ex> ToHermite(GiNaC::ex P, const GiNaC::symbol& x);
std::string JsonInts(const std::vector<int64_t>& v);

struct Features {
  int64_t degree = 1;
  std::string skeleton;
  std::string translations;
  std::string hermite;
  std::string symbolic;
};

Features WordFeatures(int64_t a0,
                      const std::vector<std::pair<int32_t, int64_t>>& segs,
                      const GiNaC::symbol& x);

struct LoadedStore {
  std::vector<int32_t> seg_parent;
  std::vector<int32_t> seg_n;
  std::vector<int64_t> seg_c;
  std::vector<int64_t> word_a0;
  std::vector<int32_t> word_tail;
  int32_t seed = 0;

  void Reconstruct(int32_t w, int64_t* a0,
                   std::vector<std::pair<int32_t, int64_t>>* segs) const {
    *a0 = word_a0[w];
    segs->clear();
    for (int32_t s = word_tail[w]; s >= 0; s = seg_parent[s])
      segs->emplace_back(seg_n[s], seg_c[s]);
    std::reverse(segs->begin(), segs->end());
  }
};

struct SliceData {
  LoadedStore store;
  std::unordered_map<int64_t, std::vector<MemoEntry>> memo;
};

std::string SliceLogPath(const std::string& dir, int64_t slice);

struct SweepResult {
  int64_t num_slices = 0;
  int64_t memo_entries = 0;
  int64_t words = 0;
  int64_t segs = 0;
  int64_t connections = 0;
  int64_t spill_bytes = 0;
  int64_t nodes = 0;
  std::unordered_map<int64_t, int64_t> last_ref_slice;
  std::unordered_set<int64_t> has_children;
};

bool SweepAndSpill(const std::vector<int64_t>& nodes,
                   const std::unordered_map<int64_t, std::vector<Edge>>& parents,
                   int64_t slice_width, const std::string& dir,
                   SweepResult* out, std::string* error);

class SliceSweeper {
 public:
  SliceSweeper(std::string dir, int64_t slice_width);
  ~SliceSweeper();
  bool AddNode(int64_t p, bool is_root, const std::vector<Edge>& edges,
               std::string* error);
  bool Finish(std::string* error);
  const SweepResult& result() const { return result_; }

 private:
  bool FlushSlice(std::string* error);

  std::string dir_;
  int64_t slice_width_;
  int64_t cur_slice_ = -1;
  int64_t last_p_ = -1;
  std::unique_ptr<ConcStore> store_;
  std::unordered_map<int64_t, std::vector<MemoEntry>> memo_;
  SweepResult result_;
};

bool SpillSlice(const std::string& dir, int64_t slice, ConcStore& store,
                const std::unordered_map<int64_t, std::vector<MemoEntry>>& memo,
                std::string* error);

bool LoadSlice(const std::string& dir, int64_t slice, SliceData* out,
               std::string* error);

int32_t ComposeOnto(ConcStore& tgt, const LoadedStore& src, int32_t base,
                    int32_t upper);

ParResult ReassembleFromDisk(
    const std::string& dir, int64_t num_slices,
    const std::unordered_set<int64_t>& has_children,
    const std::unordered_map<int64_t, int64_t>& last_ref_slice,
    std::string* error);

struct SkeletonDict {
  std::map<std::vector<int32_t>, int32_t> ix;
  std::vector<std::vector<int32_t>> tuples;
  int32_t Intern(const std::vector<int32_t>& sk);
};

struct CoproductColumns {
  std::vector<int64_t> word_id;
  std::vector<int64_t> a0;
  std::vector<int32_t> skeleton_id;
  std::vector<int64_t> c[4];
  std::vector<int64_t> word_slice;
  std::vector<int64_t> nw_node;
  std::vector<int64_t> nw_word;
  std::vector<int64_t> nw_conn;
};

inline constexpr int kMaxSegments = 4;
inline constexpr int kWordSliceShift = 40;

bool BuildCoproductSlice(const std::string& dir, int64_t slice,
                         SkeletonDict* dict, CoproductColumns* out,
                         std::string* error);

struct ReassembleOutput {
  std::vector<int64_t> word_a0;
  std::vector<std::vector<std::pair<int32_t, int64_t>>> word_segs;
  std::vector<std::pair<int64_t, int64_t>> node_word;
  int64_t distinct_words = 0;
  int64_t node_word_pairs = 0;
  int64_t max_degree = 0;
};

bool ReassembleForMaterialize(
    const std::string& dir, int64_t num_slices,
    const std::unordered_set<int64_t>& has_children,
    const std::unordered_map<int64_t, int64_t>& last_ref_slice, int workers,
    ReassembleOutput* out, std::string* error);

}  // namespace primeparts::graph
