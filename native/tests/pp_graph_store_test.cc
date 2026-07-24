#include "primeparts/graph/pp_graph_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace primeparts::graph;

namespace {

struct Graph {
  std::vector<int64_t> nodes;
  std::unordered_map<int64_t, std::vector<Edge>> parents;
  std::unordered_set<int64_t> has_children;
};

Graph SampleGraph() {
  std::vector<Edge> edges = {
      {5, 2, 1, 2},  {7, 2, 2, 2},  {7, 3, 1, 2},  {11, 5, 1, 2},
      {13, 7, 1, 3}, {17, 11, 2, 2}, {17, 3, 1, 2}, {19, 13, 1, 2},
      {19, 5, 3, 2}, {23, 17, 1, 2}, {23, 19, 2, 3}};
  Graph g;
  std::unordered_set<int64_t> node_set;
  for (const auto& e : edges) {
    g.parents[e.p].push_back(e);
    g.has_children.insert(e.q);
    node_set.insert(e.p);
    node_set.insert(e.q);
  }
  g.nodes.assign(node_set.begin(), node_set.end());
  std::sort(g.nodes.begin(), g.nodes.end());
  return g;
}

struct Canon {
  std::vector<int64_t> a0;
  std::vector<std::vector<std::pair<int32_t, int64_t>>> segs;
  std::vector<std::pair<int64_t, int64_t>> node_word;
  int64_t max_degree = 0;
};

template <class Store>
Canon Canonicalize(Store& s,
                   const std::vector<std::pair<int64_t, int32_t>>& sink) {
  std::unordered_set<int32_t> distinct;
  int64_t maxdeg = 0;
  for (const auto& [node, w] : sink) {
    distinct.insert(w);
    int64_t d = s.Degree(w);
    if (d > maxdeg) maxdeg = d;
  }
  std::vector<int32_t> uniq(distinct.begin(), distinct.end());
  const size_t U = uniq.size();
  std::vector<int64_t> a0s(U);
  std::vector<std::vector<std::pair<int32_t, int64_t>>> segss(U);
  for (size_t i = 0; i < U; ++i) s.Reconstruct(uniq[i], &a0s[i], &segss[i]);
  std::vector<size_t> ord(U);
  for (size_t i = 0; i < U; ++i) ord[i] = i;
  std::sort(ord.begin(), ord.end(), [&](size_t x, size_t y) {
    if (a0s[x] != a0s[y]) return a0s[x] < a0s[y];
    return segss[x] < segss[y];
  });
  std::unordered_map<int32_t, int64_t> wid;
  Canon c;
  c.a0.resize(U);
  c.segs.resize(U);
  for (size_t r = 0; r < U; ++r) {
    size_t i = ord[r];
    wid.emplace(uniq[i], static_cast<int64_t>(r));
    c.a0[r] = a0s[i];
    c.segs[r] = segss[i];
  }
  for (const auto& [node, w] : sink) c.node_word.emplace_back(node, wid[w]);
  std::sort(c.node_word.begin(), c.node_word.end());
  c.max_degree = maxdeg;
  return c;
}

Canon ReferenceExpansion(const Graph& g) {
  WordStore ref;
  std::unordered_map<int64_t, std::vector<int32_t>> memo;
  std::vector<std::pair<int64_t, int32_t>> sink;
  for (int64_t v : g.nodes) {
    std::unordered_set<int32_t> res;
    auto it = g.parents.find(v);
    if (it == g.parents.end()) {
      res.insert(ref.seed);
    } else {
      for (const auto& e : it->second) {
        const int64_t c = int64_t{1} << e.m;
        for (int32_t w : memo[e.q]) res.insert(ref.Push(w, e.n, c));
      }
    }
    if (g.has_children.count(v)) {
      memo[v].assign(res.begin(), res.end());
    } else {
      for (int32_t w : res) {
        if (w == ref.seed) continue;
        sink.emplace_back(v, w);
      }
    }
  }
  return Canonicalize(ref, sink);
}

std::string ScratchDir(const std::string& tag) {
  fs::path d = fs::temp_directory_path() / ("ppgs_" + tag);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d.string();
}

}  // namespace

TEST(PpGraphStore, SlicedDiskReproducesSinglePassAcrossWidths) {
  const Graph g = SampleGraph();
  const Canon ref = ReferenceExpansion(g);
  ASSERT_GT(ref.a0.size(), 0u);
  ASSERT_GT(ref.node_word.size(), 0u);

  for (int64_t width : {1, 2, 3, 5, 8, 1000}) {
    const std::string dir = ScratchDir("width" + std::to_string(width));
    SweepResult sw;
    std::string err;
    ASSERT_TRUE(SweepAndSpill(g.nodes, g.parents, width, dir, &sw, &err)) << err;

    ReassembleOutput mo;
    ASSERT_TRUE(ReassembleForMaterialize(dir, sw.num_slices, g.has_children,
                                         sw.last_ref_slice, 4, &mo, &err))
        << err;

    EXPECT_EQ(mo.distinct_words, static_cast<int64_t>(ref.a0.size()))
        << "width=" << width;
    EXPECT_EQ(mo.node_word_pairs, static_cast<int64_t>(ref.node_word.size()))
        << "width=" << width;
    EXPECT_EQ(mo.max_degree, ref.max_degree) << "width=" << width;
    EXPECT_EQ(mo.word_a0, ref.a0) << "width=" << width;
    EXPECT_EQ(mo.word_segs, ref.segs) << "width=" << width;
    EXPECT_EQ(mo.node_word, ref.node_word) << "width=" << width;
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

TEST(PpGraphStore, SliceSweeperStreamsToSameResult) {
  const Graph g = SampleGraph();
  const Canon ref = ReferenceExpansion(g);

  for (int64_t width : {1, 2, 3, 5, 1000}) {
    const std::string dir = ScratchDir("sweeper" + std::to_string(width));
    SliceSweeper sweeper(dir, width);
    std::string err;
    for (int64_t p : g.nodes) {
      auto it = g.parents.find(p);
      const bool is_root = (it == g.parents.end());
      std::vector<Edge> edges = is_root ? std::vector<Edge>{} : it->second;
      ASSERT_TRUE(sweeper.AddNode(p, is_root, edges, &err)) << err;
    }
    ASSERT_TRUE(sweeper.Finish(&err)) << err;

    const SweepResult& sw = sweeper.result();
    ReassembleOutput mo;
    ASSERT_TRUE(ReassembleForMaterialize(dir, sw.num_slices, sw.has_children,
                                         sw.last_ref_slice, 4, &mo, &err))
        << err;
    EXPECT_EQ(mo.node_word, ref.node_word) << "width=" << width;
    EXPECT_EQ(mo.word_a0, ref.a0) << "width=" << width;
    EXPECT_EQ(mo.word_segs, ref.segs) << "width=" << width;
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

TEST(PpGraphStore, SpillRoundTripPreservesWordsAndMemo) {
  ConcStore store;
  int32_t w1 = store.Push(store.seed, 2, 4);
  int32_t w2 = store.Push(w1, 3, 8);
  int32_t w3 = store.Push(store.seed, 1, 7);
  std::unordered_map<int64_t, std::vector<MemoEntry>> memo;
  memo[100] = {{w1, -1}, {w2, 42}};
  memo[200] = {{w3, 7}};

  const std::string dir = ScratchDir("roundtrip");
  std::string err;
  ASSERT_TRUE(SpillSlice(dir, 0, store, memo, &err)) << err;

  SliceData sd;
  ASSERT_TRUE(LoadSlice(dir, 0, &sd, &err)) << err;
  ASSERT_EQ(sd.memo.size(), 2u);
  ASSERT_EQ(sd.memo.at(100).size(), 2u);
  ASSERT_EQ(sd.memo.at(200).size(), 1u);
  EXPECT_EQ(sd.memo.at(100)[1].conn, 42);
  EXPECT_EQ(sd.memo.at(200)[0].conn, 7);

  int64_t a0_src, a0_dst;
  std::vector<std::pair<int32_t, int64_t>> segs_src, segs_dst;
  store.Reconstruct(w2, &a0_src, &segs_src);
  sd.store.Reconstruct(sd.memo.at(100)[1].word, &a0_dst, &segs_dst);
  EXPECT_EQ(a0_src, a0_dst);
  EXPECT_EQ(segs_src, segs_dst);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(PpGraphStore, MissingSliceLogLoadsEmpty) {
  const std::string dir = ScratchDir("missing");
  SliceData sd;
  std::string err;
  EXPECT_TRUE(LoadSlice(dir, 99, &sd, &err)) << err;
  EXPECT_TRUE(sd.memo.empty());
}
