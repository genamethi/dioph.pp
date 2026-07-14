#include <httplib.h>

#include <cstdio>
#include <cstdlib>
#include <getopt.h>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iceberg/catalog.h"
#include "iceberg/json_serde_internal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/sort_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_requirement.h"
#include "iceberg/table_update.h"
#include "iceberg/transform.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/config.h"

namespace {

using json = nlohmann::json;

struct Options {
  std::string table;
  std::string field;
  std::string ns = "primeparts";
  std::string rest_uri;
  std::string warehouse;
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s --table T --field F [options]\n"
    "  Declare an ascending identity sort order on F as table T's default,\n"
    "  via catalogd's updateTable route (assert-table-uuid guarded).\n"
    "  --table T          table name (required)\n"
    "  --field F          sort column (required)\n"
    "  --ns NS            namespace (default primeparts)\n"
    "  --rest-uri URI     catalogd base (default config.lua / %s)\n"
    "  --warehouse DIR    warehouse root (default config.lua)\n",
    argv0, primeparts::catalog::kDefaultRestUri);
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  static struct option long_opts[] = {
      {"table", required_argument, nullptr, 't'},
      {"field", required_argument, nullptr, 'f'},
      {"ns", required_argument, nullptr, 'N'},
      {"rest-uri", required_argument, nullptr, 'r'},
      {"warehouse", required_argument, nullptr, 'w'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};
  int o;
  while ((o = getopt_long(argc, argv, "t:f:N:r:w:h", long_opts, nullptr)) != -1) {
    switch (o) {
      case 't': opts.table = optarg; break;
      case 'f': opts.field = optarg; break;
      case 'N': opts.ns = optarg; break;
      case 'r': opts.rest_uri = optarg; break;
      case 'w': opts.warehouse = optarg; break;
      case 'h': Usage(argv[0]); return 0;
      default: Usage(argv[0]); return 2;
    }
  }
  if (opts.table.empty() || opts.field.empty()) {
    Usage(argv[0]);
    return 2;
  }

  std::string cfg_err;
  auto cfg = primeparts::config::Load(&cfg_err);
  if (opts.rest_uri.empty()) {
    if (const char* env = std::getenv("PRIMEPARTS_REST_URI"); env && env[0])
      opts.rest_uri = env;
    else if (auto it = cfg.find("rest_uri"); it != cfg.end() && !it->second.empty())
      opts.rest_uri = it->second;
    else
      opts.rest_uri = primeparts::catalog::kDefaultRestUri;
  }
  if (opts.warehouse.empty()) {
    if (auto it = cfg.find("warehouse"); it != cfg.end()) opts.warehouse = it->second;
  }

  std::string mode, err;
  auto catalog = primeparts::catalog::OpenCatalog(opts.warehouse, opts.rest_uri,
                                                  &mode, &err);
  if (!catalog) {
    std::fprintf(stderr, "error: OpenCatalog: %s\n", err.c_str());
    return 1;
  }

  iceberg::TableIdentifier ident{.ns = iceberg::Namespace{{opts.ns}},
                                 .name = opts.table};
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    std::fprintf(stderr, "error: LoadTable(%s): %s\n", opts.table.c_str(),
                 loaded.error().message.c_str());
    return 1;
  }
  auto tbl = loaded.value();
  const auto& metadata = tbl->metadata();
  if (!metadata) {
    std::fprintf(stderr, "error: %s has no metadata\n", opts.table.c_str());
    return 1;
  }

  auto schema_r = metadata->Schema();
  if (!schema_r.has_value()) {
    std::fprintf(stderr, "error: schema: %s\n",
                 schema_r.error().message.c_str());
    return 1;
  }
  const auto& schema = schema_r.value();
  int32_t field_id = -1;
  for (const auto& f : schema->fields()) {
    if (f.name() == opts.field) {
      field_id = f.field_id();
      break;
    }
  }
  if (field_id < 0) {
    std::fprintf(stderr, "error: field '%s' not in %s schema\n",
                 opts.field.c_str(), opts.table.c_str());
    return 1;
  }

  auto current = metadata->SortOrder();
  if (current.has_value() && current.value() && current.value()->is_sorted()) {
    const auto fields = current.value()->fields();
    if (fields.size() == 1 && fields[0].source_id() == field_id &&
        fields[0].direction() == iceberg::SortDirection::kAscending) {
      std::printf("%s.%s: ascending sort on '%s' already declared (order %d)\n",
                  opts.ns.c_str(), opts.table.c_str(), opts.field.c_str(),
                  current.value()->order_id());
      return 0;
    }
    std::fprintf(stderr,
                 "error: %s already declares a different default sort order "
                 "(%s); refusing to replace\n",
                 opts.table.c_str(),
                 current.value()->ToString().c_str());
    return 1;
  }

  auto snap_r = tbl->current_snapshot();
  if (snap_r.has_value() && snap_r.value()) {
    auto spec_r = tbl->spec();
    if (!spec_r.has_value()) {
      std::fprintf(stderr, "error: spec: %s\n", spec_r.error().message.c_str());
      return 1;
    }
    iceberg::SnapshotCache cache(snap_r.value().get());
    auto manifests = cache.DataManifests(tbl->io());
    if (!manifests.has_value()) {
      std::fprintf(stderr, "error: manifests: %s\n",
                   manifests.error().message.c_str());
      return 1;
    }
    for (const auto& m : manifests.value()) {
      auto reader = iceberg::ManifestReader::Make(m, tbl->io(), schema,
                                                  spec_r.value());
      if (!reader.has_value()) {
        std::fprintf(stderr, "error: manifest: %s\n",
                     reader.error().message.c_str());
        return 1;
      }
      auto entries = reader.value()->LiveEntries();
      if (!entries.has_value()) {
        std::fprintf(stderr, "error: entries: %s\n",
                     entries.error().message.c_str());
        return 1;
      }
      for (const auto& entry : entries.value()) {
        if (!entry.data_file) continue;
        if (!entry.data_file->lower_bounds.contains(field_id)) {
          std::fprintf(stderr,
                       "error: refusing to declare: committed file %s carries "
                       "no manifest bounds for '%s'; a declared sort order "
                       "requires key bounds on every data file\n",
                       entry.data_file->file_path.c_str(), opts.field.c_str());
          return 1;
        }
      }
    }
  }

  auto order_r = iceberg::SortOrder::Make(
      *schema, iceberg::SortOrder::kInitialSortOrderId,
      {iceberg::SortField(field_id, iceberg::Transform::Identity(),
                          iceberg::SortDirection::kAscending,
                          iceberg::NullOrder::kFirst)});
  if (!order_r.has_value()) {
    std::fprintf(stderr, "error: SortOrder::Make: %s\n",
                 order_r.error().message.c_str());
    return 1;
  }
  std::shared_ptr<iceberg::SortOrder> order = std::move(order_r.value());

  iceberg::table::AssertUUID requirement(metadata->table_uuid);
  iceberg::table::AddSortOrder add_order(order);
  iceberg::table::SetDefaultSortOrder set_default(order->order_id());

  auto add_json = iceberg::ToJson(static_cast<const iceberg::TableUpdate&>(add_order));
  auto set_json = iceberg::ToJson(static_cast<const iceberg::TableUpdate&>(set_default));
  if (!add_json.has_value() || !set_json.has_value()) {
    std::fprintf(stderr, "error: serializing table updates failed\n");
    return 1;
  }
  json body;
  body["requirements"] = json::array();
  body["requirements"].push_back(iceberg::ToJson(
      static_cast<const iceberg::TableRequirement&>(requirement)));
  body["updates"] = json::array();
  body["updates"].push_back(std::move(add_json.value()));
  body["updates"].push_back(std::move(set_json.value()));

  httplib::Client cli(opts.rest_uri);
  cli.set_read_timeout(30, 0);
  const std::string path =
      "/v1/namespaces/" + opts.ns + "/tables/" + opts.table;
  auto res = cli.Post(path, body.dump(), "application/json");
  if (!res) {
    std::fprintf(stderr, "error: POST %s: no response from %s\n", path.c_str(),
                 opts.rest_uri.c_str());
    return 1;
  }
  if (res->status != 200) {
    std::fprintf(stderr, "error: POST %s -> %d: %s\n", path.c_str(),
                 res->status, res->body.c_str());
    return 1;
  }
  std::printf("%s.%s: declared ascending sort on '%s' (order %d)\n",
              opts.ns.c_str(), opts.table.c_str(), opts.field.c_str(),
              order->order_id());
  return 0;
}
