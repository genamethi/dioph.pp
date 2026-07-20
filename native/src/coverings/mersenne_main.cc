// primeparts-mersenne — build the primeparts.mersenne_factors catalog table.
//
// One-time sidecar: factors each Mersenne number M_d = 2^d - 1 over a working
// d-range and publishes (d, prime, exponent, ord2, is_primitive) as a real,
// catalog-registered Iceberg table. This is the single source of truth the
// downstream cover/congruence work joins the index-differences against (the d's
// decoded from mdiff's hit_mask via HitMaskDiffs).
//
// Primitivity is precomputed and stored: prime is a *primitive* factor of M_d
// iff ord_2(prime) == d (i.e. d is the least index where prime divides 2^d-1),
// so consumers never rescan earlier d-rows to decide it.
//
// Output goes only through the catalog seam: ppc::DropTable(purge) to replace,
// CommitFiles to publish. The warehouse filesystem is never touched directly.

#include "primeparts/writer.h"
#include "primeparts/schemas.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/coverings/primitive_factors.h"

#include <arrow/api.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
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
  int32_t max_d = 40;  // working d-range; every 2^d-1 < 2^64 for d <= 63
  std::string rest_uri;  // empty => OpenCatalog resolves env or kDefaultRestUri
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--warehouse DIR] [--max-d N] [--rest-uri URI]\n\n"
               "Builds primeparts.mersenne_factors: full factorization of each\n"
               "2^d-1 for d in [1, N] with ord2 + is_primitive, published through\n"
               "the catalog (replace = DropTable purge + CommitFiles).\n\n"
               "  --rest-uri URI   IRC endpoint override (default %s); commits go\n"
               "                   through pp-catalogd, falling back to the local\n"
               "                   LMDB catalog only if it is unreachable.\n",
               argv0, ppc::kDefaultRestUri);
}

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* n) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s requires a value\n", n); return nullptr; }
      return argv[++i];
    };
    if (a == "--warehouse") {
      const char* v = val("--warehouse"); if (!v) return false; o->warehouse = v;
    } else if (a == "--max-d") {
      const char* v = val("--max-d");
      char* end = nullptr; long d = v ? std::strtol(v, &end, 10) : 0;
      if (!v || *end != '\0' || d < 1 || d > 63) {
        std::fprintf(stderr, "invalid --max-d (1..63): %s\n", v ? v : ""); return false;
      }
      o->max_d = static_cast<int32_t>(d);
    } else if (a == "--rest-uri") {
      const char* v = val("--rest-uri"); if (!v) return false; o->rest_uri = v;
    } else if (a == "--help" || a == "-h") {
      Usage(argv[0]); std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false;
    }
  }
  return true;
}

template <typename Builder>
std::shared_ptr<arrow::Array> Finish(Builder* b) {
  std::shared_ptr<arrow::Array> out;
  auto st = b->Finish(&out);
  if (!st.ok()) throw std::runtime_error(st.ToString());
  return out;
}

bool Build(const std::shared_ptr<iceberg::Catalog>& catalog, const Options& opts,
           std::string* error) {
  auto helper = primeparts::BuildMersenneHelper(opts.max_d);
  auto schema = primeparts::MersenneFactorsSchema();
  auto spec = iceberg::PartitionSpec::Unpartitioned();

  arrow::Int32Builder d_b, e_b, ord2_b, prim_b;
  arrow::Int64Builder q_b;
  int64_t primitive_count = 0;
  for (const auto& f : helper.factors) {
    const int32_t ord2 = helper.GetOrd2(f.q);
    const int32_t is_primitive = (ord2 == f.d) ? 1 : 0;
    primitive_count += is_primitive;
    if (!d_b.Append(f.d).ok() || !q_b.Append(static_cast<int64_t>(f.q)).ok() ||
        !e_b.Append(f.exponent).ok() || !ord2_b.Append(ord2).ok() ||
        !prim_b.Append(is_primitive).ok()) {
      *error = "mersenne: append failed"; return false;
    }
  }
  auto batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("d", arrow::int32()),
                     arrow::field("prime", arrow::int64()),
                     arrow::field("exponent", arrow::int32()),
                     arrow::field("ord2", arrow::int32()),
                     arrow::field("is_primitive", arrow::int32())}),
      static_cast<int64_t>(helper.factors.size()),
      {Finish(&d_b), Finish(&q_b), Finish(&e_b), Finish(&ord2_b), Finish(&prim_b)});

  // Replace through the catalog seam (the only file-removal path).
  if (!ppc::DropTable(catalog, opts.warehouse, "mersenne_factors", /*purge=*/true,
                      error))
    return false;

  primeparts::WriterConfig cfg;
  // Staging dir outside the warehouse; CommitFiles moves into place (seam).
  cfg.output_dir = ppc::StagingDataDir(opts.warehouse, "mersenne_factors");
  cfg.schema = schema;
  cfg.table_name = "mersenne_factors";
  cfg.filename_prefix = "mersenne_factors";
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
  if (!ppc::CommitFiles(catalog, opts.warehouse, "mersenne_factors", schema, spec,
                        data_files, &meta, error))
    return false;
  std::fprintf(stderr,
               "mersenne_factors: %zu rows (d<=%d, %" PRId64 " primitive) -> %s\n",
               helper.factors.size(), opts.max_d, primitive_count, meta.c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) { Usage(argv[0]); return 2; }

  std::string err;
  // REST-default via PRIMEPARTS_REST_URI; transparent local LMDB fallback.
  auto catalog = ppc::OpenCatalog(opts.warehouse, opts.rest_uri, nullptr, &err);
  if (!catalog) { std::fprintf(stderr, "OpenCatalog: %s\n", err.c_str()); return 1; }

  if (!Build(catalog, opts, &err)) {
    std::fprintf(stderr, "build mersenne_factors: %s\n", err.c_str()); return 1;
  }
  return 0;
}
