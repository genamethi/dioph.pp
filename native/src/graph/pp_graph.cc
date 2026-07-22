#include <duckdb.hpp>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table_scan.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/client/session.h"
#include "primeparts/scan/scan_plan.h"

namespace client = primeparts::client;
namespace ppc = primeparts::catalog;
namespace scan = primeparts::scan;

namespace {

constexpr const char* kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
constexpr const char* kDefaultRestUri = "http://127.0.0.1:8181";

struct Options {
  int64_t bound = 5000000000LL;
  int threads = 8;
  std::string rest_uri;
  std::string warehouse = kDefaultWarehouse;
};

double Seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

std::string StripFileScheme(const std::string& path) {
  if (path.rfind("file://", 0) == 0) return path.substr(7);
  if (path.rfind("file:", 0) == 0) return path.substr(5);
  return path;
}

std::string QuoteList(const std::vector<std::string>& paths) {
  std::string out = "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) out += ", ";
    out += "'";
    for (char c : StripFileScheme(paths[i])) {
      if (c == '\'') out += "''";
      else out += c;
    }
    out += "'";
  }
  out += "]";
  return out;
}

bool ParseArgs(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--bound") {
      out->bound = static_cast<int64_t>(std::strtod(need("--bound"), nullptr));
    } else if (a == "--rest-uri") {
      out->rest_uri = need("--rest-uri");
    } else if (a == "--warehouse") {
      out->warehouse = need("--warehouse");
    } else if (a == "--threads") {
      out->threads = std::atoi(need("--threads"));
    } else {
      std::fprintf(stderr,
                   "usage: pp-graph [--bound N] [--threads N] "
                   "[--rest-uri URI] [--warehouse DIR]\n");
      return false;
    }
  }
  if (out->rest_uri.empty()) {
    const char* env = std::getenv("PRIMEPARTS_REST_URI");
    out->rest_uri = env ? env : kDefaultRestUri;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;

  std::string error;
  client::SessionOptions session_options;
  session_options.rest_uri = opt.rest_uri;
  session_options.warehouse = opt.warehouse;
  session_options.scan_threads = opt.threads;
  auto session = client::Session::Open(session_options, &error);
  if (!session) {
    std::fprintf(stderr, "Session::Open: %s\n", error.c_str());
    return 1;
  }

  client::TableHandle partitions;
  if (!session->LoadTable("partitions", &partitions, &error)) {
    std::fprintf(stderr, "LoadTable: %s\n", error.c_str());
    return 1;
  }

  scan::ScanPlanRequest request;
  request.select = {"p", "m_k", "n_k", "q_k"};
  request.filter = iceberg::Expressions::LessThanOrEqual(
      "p", iceberg::Literal::Long(opt.bound));

  const auto ns = ppc::ResolveNamespace("");
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (!ppc::PlanScanOnServer(opt.rest_uri, ns, "partitions", request,
                             *partitions.metadata(), ppc::PlanPollOptions{},
                             &tasks, &error)) {
    std::fprintf(stderr, "PlanScanOnServer: %s\n", error.c_str());
    return 1;
  }
  double t_plan = Seconds(t0);

  std::vector<std::string> paths;
  paths.reserve(tasks.size());
  for (const auto& task : tasks) paths.push_back(task->data_file()->file_path);
  if (paths.empty()) {
    std::printf("[wiring] no data files planned at B=%" PRId64 "\n", opt.bound);
    return 0;
  }

  duckdb::DBConfig config;
  config.SetOptionByName("threads", duckdb::Value::BIGINT(opt.threads));
  duckdb::DuckDB db(nullptr, &config);
  duckdb::Connection con(db);

  std::string sql =
      "SELECT count(*) AS edges, min(p) AS minp, max(p) AS maxp, "
      "max(n_k) AS max_n, count(*) FILTER (n_k = 1) AS n1 "
      "FROM read_parquet(" +
      QuoteList(paths) +
      ") WHERE p <= " + std::to_string(opt.bound);

  t0 = std::chrono::steady_clock::now();
  auto result = con.Query(sql);
  if (result->HasError()) {
    std::fprintf(stderr, "duckdb: %s\n", result->GetError().c_str());
    return 1;
  }
  double t_read = Seconds(t0);

  std::printf("[wiring] files=%zu plan=%.2fs read=%.2fs (in-process duckdb)\n",
              paths.size(), t_plan, t_read);
  std::printf(
      "[census] edges=%s minp=%s maxp=%s max_n=%s n1=%s\n",
      result->GetValue(0, 0).ToString().c_str(),
      result->GetValue(1, 0).ToString().c_str(),
      result->GetValue(2, 0).ToString().c_str(),
      result->GetValue(3, 0).ToString().c_str(),
      result->GetValue(4, 0).ToString().c_str());
  return 0;
}
