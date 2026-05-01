#include "primeparts/ui_iceberg.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "iceberg/arrow/arrow_file_io.h"
#include "iceberg/file_io.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/snapshot.h"
#include "iceberg/table_metadata.h"

namespace {

constexpr int32_t kPColumnFieldId = 1;

std::string strip_file_scheme(const std::string& uri) {
    if (uri.rfind("file://", 0) == 0) return uri.substr(7);
    return uri;
}

int64_t decode_int64_le(const std::vector<uint8_t>& bytes) {
    /* Iceberg lower/upper bounds for int columns are little-endian
     * fixed-width values. int32 → 4 bytes, int64 → 8 bytes. */
    int64_t v = 0;
    const size_t n = bytes.size() < 8 ? bytes.size() : 8;
    for (size_t i = 0; i < n; ++i) {
        v |= static_cast<int64_t>(bytes[i]) << (8 * i);
    }
    /* sign-extend if 4-byte */
    if (n == 4 && (bytes[3] & 0x80)) {
        v |= ~static_cast<int64_t>(0) << 32;
    }
    return v;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

char* dup_cstr(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

}  // namespace

struct pp_uic_handle {
    std::string warehouse_root;
    std::string primes_metadata_path;
    std::string decomp_metadata_path;
    std::shared_ptr<iceberg::FileIO> io;
    std::string last_error;

    /* Cached metadata; reloaded lazily so the TUI can refresh after a
     * commit without reopening the handle. */
    std::unique_ptr<iceberg::TableMetadata> primes_meta;
    std::unique_ptr<iceberg::TableMetadata> decomp_meta;

    const std::string* metadata_path_for(std::string_view table) const {
        if (table == "primes") return &primes_metadata_path;
        if (table == "decompositions") return &decomp_metadata_path;
        return nullptr;
    }

    iceberg::TableMetadata* metadata_for(std::string_view table) {
        const std::string* p = metadata_path_for(table);
        if (!p) {
            last_error = "unknown table: ";
            last_error.append(table);
            return nullptr;
        }
        std::unique_ptr<iceberg::TableMetadata>& slot =
            (table == "primes") ? primes_meta : decomp_meta;
        if (!slot) {
            auto r = iceberg::TableMetadataUtil::Read(*io, *p);
            if (!r.has_value()) {
                last_error = "TableMetadataUtil::Read(" + *p + "): " + r.error().message;
                return nullptr;
            }
            slot = std::move(r.value());
        }
        return slot.get();
    }
};

namespace {

/* Look up `metadata_location` for one (namespace, name) row. The
 * primeparts SqlCatalog does not have a stable catalog_name across
 * environments (production uses one name, --temp uses another), so we
 * only filter by namespace + name. */
bool sqlite_lookup_metadata_location(sqlite3* db,
                                     const char* table_name,
                                     std::string& out,
                                     std::string& err) {
    const char* sql =
        "SELECT metadata_location FROM iceberg_tables "
        "WHERE table_namespace = 'funbuns' AND table_name = ?1 "
        "LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = "sqlite3_prepare_v2: ";
        err += sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* loc = sqlite3_column_text(stmt, 0);
        if (loc) out = strip_file_scheme(reinterpret_cast<const char*>(loc));
        sqlite3_finalize(stmt);
        return !out.empty();
    }
    sqlite3_finalize(stmt);
    err = "no iceberg_tables row for funbuns.";
    err += table_name;
    return false;
}

}  // namespace

extern "C" {

pp_uic_handle* pp_uic_open(const char* warehouse_root) {
    if (!warehouse_root || !*warehouse_root) return nullptr;
    auto h = std::make_unique<pp_uic_handle>();
    h->warehouse_root = warehouse_root;

    std::string db_path = h->warehouse_root + "/catalog.db";
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }

    std::string err;
    bool ok =
        sqlite_lookup_metadata_location(db, "primes", h->primes_metadata_path, err) &&
        sqlite_lookup_metadata_location(db, "decompositions", h->decomp_metadata_path, err);
    sqlite3_close(db);
    if (!ok) {
        h->last_error = err;
        /* surface the error via a partially-initialized handle so callers
         * can inspect it before close. */
        return h.release();
    }

    auto unique_io = iceberg::arrow::MakeLocalFileIO();
    h->io = std::shared_ptr<iceberg::FileIO>(std::move(unique_io));
    return h.release();
}

void pp_uic_close(pp_uic_handle* h) { delete h; }

const char* pp_uic_last_error(const pp_uic_handle* h) {
    return h ? h->last_error.c_str() : "null handle";
}

void pp_uic_free_string(char* s) { std::free(s); }

int64_t pp_uic_max_p(pp_uic_handle* h, const char* table) {
    if (!h || !table) return -1;
    auto* meta = h->metadata_for(table);
    if (!meta) return -1;

    auto snap_r = meta->Snapshot();
    if (!snap_r.has_value()) {
        h->last_error = "current snapshot: " + snap_r.error().message;
        return -1;
    }
    auto schema_r = meta->Schema();
    if (!schema_r.has_value()) {
        h->last_error = "current schema: " + schema_r.error().message;
        return -1;
    }
    auto spec_r = meta->PartitionSpec();
    if (!spec_r.has_value()) {
        h->last_error = "current partition spec: " + spec_r.error().message;
        return -1;
    }

    auto list_r = iceberg::ManifestListReader::Make(snap_r.value()->manifest_list, h->io);
    if (!list_r.has_value()) {
        h->last_error = "ManifestListReader::Make: " + list_r.error().message;
        return -1;
    }
    auto files_r = list_r.value()->Files();
    if (!files_r.has_value()) {
        h->last_error = "ManifestListReader::Files: " + files_r.error().message;
        return -1;
    }

    int64_t best = std::numeric_limits<int64_t>::min();
    bool any = false;
    for (const auto& mf : files_r.value()) {
        auto reader_r = iceberg::ManifestReader::Make(
            mf, h->io, schema_r.value(), spec_r.value());
        if (!reader_r.has_value()) {
            h->last_error = "ManifestReader::Make: " + reader_r.error().message;
            return -1;
        }
        auto entries_r = reader_r.value()->LiveEntries();
        if (!entries_r.has_value()) {
            h->last_error = "ManifestReader::LiveEntries: " + entries_r.error().message;
            return -1;
        }
        for (const auto& entry : entries_r.value()) {
            if (!entry.data_file) continue;
            auto it = entry.data_file->upper_bounds.find(kPColumnFieldId);
            if (it == entry.data_file->upper_bounds.end()) continue;
            int64_t v = decode_int64_le(it->second);
            if (!any || v > best) {
                best = v;
                any = true;
            }
        }
    }
    return any ? best : -1;
}

int64_t pp_uic_total_rows(pp_uic_handle* h, const char* table) {
    if (!h || !table) return -1;
    auto* meta = h->metadata_for(table);
    if (!meta) return -1;

    auto snap_r = meta->Snapshot();
    if (!snap_r.has_value()) {
        h->last_error = "current snapshot: " + snap_r.error().message;
        return -1;
    }
    auto list_r = iceberg::ManifestListReader::Make(snap_r.value()->manifest_list, h->io);
    if (!list_r.has_value()) {
        h->last_error = "ManifestListReader::Make: " + list_r.error().message;
        return -1;
    }
    auto files_r = list_r.value()->Files();
    if (!files_r.has_value()) {
        h->last_error = "ManifestListReader::Files: " + files_r.error().message;
        return -1;
    }
    int64_t total = 0;
    for (const auto& mf : files_r.value()) {
        total += mf.added_rows_count.value_or(0);
        total += mf.existing_rows_count.value_or(0);
        total -= mf.deleted_rows_count.value_or(0);
    }
    return total;
}

char* pp_uic_list_snapshots_json(pp_uic_handle* h, const char* table) {
    if (!h || !table) return nullptr;
    auto* meta = h->metadata_for(table);
    if (!meta) return nullptr;

    /* Sort by sequence_number for stable display order. */
    std::vector<const iceberg::Snapshot*> ordered;
    ordered.reserve(meta->snapshots.size());
    for (const auto& sp : meta->snapshots) {
        if (sp) ordered.push_back(sp.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const iceberg::Snapshot* a, const iceberg::Snapshot* b) {
                  return a->sequence_number < b->sequence_number;
              });

    std::ostringstream os;
    os << "{\"current_snapshot_id\":" << meta->current_snapshot_id
       << ",\"snapshots\":[";
    bool first = true;
    for (const auto* sp : ordered) {
        if (!first) os << ',';
        first = false;
        const auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               sp->timestamp_ms.time_since_epoch()).count();
        os << "{\"snapshot_id\":" << sp->snapshot_id
           << ",\"parent_id\":";
        if (sp->parent_snapshot_id.has_value()) os << *sp->parent_snapshot_id;
        else os << "null";
        os << ",\"sequence_number\":" << sp->sequence_number
           << ",\"timestamp_ms\":" << ts_ms;
        auto op = sp->Operation();
        os << ",\"operation\":";
        if (op.has_value()) os << '"' << json_escape(*op) << '"';
        else os << "null";
        os << ",\"manifest_list\":\"" << json_escape(sp->manifest_list) << '"'
           << ",\"summary\":{";
        bool first_sum = true;
        for (const auto& [k, v] : sp->summary) {
            if (!first_sum) os << ',';
            first_sum = false;
            os << '"' << json_escape(k) << "\":\"" << json_escape(v) << '"';
        }
        os << "}}";
    }
    os << "]}";
    return dup_cstr(os.str());
}

}  /* extern "C" */
