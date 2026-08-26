#include <getopt.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <vector>

#include <arrow/record_batch.h>

#include "primeparts/client/session.h"
#include "primeparts/config.h"
#include "primeparts/parts_expand.h"
#include "primeparts/scan/column_binder.h"
#include "primeparts/scan/scan_plan.h"

#include "iceberg/expression/expression.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"

namespace {

namespace client = primeparts::client;
namespace scan = primeparts::scan;

struct Options {
  std::string config_path;
  int64_t p_hi = 0;
  int64_t report_every = 50000000;
  int scan_threads = 8;
  double max_gb = 20.0;
};

struct Interner {
  std::unordered_map<std::string, uint32_t> by_key;
  std::vector<uint32_t> singleton_root;
  uint64_t merges = 0;

  uint32_t Singleton(int64_t root) {
    const uint32_t id = static_cast<uint32_t>(singleton_root.size());
    singleton_root.push_back(static_cast<uint32_t>(root & 0xffffffff));
    return id;
  }

  uint32_t Union(std::vector<uint32_t>* ids) {
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    if (ids->size() == 1) return (*ids)[0];
    std::string key(reinterpret_cast<const char*>(ids->data()),
                    ids->size() * sizeof(uint32_t));
    const auto at = by_key.find(key);
    if (at != by_key.end()) return at->second;
    const uint32_t id =
        static_cast<uint32_t>(singleton_root.size() + by_key.size());
    by_key.emplace(std::move(key), id);
    ++merges;
    return id;
  }

  std::size_t Count() const { return singleton_root.size() + by_key.size(); }
};

struct Cursor {
  client::TableHandle handle;
  std::unique_ptr<client::ScanStream> stream;
  std::shared_ptr<arrow::RecordBatch> batch;
  std::vector<const int64_t*> cols;
  std::vector<const int32_t*> cols32;
  std::vector<std::string> names;
  int64_t at = 0;

  bool Open(client::Session& session, const std::string& table,
            std::vector<std::string> select,
            const std::shared_ptr<iceberg::Expression>& filter,
            std::string* error) {
    names = select;
    if (!session.LoadTable(table, &handle, error)) return false;
    scan::ScanPlanRequest req;
    req.select = std::move(select);
    req.filter = filter;
    stream = session.Scan(handle, req, error);
    return stream != nullptr && Advance(error);
  }

  bool Advance(std::string* error) {
    while (batch == nullptr || at >= batch->num_rows()) {
      if (!stream->Next(&batch, error)) return false;
      if (batch == nullptr) return true;
      at = 0;
      cols.assign(names.size(), nullptr);
      cols32.assign(names.size(), nullptr);
      for (std::size_t i = 0; i < names.size(); ++i) {
        std::string ignored;
        cols[i] = scan::BindInt64(*batch, names[i], &ignored);
        if (cols[i] != nullptr) continue;
        cols32[i] = scan::BindInt32(*batch, names[i], error);
        if (cols32[i] == nullptr) return false;
      }
    }
    return true;
  }

  bool Done() const { return batch == nullptr; }
  int64_t P() const { return Col(0); }
  int64_t Col(std::size_t i) const {
    return cols[i] != nullptr ? cols[i][at] : cols32[i][at];
  }
};

double Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string Gb(std::size_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1e9);
  return buf;
}

int Run(const Options& opt) {
  primeparts::config::Conf conf;
  std::string error;
  if (!primeparts::config::Load(opt.config_path, &conf, &error)) {
    std::fprintf(stderr, "config: %s\n", error.c_str());
    return 1;
  }

  client::SessionOptions so;
  so.rest_uri = conf.core.rest_uri;
  so.warehouse = conf.core.warehouse;
  so.ns = conf.core.ns_name;
  so.scan_threads = opt.scan_threads;
  auto session = client::Session::Open(so, &error);
  if (!session) {
    std::fprintf(stderr, "session: %s\n", error.c_str());
    return 1;
  }

  std::shared_ptr<iceberg::Expression> filter;
  if (opt.p_hi > 0) {
    filter = iceberg::Expressions::LessThanOrEqual(
        "p", iceberg::Literal::Long(opt.p_hi));
  }

  Cursor primes, flat, twist;
  if (!primes.Open(*session, "primes", {"p", "k"}, filter, &error) ||
      !flat.Open(*session, "flat_parts", {"p", "hit_mask"}, filter, &error) ||
      !twist.Open(*session, "higher_parts", {"p", "q_k"}, filter, &error)) {
    std::fprintf(stderr, "open: %s\n", error.c_str());
    return 1;
  }
  std::fprintf(stderr, "planned primes=%lld flat=%lld twist=%lld\n",
               static_cast<long long>(primes.stream->planned_rows()),
               static_cast<long long>(flat.stream->planned_rows()),
               static_cast<long long>(twist.stream->planned_rows()));

  std::unordered_map<int64_t, uint32_t> set_of;
  Interner interner;
  std::vector<uint32_t> parents;
  int64_t rows = 0, roots = 0, missing = 0, orphan = 0, last_p = 0;
  const double t0 = Now();
  double next_report = opt.report_every;

  while (!primes.Done()) {
    const int64_t p = primes.P();
    const int64_t k = primes.Col(1);
    if (p < last_p) {
      std::fprintf(stderr, "primes not ascending (%lld after %lld)\n",
                   static_cast<long long>(p), static_cast<long long>(last_p));
      return 1;
    }
    last_p = p;

    parents.clear();
    while (!flat.Done() && flat.P() < p) {
      ++flat.at;
      if (!flat.Advance(&error)) return 1;
    }
    if (!flat.Done() && flat.P() == p) {
      primeparts::ForEachPart(p, static_cast<uint64_t>(flat.Col(1)),
                              [&](int32_t, int64_t q) {
                                const auto at = set_of.find(q);
                                if (at == set_of.end()) {
                                  ++missing;
                                  return;
                                }
                                parents.push_back(at->second);
                              });
      ++flat.at;
      if (!flat.Advance(&error)) return 1;
    }
    while (!twist.Done() && twist.P() < p) {
      ++twist.at;
      if (!twist.Advance(&error)) return 1;
    }
    while (!twist.Done() && twist.P() == p) {
      const auto at = set_of.find(twist.Col(1));
      if (at == set_of.end()) ++missing;
      else parents.push_back(at->second);
      ++twist.at;
      if (!twist.Advance(&error)) return 1;
    }

    if (parents.empty()) {
      set_of.emplace(p, interner.Singleton(p));
      ++roots;
      if (k != 0) ++orphan;
    } else {
      set_of.emplace(p, interner.Union(&parents));
    }

    const std::size_t live = set_of.size() * 40 + interner.Count() * 16;
    if (static_cast<double>(live) > opt.max_gb * 1e9) {
      std::fprintf(stderr,
                   "STOP at the %.0f GB budget: p=%lld rows=%lld roots=%lld "
                   "sets=%zu sets/rows=%.4f  %.0fs\n",
                   opt.max_gb, static_cast<long long>(p),
                   static_cast<long long>(rows), static_cast<long long>(roots),
                   interner.Count(),
                   static_cast<double>(interner.Count()) / rows, Now() - t0);
      return 0;
    }
    if (++rows >= next_report) {
      next_report += opt.report_every;
      const std::size_t bytes = live;
      std::fprintf(stderr,
                   "p=%lld rows=%lld roots=%lld sets=%zu sets/rows=%.4f "
                   "missing=%lld orphan=%lld ~%s %.0fs\n",
                   static_cast<long long>(p), static_cast<long long>(rows),
                   static_cast<long long>(roots), interner.Count(),
                   static_cast<double>(interner.Count()) / rows,
                   static_cast<long long>(missing),
                   static_cast<long long>(orphan), Gb(bytes).c_str(),
                   Now() - t0);
    }
    ++primes.at;
    if (!primes.Advance(&error)) return 1;
  }
  if (!error.empty()) {
    std::fprintf(stderr, "scan ended: %s\n", error.c_str());
    return 1;
  }
  std::fprintf(stderr,
               "done: rows=%lld roots=%lld sets=%zu missing=%lld orphan=%lld "
               "%.0fs\n",
               static_cast<long long>(rows), static_cast<long long>(roots),
               interner.Count(), static_cast<long long>(missing),
               static_cast<long long>(orphan), Now() - t0);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  static const option kLong[] = {
      {"config", required_argument, nullptr, 'c'},
      {"p-hi", required_argument, nullptr, 'H'},
      {"report-every", required_argument, nullptr, 'r'},
      {"threads", required_argument, nullptr, 't'},
      {"max-gb", required_argument, nullptr, 'g'},
      {nullptr, 0, nullptr, 0}};
  for (int c; (c = getopt_long(argc, argv, "c:H:r:t:g:", kLong, nullptr)) != -1;) {
    switch (c) {
      case 'c': opt.config_path = optarg; break;
      case 'H': opt.p_hi = std::atoll(optarg); break;
      case 'r': opt.report_every = std::atoll(optarg); break;
      case 't': opt.scan_threads = std::atoi(optarg); break;
      case 'g': opt.max_gb = std::atof(optarg); break;
      default:
        std::fprintf(stderr,
                     "usage: %s [-c config] [--p-hi N] [--report-every N] "
                     "[--threads N]\n",
                     argv[0]);
        return 2;
    }
  }
  return Run(opt);
}
