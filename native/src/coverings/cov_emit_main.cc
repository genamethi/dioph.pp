// primeparts-cov-emit — build the cap3 covering tables + write the research answer.
//
// Emits two catalog tables in the CoveringSchema (modulus, residue), both
// residue-0 (divisibility) covers over the difference x = p - 2^m:
//   covering_primary   — the certificate: every prime q <= --cert-bound. A k=0
//                        prime is fully covered because each x = p - 2^m is
//                        composite, hence has a prime factor <= sqrt(x), and
//                        sqrt(x) <= sqrt(p_max) = the certificate bound.
//   covering_bleed_min — the exponent-order-targeted cover: every prime l with
//                        ord_l(2) <= --order-cap. Few primes bleed into k>0, at
//                        the cost of much lower k=0 coverage.
// And writes `false` to research_answer.txt: the order-targeted cover has the
// LEAST coverage (order_cap << the certificate bound), not the best — so the
// claim "exponent-targeted delivers the best coverage" is false.
//
// Output goes only through the catalog seam (DropTable purge + CommitFiles),
// mirroring primeparts-mersenne. --dry-run/--csv-dir dump the coverings without
// touching the catalog (used to grade them with tests/grade_covering.cc).

#include "primeparts/writer.h"
#include "primeparts/schemas.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/coverings/primitive_factors.h"

#include <arrow/api.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/expression/literal.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;

namespace {

const fs::path kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";

struct Options {
  fs::path warehouse = kDefaultWarehouse;
  std::string rest_uri;
  int64_t cert_bound = 45145;  // floor(sqrt(max k=0 prime)); certificate q <= this
  int32_t order_cap = 63;      // covering_bleed_min: primes l with ord_2(l) <= this
  std::string answer_path = "/app/research_answer.txt";
  std::string csv_dir;         // if set, also dump both coverings as CSV
  bool dry_run = false;        // dump CSV + answer, skip the catalog
};

void Usage(const char* a0) {
  std::fprintf(stderr,
    "usage: %s [options]\n"
    "  Build the cap3 covering tables (CoveringSchema: modulus,residue) and write\n"
    "  the research answer, published through the catalog seam.\n"
    "  --warehouse DIR   warehouse root (default %s)\n"
    "  --rest-uri URI    IRC endpoint override (default: env / built-in)\n"
    "  --cert-bound N    covering_primary: all primes q <= N (default 45145)\n"
    "  --order-cap N     covering_bleed_min: primes l with ord_2(l) <= N (1..63, default 63)\n"
    "  --answer-path P   research answer file (default /app/research_answer.txt)\n"
    "  --csv-dir DIR     also write covering_primary.csv / covering_bleed_min.csv\n"
    "  --dry-run         write CSV + answer only; do not touch the catalog\n",
    a0, kDefaultWarehouse.c_str());
}

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* n) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s requires a value\n", n); return nullptr; }
      return argv[++i];
    };
    if (a == "--warehouse") { const char* v=val("--warehouse"); if(!v) return false; o->warehouse=v; }
    else if (a == "--rest-uri") { const char* v=val("--rest-uri"); if(!v) return false; o->rest_uri=v; }
    else if (a == "--cert-bound") { const char* v=val("--cert-bound"); if(!v) return false; o->cert_bound=std::strtoll(v,nullptr,10); }
    else if (a == "--order-cap") { const char* v=val("--order-cap"); if(!v) return false; o->order_cap=(int32_t)std::strtol(v,nullptr,10); }
    else if (a == "--answer-path") { const char* v=val("--answer-path"); if(!v) return false; o->answer_path=v; }
    else if (a == "--csv-dir") { const char* v=val("--csv-dir"); if(!v) return false; o->csv_dir=v; }
    else if (a == "--dry-run") { o->dry_run=true; }
    else if (a == "--help" || a == "-h") { Usage(argv[0]); std::exit(0); }
    else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
  }
  if (o->order_cap < 1 || o->order_cap > 63) {
    std::fprintf(stderr, "--order-cap must be in 1..63 (2^d-1 must fit in uint64)\n"); return false;
  }
  if (o->cert_bound < 2) { std::fprintf(stderr, "--cert-bound must be >= 2\n"); return false; }
  return true;
}

template <typename Builder>
std::shared_ptr<arrow::Array> Finish(Builder* b) {
  std::shared_ptr<arrow::Array> out;
  auto st = b->Finish(&out);
  if (!st.ok()) throw std::runtime_error(st.ToString());
  return out;
}

// All primes <= n (simple sieve; n is the certificate bound, ~45k).
std::vector<int64_t> PrimesUpTo(int64_t n) {
  std::vector<int64_t> primes;
  if (n < 2) return primes;
  std::vector<char> comp(n + 1, 0);
  for (int64_t i = 2; i * i <= n; ++i)
    if (!comp[i]) for (int64_t j = i * i; j <= n; j += i) comp[j] = 1;
  for (int64_t i = 2; i <= n; ++i) if (!comp[i]) primes.push_back(i);
  return primes;
}

// covering_primary: every prime q <= bound, class residue 0 (q | x).
std::vector<std::pair<int64_t,int64_t>> CertificateCover(int64_t bound) {
  std::vector<std::pair<int64_t,int64_t>> rows;
  for (int64_t q : PrimesUpTo(bound)) rows.push_back({q, 0});
  return rows;
}

// covering_bleed_min: every odd prime l with ord_2(l) <= order_cap, residue 0.
// BuildMersenneHelper(order_cap) fully factors 2^d-1 for d in [1,order_cap], so
// ord2_by_q holds exactly the primes whose order divides some d <= order_cap,
// i.e. ord <= order_cap (including large small-order primes a value-sieve misses).
std::vector<std::pair<int64_t,int64_t>> OrderCappedCover(int32_t order_cap) {
  auto helper = primeparts::BuildMersenneHelper(order_cap);
  std::vector<int64_t> mods;
  for (const auto& [q, d] : helper.ord2_by_q)
    if (d >= 1 && d <= order_cap && q >= 3) mods.push_back(static_cast<int64_t>(q));
  std::sort(mods.begin(), mods.end());
  mods.erase(std::unique(mods.begin(), mods.end()), mods.end());
  std::vector<std::pair<int64_t,int64_t>> rows;
  for (int64_t m : mods) rows.push_back({m, 0});
  return rows;
}

bool WriteCoveringTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                        const fs::path& warehouse, const std::string& name,
                        const std::vector<std::pair<int64_t,int64_t>>& rows,
                        std::string* error) {
  auto schema = primeparts::CoveringSchema();
  auto spec = iceberg::PartitionSpec::Unpartitioned();

  arrow::Int64Builder mod_b, res_b;
  for (const auto& [m, r] : rows)
    if (!mod_b.Append(m).ok() || !res_b.Append(r).ok()) { *error = name + ": append failed"; return false; }
  auto batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("modulus", arrow::int64()),
                     arrow::field("residue", arrow::int64())}),
      static_cast<int64_t>(rows.size()), {Finish(&mod_b), Finish(&res_b)});

  if (!ppc::DropTable(catalog, warehouse, name, /*purge=*/true, error)) return false;

  primeparts::WriterConfig cfg;
  cfg.output_dir = ppc::StagingDataDir(warehouse, name);
  cfg.schema = schema;
  cfg.table_name = name;
  cfg.filename_prefix = name;
  cfg.partition_spec = spec;
  cfg.partition_values =
      std::make_shared<iceberg::PartitionValues>(std::vector<iceberg::Literal>{});
  cfg.simple_filename = true;
  cfg.target_rows_per_file = 0;
  auto writer = primeparts::BucketParquetWriter::Make(cfg, error);
  if (!writer) return false;
  primeparts::BucketParquetWriter::BatchStats st{};
  if (!writer->Write(*batch, st, error)) return false;
  std::vector<primeparts::WrittenFile> written;
  if (!writer->Close(&written, error)) return false;
  std::vector<std::shared_ptr<iceberg::DataFile>> data_files;
  for (const auto& wf : written) if (wf.data_file) data_files.push_back(wf.data_file);

  std::string meta;
  if (!ppc::CommitFiles(catalog, warehouse, name, schema, spec, data_files, &meta, error))
    return false;
  std::fprintf(stderr, "%s: %zu congruences -> %s\n", name.c_str(), rows.size(), meta.c_str());
  return true;
}

void MaybeDumpCsv(const std::string& dir, const std::string& name,
                  const std::vector<std::pair<int64_t,int64_t>>& rows) {
  if (dir.empty()) return;
  fs::create_directories(dir);
  std::ofstream f(dir + "/" + name + ".csv");
  f << "modulus,residue\n";
  for (const auto& [m, r] : rows) f << m << "," << r << "\n";
}

bool WriteAnswer(const std::string& path, std::string* error) {
  std::ofstream a(path);
  if (!a) { *error = "cannot write " + path; return false; }
  a << "false\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) { Usage(argv[0]); return 2; }

  const auto primary = CertificateCover(opts.cert_bound);
  const auto bleed = OrderCappedCover(opts.order_cap);
  MaybeDumpCsv(opts.csv_dir, "covering_primary", primary);
  MaybeDumpCsv(opts.csv_dir, "covering_bleed_min", bleed);

  std::string err;
  if (!WriteAnswer(opts.answer_path, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  std::fprintf(stderr, "research_answer -> %s (false)\n", opts.answer_path.c_str());

  if (opts.dry_run) {
    std::fprintf(stderr, "dry-run: covering_primary=%zu, covering_bleed_min=%zu rows; catalog skipped\n",
                 primary.size(), bleed.size());
    return 0;
  }

  auto catalog = ppc::OpenCatalog(opts.warehouse, opts.rest_uri, nullptr, &err);
  if (!catalog) { std::fprintf(stderr, "OpenCatalog: %s\n", err.c_str()); return 1; }
  if (!WriteCoveringTable(catalog, opts.warehouse, "covering_primary", primary, &err)) {
    std::fprintf(stderr, "covering_primary: %s\n", err.c_str()); return 1;
  }
  if (!WriteCoveringTable(catalog, opts.warehouse, "covering_bleed_min", bleed, &err)) {
    std::fprintf(stderr, "covering_bleed_min: %s\n", err.c_str()); return 1;
  }
  return 0;
}
