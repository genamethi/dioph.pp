// cluster — clustering & arithmetic-progression analysis over the mtuple tables.
//
// Reads primeparts.mtuple_k{K} (built by `tally`) through the catalog seam
// (SourceTableReader) and writes a scannable markdown summary. Each table yields
// two views, given the same treatment:
//   * unshifted  — the rows as stored (shape + shift = the original tuple);
//   * shifted    — grouped by the base-shifted shape (sum count over shift), the
//                  translation-invariant / same-modulus view.
// Per view/k: position histogram, top representatives, exact-AP flag, shared
// modulus (gcd of gaps), near-AP deviation, and a uniform-null residual. Across
// k: which common differences (moduli) recur, and which span the whole k range.

#include "primeparts/analysis/tuples.h"
#include "primeparts/source_scan.h"
#include "primeparts/catalog/pp_iceberg_rest.h"

#include <arrow/api.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;
namespace ana = primeparts::analysis;

namespace {

const fs::path kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
const char* kDefaultOut = "markdown/data_eng/mtuple_stats.md";
const char* kDefaultCsv = "markdown/data_eng/shift_freq.csv";
constexpr int kTopN = 12;  // rows shown per ranked table

struct Options {
  fs::path warehouse = kDefaultWarehouse;
  std::string out = kDefaultOut;
  std::string csv = kDefaultCsv;
  std::string rest_uri;
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--warehouse DIR] [--out FILE] [--csv FILE] [--rest-uri URI]\n\n"
               "Reads primeparts.mtuple_k{K} through the catalog and writes a\n"
               "markdown summary (default %s) plus a shift-frequency CSV (one row\n"
               "per k, columns 0..maxshift; default %s). Run from the repo root.\n",
               argv0, kDefaultOut, kDefaultCsv);
}

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* n) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s requires a value\n", n); return nullptr; }
      return argv[++i];
    };
    if (a == "--warehouse") { const char* v = val("--warehouse"); if (!v) return false; o->warehouse = v; }
    else if (a == "--out") { const char* v = val("--out"); if (!v) return false; o->out = v; }
    else if (a == "--csv") { const char* v = val("--csv"); if (!v) return false; o->csv = v; }
    else if (a == "--rest-uri") { const char* v = val("--rest-uri"); if (!v) return false; o->rest_uri = v; }
    else if (a == "--help" || a == "-h") { Usage(argv[0]); std::exit(0); }
    else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
  }
  return true;
}

struct Row { uint64_t mask; int64_t count; };

// Read a whole mtuple table through the catalog. Returns false + *error only on
// a real failure; sets *present=false (no error) when the table is absent.
bool LoadTable(const std::shared_ptr<iceberg::Catalog>& catalog, int k,
               std::vector<Row>* out, bool* present, std::string* error) {
  const std::string table = "mtuple_k" + std::to_string(k);
  std::string e;
  fs::path meta = ppc::TableMetadataPath(catalog, table, &e);
  if (meta.empty()) { *present = false; return true; }  // not built for this k
  *present = true;

  std::vector<std::string> cols;
  cols.reserve(ana::kTupleWidth + 2);
  for (int j = 1; j <= ana::kTupleWidth; ++j) cols.push_back("b" + std::to_string(j));
  cols.push_back("shift");
  cols.push_back("count");

  auto reader = primeparts::SourceTableReader::OpenMetadata(meta, cols, nullptr, error);
  if (!reader) return false;
  std::shared_ptr<arrow::RecordBatch> batch;
  int32_t bits[ana::kTupleWidth];
  while (true) {
    if (!reader->Next(&batch, error)) return false;
    if (!batch) break;
    std::vector<std::shared_ptr<arrow::Int32Array>> bcol(ana::kTupleWidth);
    for (int j = 0; j < ana::kTupleWidth; ++j)
      bcol[j] = std::static_pointer_cast<arrow::Int32Array>(
          batch->GetColumnByName("b" + std::to_string(j + 1)));
    auto shift_a = std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("shift"));
    auto count_a = std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("count"));
    const int64_t n = batch->num_rows();
    for (int64_t i = 0; i < n; ++i) {
      for (int j = 0; j < ana::kTupleWidth; ++j) bits[j] = bcol[j]->Value(i);
      out->push_back({ana::ColumnsToMask(bits, shift_a->Value(i)), count_a->Value(i)});
    }
  }
  return true;
}

std::string PositionsStr(const std::vector<int>& m) {
  std::string s;
  for (size_t i = 0; i < m.size(); ++i) {
    if (i) s += ',';
    s += std::to_string(m[i]);
  }
  return s;
}

double Pct(int64_t part, int64_t whole) {
  return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

// A ranked-representative table (used for both the unshifted and shifted views).
// `marg[pos]` is the single-position marginal frequency for the z_indep null.
void WriteRepTable(std::ofstream& o, const std::vector<Row>& reps, int64_t total,
                   bool shifted, const std::vector<double>& marg) {
  o << "\n| " << (shifted ? "shape (from 0)" : "tuple (m)")
    << " | count | share% | exact_ap | mod(gcd) | near_dev | z_unif | z_indep |\n";
  o << "|---|---:|---:|:--:|---:|---:|---:|---:|\n";
  const int64_t n_distinct = static_cast<int64_t>(reps.size());
  const int shown = std::min<int>(kTopN, static_cast<int>(reps.size()));
  for (int i = 0; i < shown; ++i) {
    auto pos = ana::MaskToPositions(reps[i].mask);
    if (shifted) { const int b = pos.front(); for (int& x : pos) x -= b; }
    char zu[32], zi[32], pb[16];
    std::snprintf(zu, sizeof(zu), "%.2f",
                  ana::UniformResidual(reps[i].count, total, n_distinct));
    std::snprintf(zi, sizeof(zi), "%.2f",
                  ana::IndependenceResidual(reps[i].count, total, pos, marg));
    std::snprintf(pb, sizeof(pb), "%.3f", Pct(reps[i].count, total));
    o << "| " << PositionsStr(pos) << " | " << reps[i].count << " | " << pb
      << " | " << (ana::IsExactAP(pos) ? "Y" : "·") << " | " << ana::GcdDiffs(pos)
      << " | " << ana::NearAPDeviation(pos) << " | " << zu << " | " << zi << " |\n";
  }
  if (n_distinct > shown)
    o << "_(+" << (n_distinct - shown) << " more)_\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) { Usage(argv[0]); return 2; }

  std::string err;
  auto catalog = ppc::OpenCatalog(opts.warehouse, opts.rest_uri, nullptr, &err);
  if (!catalog) { std::fprintf(stderr, "OpenCatalog: %s\n", err.c_str()); return 1; }

  std::ofstream o(opts.out);
  if (!o) { std::fprintf(stderr, "cannot open %s\n", opts.out.c_str()); return 1; }

  o << "# m-tuple statistics\n\n";
  o << "Tuples of exponents m for primes p = 2^m + q^n, per k = #representations. "
       "Source: primeparts.mtuple_k{K}. Two views per k: **unshifted** (exact m) "
       "and **shifted** (base-normalized shape).\n";
  o << "\nSignificance: `z_unif` = residual vs a uniform 1/N null over distinct "
       "reps; `z_indep` = residual vs a position-independence null (expected = "
       "total × ∏ single-position frequencies), flagging co-occurrence beyond how "
       "common each m is on its own.\n";

  // Cross-k accumulators: common difference (gcd of gaps) -> per-k prime totals.
  std::map<int, std::map<int, int64_t>> mod_by_k;   // gcd -> k -> primes
  std::map<int, int64_t> primes_per_k;
  std::set<int> ks_present;
  // Shift-frequency CSV: k -> (0-based shift = m_min-1) -> primes.
  std::map<int, std::map<int, int64_t>> shift_by_k;
  int max_shift = 0;

  for (int k = ana::kMinK; k <= 40; ++k) {
    std::vector<Row> rows;
    bool present = false;
    if (!LoadTable(catalog, k, &rows, &present, &err)) {
      std::fprintf(stderr, "load mtuple_k%d: %s\n", k, err.c_str()); return 1;
    }
    if (!present || rows.empty()) continue;
    ks_present.insert(k);

    // Unshifted totals + cross-k modulus accumulation.
    int64_t total = 0, exact_primes = 0;
    for (const auto& r : rows) {
      total += r.count;
      auto pos = ana::MaskToPositions(r.mask);
      if (ana::IsExactAP(pos)) exact_primes += r.count;
      mod_by_k[ana::GcdDiffs(pos)][k] += r.count;
      const int sc = ana::ShiftOf(r.mask) - 1;  // 0-based (m_min-1); min m is 1
      shift_by_k[k][sc] += r.count;
      if (sc > max_shift) max_shift = sc;
    }
    primes_per_k[k] = total;

    // Shifted view: group by shape, sum counts.
    std::unordered_map<uint64_t, int64_t> shape_map;
    for (const auto& r : rows) shape_map[ana::ShapeOf(r.mask)] += r.count;
    std::vector<Row> shapes;
    shapes.reserve(shape_map.size());
    for (const auto& [s, c] : shape_map) shapes.push_back({s, c});

    // Position histogram (exponent m -> primes) from the unshifted tuples.
    std::map<int, int64_t> pos_hist;
    for (const auto& r : rows) {
      for (int m : ana::MaskToPositions(r.mask)) pos_hist[m] += r.count;
    }

    // rows already sorted desc by count from tally; sort shapes the same way.
    std::sort(shapes.begin(), shapes.end(),
              [](const Row& a, const Row& b) {
                if (a.count != b.count) return a.count > b.count;
                return a.mask < b.mask;
              });

    o << "\n## k = " << k << "\n";
    o << "primes: " << total << "\n";
    o << "distinct_tuples: " << rows.size() << "\n";
    o << "distinct_shapes: " << shapes.size() << "\n";
    char fb[16]; std::snprintf(fb, sizeof(fb), "%.2f", Pct(exact_primes, total));
    o << "exact_ap_primes: " << exact_primes << " (" << fb << "%)\n";

    o << "\nposition histogram (m: primes):\n\n";
    for (const auto& [m, c] : pos_hist) o << "- " << m << ": " << c << "\n";

    // Single-position marginals for the z_indep (position-independence) null:
    // unshifted indexed by exponent m, shifted by shape bit (offset from base).
    const double dt = static_cast<double>(total);
    std::vector<double> marg_unshift(ana::kTupleWidth + 2, 0.0);
    for (const auto& [m, c] : pos_hist)
      if (m < static_cast<int>(marg_unshift.size())) marg_unshift[m] = c / dt;
    std::vector<double> marg_shift(ana::kTupleWidth + 2, 0.0);
    for (const auto& s : shapes)
      for (int j : ana::MaskToPositions(s.mask))
        if (j < static_cast<int>(marg_shift.size())) marg_shift[j] += s.count / dt;

    o << "\n### unshifted — top tuples\n";
    WriteRepTable(o, rows, total, /*shifted=*/false, marg_unshift);
    o << "\n### shifted — top shapes\n";
    WriteRepTable(o, shapes, total, /*shifted=*/true, marg_shift);
  }

  // Cross-k: modulus (common difference) continuation. A modulus that carries
  // primes at every k in the present range is a family spanning the whole range.
  o << "\n## cross-k modulus continuation\n";
  o << "Rows: common difference g = gcd(consecutive gaps). Cell = primes at that "
       "k. A g present at every k is a family spanning the range.\n\n";
  o << "| mod g |";
  for (int k : ks_present) o << " k" << k << " |";
  o << " #k |\n|---:|";
  for (size_t i = 0; i < ks_present.size(); ++i) o << "---:|";
  o << "---:|\n";
  // Order moduli by breadth (how many k), then by total.
  std::vector<std::pair<int, int>> mod_order;  // (g, #k)
  for (const auto& [g, per_k] : mod_by_k)
    mod_order.emplace_back(g, static_cast<int>(per_k.size()));
  std::sort(mod_order.begin(), mod_order.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  const int mod_shown = std::min<int>(20, static_cast<int>(mod_order.size()));
  for (int i = 0; i < mod_shown; ++i) {
    const int g = mod_order[i].first;
    o << "| " << g << " |";
    for (int k : ks_present) {
      auto it = mod_by_k[g].find(k);
      if (it == mod_by_k[g].end()) o << " · |";
      else o << " " << it->second << " |";
    }
    o << " " << mod_order[i].second << " |\n";
  }

  o << "\nfull-range moduli (present at every k):\n\n";
  const int nk = static_cast<int>(ks_present.size());
  bool any_full = false;
  for (const auto& [g, per_k] : mod_by_k) {
    if (static_cast<int>(per_k.size()) == nk) {
      int64_t tot = 0; for (const auto& [k, c] : per_k) tot += c;
      o << "- g=" << g << ": " << tot << " primes across all " << nk << " k\n";
      any_full = true;
    }
  }
  if (!any_full) o << "- (none)\n";
  o.close();

  // Shift-frequency CSV: one row per k (ascending), columns 0..max_shift, cell =
  // primes with that k whose tuple has that 0-based shift (m_min - 1).
  std::ofstream cf(opts.csv);
  if (!cf) { std::fprintf(stderr, "cannot open %s\n", opts.csv.c_str()); return 1; }
  cf << "k";
  for (int s = 0; s <= max_shift; ++s) cf << "," << s;
  cf << "\n";
  for (int k : ks_present) {
    cf << k;
    const auto& row = shift_by_k[k];
    for (int s = 0; s <= max_shift; ++s) {
      auto it = row.find(s);
      cf << "," << (it != row.end() ? it->second : int64_t{0});
    }
    cf << "\n";
  }
  cf.close();

  std::fprintf(stderr, "wrote %s and %s (%d k tables)\n", opts.out.c_str(),
               opts.csv.c_str(), static_cast<int>(ks_present.size()));
  return 0;
}
