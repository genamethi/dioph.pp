// primeparts/catalog/pp_lmdb_store.cc
//
// LMDB-backed iceberg::sql::CatalogStore. See pp_lmdb_store.h for the public
// seam and the on-disk layout. This is the implementation of that contract,
// modeled on the upstream Sqlpp23CatalogStore reference semantics:
//
//   - one env per catalog (the catalog name is informational; it does NOT scope
//     keys, because the env is already dedicated to one catalog);
//   - `tables`  sub-DB: key = ns '\0' name   -> value = metadata_loc '\0' prev_loc
//   - `nsprops` sub-DB: key = ns '\0' key    -> value = <presence> [value bytes]
//   - a namespace "exists" iff >=1 nsprops row (SqlCatalog inserts a sentinel
//     "exists" property on create) OR it owns >=1 table; ListNamespaceNames
//     unions the distinct namespaces of both sub-DBs, exactly as the SQL store
//     unions iceberg_namespace_properties with the distinct table namespaces.
//
// Concurrency: every operation runs under one recursive mutex, and each LMDB
// transaction begins and ends within a single mutex-held critical section on
// one thread. That trivially satisfies LMDB's single-writer rule and its
// per-thread transaction binding without needing MDB_NOTLS. Read concurrency is
// sacrificed for simplicity; the catalog read volume is negligible. The active
// write transaction is stashed in active_txn_ so nested store calls issued by
// SqlCatalog inside RunInTransaction() reuse it rather than self-committing.

#include "primeparts/catalog/pp_lmdb_store.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "lmdb.h"

namespace primeparts::catalog {

namespace {

using iceberg::AlreadyExists;
using iceberg::InvalidArgument;
using iceberg::IOError;
using iceberg::Result;
using iceberg::Status;
using iceberg::sql::CatalogStore;
using iceberg::sql::NamespaceProperty;

// Value separator and namespace-property presence bytes. metadata locations and
// table/namespace names are file paths / identifiers and never contain a NUL,
// so '\0' is an unambiguous field separator.
constexpr char kSep = '\0';
constexpr char kAbsent = '\x00';
constexpr char kPresent = '\x01';

std::string_view ToView(const MDB_val& v) {
  return std::string_view(static_cast<const char*>(v.mv_data), v.mv_size);
}

MDB_val ToVal(std::string& s) {
  return MDB_val{.mv_size = s.size(), .mv_data = s.data()};
}

// key = a '\0' b
std::string JoinKey(std::string_view a, std::string_view b) {
  std::string k;
  k.reserve(a.size() + 1 + b.size());
  k.append(a);
  k.push_back(kSep);
  k.append(b);
  return k;
}

// Prefix that selects every key under namespace `ns`: "ns\0".
std::string NsPrefix(std::string_view ns) {
  std::string p(ns);
  p.push_back(kSep);
  return p;
}

// The leading field of a value/key, i.e. bytes up to the first separator.
std::string_view FirstField(std::string_view bytes) {
  return bytes.substr(0, bytes.find(kSep));
}

class LmdbCatalogStore final : public CatalogStore {
 public:
  LmdbCatalogStore(std::filesystem::path path, std::string catalog_name,
                   std::size_t map_size)
      : path_(std::move(path)),
        catalog_name_(std::move(catalog_name)),
        map_size_(map_size) {}

  ~LmdbCatalogStore() override {
    if (env_ != nullptr) {
      mdb_env_close(env_);
      env_ = nullptr;
    }
  }

  LmdbCatalogStore(const LmdbCatalogStore&) = delete;
  LmdbCatalogStore& operator=(const LmdbCatalogStore&) = delete;

  Status Initialize() override {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    if (ec) {
      return IOError("lmdb: create dir {}: {}", path_.string(), ec.message());
    }
    if (int rc = mdb_env_create(&env_); rc != MDB_SUCCESS) {
      env_ = nullptr;
      return IOError("lmdb: env_create: {}", mdb_strerror(rc));
    }
    if (int rc = mdb_env_set_maxdbs(env_, 8); rc != MDB_SUCCESS) {
      return IOError("lmdb: set_maxdbs: {}", mdb_strerror(rc));
    }
    if (int rc = mdb_env_set_mapsize(env_, map_size_); rc != MDB_SUCCESS) {
      return IOError("lmdb: set_mapsize: {}", mdb_strerror(rc));
    }
    if (int rc = mdb_env_open(env_, path_.c_str(), /*flags=*/0, /*mode=*/0644);
        rc != MDB_SUCCESS) {
      return IOError("lmdb: env_open {}: {}", path_.string(), mdb_strerror(rc));
    }

    MDB_txn* txn = nullptr;
    if (int rc = mdb_txn_begin(env_, nullptr, /*flags=*/0, &txn); rc != MDB_SUCCESS) {
      return IOError("lmdb: open-dbi txn: {}", mdb_strerror(rc));
    }
    if (int rc = mdb_dbi_open(txn, "tables", MDB_CREATE, &dbi_tables_);
        rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return IOError("lmdb: dbi_open tables: {}", mdb_strerror(rc));
    }
    if (int rc = mdb_dbi_open(txn, "nsprops", MDB_CREATE, &dbi_nsprops_);
        rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return IOError("lmdb: dbi_open nsprops: {}", mdb_strerror(rc));
    }
    if (int rc = mdb_txn_commit(txn); rc != MDB_SUCCESS) {
      return IOError("lmdb: open-dbi commit: {}", mdb_strerror(rc));
    }
    return {};
  }

  // --- Namespaces --------------------------------------------------------

  Result<std::vector<std::string>> ListNamespaceNames() override {
    return WithRead([&](MDB_txn* txn) -> Result<std::vector<std::string>> {
      std::set<std::string> names;
      for (MDB_dbi dbi : {dbi_nsprops_, dbi_tables_}) {
        MDB_cursor* cur = nullptr;
        if (int rc = mdb_cursor_open(txn, dbi, &cur); rc != MDB_SUCCESS) {
          return IOError("lmdb: cursor_open: {}", mdb_strerror(rc));
        }
        MDB_val k{};
        MDB_val v{};
        int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
        while (rc == MDB_SUCCESS) {
          names.emplace(FirstField(ToView(k)));
          rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
        }
        mdb_cursor_close(cur);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
          return IOError("lmdb: list namespaces: {}", mdb_strerror(rc));
        }
      }
      return std::vector<std::string>(names.begin(), names.end());
    });
  }

  Result<std::vector<NamespaceProperty>> GetNamespaceProperties(
      std::string_view ns) override {
    std::string prefix = NsPrefix(ns);
    return WithRead([&](MDB_txn* txn) -> Result<std::vector<NamespaceProperty>> {
      std::vector<NamespaceProperty> out;
      MDB_cursor* cur = nullptr;
      if (int rc = mdb_cursor_open(txn, dbi_nsprops_, &cur); rc != MDB_SUCCESS) {
        return IOError("lmdb: cursor_open: {}", mdb_strerror(rc));
      }
      MDB_val k = ToVal(prefix);
      MDB_val v{};
      int rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
      while (rc == MDB_SUCCESS) {
        std::string_view key = ToView(k);
        if (key.substr(0, prefix.size()) != std::string_view(prefix)) break;
        NamespaceProperty p;
        p.key = std::string(key.substr(prefix.size()));
        std::string_view val = ToView(v);
        if (!val.empty() && val.front() == kPresent) {
          p.value = std::string(val.substr(1));
        }  // val.front() == kAbsent -> value stays std::nullopt
        out.push_back(std::move(p));
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
      }
      mdb_cursor_close(cur);
      if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
        return IOError("lmdb: get namespace properties: {}", mdb_strerror(rc));
      }
      return out;
    });
  }

  Status InsertNamespaceProperty(std::string_view ns, std::string_view key,
                                 std::optional<std::string_view> value) override {
    std::string k = JoinKey(ns, key);
    std::string val;
    val.push_back(value.has_value() ? kPresent : kAbsent);
    if (value.has_value()) val.append(*value);
    return WithWrite([&](MDB_txn* txn) -> Status {
      MDB_val kk = ToVal(k);
      MDB_val vv = ToVal(val);
      int rc = mdb_put(txn, dbi_nsprops_, &kk, &vv, MDB_NOOVERWRITE);
      if (rc == MDB_KEYEXIST) {
        return AlreadyExists("namespace property already exists: {}/{}", ns, key);
      }
      if (rc != MDB_SUCCESS) {
        return IOError("lmdb: insert namespace property: {}", mdb_strerror(rc));
      }
      return {};
    });
  }

  Status DeleteNamespaceProperty(std::string_view ns, std::string_view key) override {
    std::string k = JoinKey(ns, key);
    return WithWrite([&](MDB_txn* txn) -> Status {
      MDB_val kk = ToVal(k);
      int rc = mdb_del(txn, dbi_nsprops_, &kk, nullptr);
      if (rc == MDB_SUCCESS || rc == MDB_NOTFOUND) return Status{};
      return IOError("lmdb: delete namespace property: {}", mdb_strerror(rc));
    });
  }

  Result<int64_t> DeleteNamespace(std::string_view ns) override {
    std::string prefix = NsPrefix(ns);
    return WithWrite([&](MDB_txn* txn) -> Result<int64_t> {
      std::vector<std::string> keys;
      MDB_cursor* cur = nullptr;
      if (int rc = mdb_cursor_open(txn, dbi_nsprops_, &cur); rc != MDB_SUCCESS) {
        return IOError("lmdb: cursor_open: {}", mdb_strerror(rc));
      }
      MDB_val k = ToVal(prefix);
      MDB_val v{};
      int rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
      while (rc == MDB_SUCCESS) {
        std::string_view key = ToView(k);
        if (key.substr(0, prefix.size()) != std::string_view(prefix)) break;
        keys.emplace_back(key);
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
      }
      mdb_cursor_close(cur);
      if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
        return IOError("lmdb: scan namespace: {}", mdb_strerror(rc));
      }
      for (std::string& key : keys) {
        MDB_val kk = ToVal(key);
        if (int drc = mdb_del(txn, dbi_nsprops_, &kk, nullptr);
            drc != MDB_SUCCESS && drc != MDB_NOTFOUND) {
          return IOError("lmdb: delete namespace row: {}", mdb_strerror(drc));
        }
      }
      return static_cast<int64_t>(keys.size());
    });
  }

  // --- Tables ------------------------------------------------------------

  Result<std::vector<std::string>> ListTableNames(std::string_view ns) override {
    std::string prefix = NsPrefix(ns);
    return WithRead([&](MDB_txn* txn) -> Result<std::vector<std::string>> {
      std::vector<std::string> names;
      MDB_cursor* cur = nullptr;
      if (int rc = mdb_cursor_open(txn, dbi_tables_, &cur); rc != MDB_SUCCESS) {
        return IOError("lmdb: cursor_open: {}", mdb_strerror(rc));
      }
      MDB_val k = ToVal(prefix);
      MDB_val v{};
      int rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
      while (rc == MDB_SUCCESS) {
        std::string_view key = ToView(k);
        if (key.substr(0, prefix.size()) != std::string_view(prefix)) break;
        names.emplace_back(key.substr(prefix.size()));
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
      }
      mdb_cursor_close(cur);
      if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
        return IOError("lmdb: list tables: {}", mdb_strerror(rc));
      }
      return names;
    });
  }

  Result<bool> TableExists(std::string_view ns, std::string_view name) override {
    std::string k = JoinKey(ns, name);
    return WithRead([&](MDB_txn* txn) -> Result<bool> {
      MDB_val kk = ToVal(k);
      MDB_val v{};
      int rc = mdb_get(txn, dbi_tables_, &kk, &v);
      if (rc == MDB_SUCCESS) return true;
      if (rc == MDB_NOTFOUND) return false;
      return IOError("lmdb: table exists: {}", mdb_strerror(rc));
    });
  }

  Result<std::optional<std::string>> GetTableMetadataLocation(
      std::string_view ns, std::string_view name) override {
    std::string k = JoinKey(ns, name);
    return WithRead([&](MDB_txn* txn) -> Result<std::optional<std::string>> {
      MDB_val kk = ToVal(k);
      MDB_val v{};
      int rc = mdb_get(txn, dbi_tables_, &kk, &v);
      if (rc == MDB_NOTFOUND) return std::optional<std::string>{};
      if (rc != MDB_SUCCESS) {
        return IOError("lmdb: get table metadata location: {}", mdb_strerror(rc));
      }
      return std::optional<std::string>(std::string(FirstField(ToView(v))));
    });
  }

  Status InsertTable(std::string_view ns, std::string_view name,
                     std::string_view metadata_location) override {
    std::string k = JoinKey(ns, name);
    // value = metadata_location '\0' previous(empty). Previous is NULL on insert;
    // it is write-only state (no store reader), so an empty previous is faithful.
    std::string val(metadata_location);
    val.push_back(kSep);
    return WithWrite([&](MDB_txn* txn) -> Status {
      MDB_val kk = ToVal(k);
      MDB_val vv = ToVal(val);
      int rc = mdb_put(txn, dbi_tables_, &kk, &vv, MDB_NOOVERWRITE);
      if (rc == MDB_KEYEXIST) {
        return AlreadyExists("table already exists: {}.{}", ns, name);
      }
      if (rc != MDB_SUCCESS) {
        return IOError("lmdb: insert table: {}", mdb_strerror(rc));
      }
      return {};
    });
  }

  Result<int64_t> UpdateTableMetadataLocation(
      std::string_view ns, std::string_view name, std::string_view new_location,
      std::string_view new_previous_location,
      std::string_view expected_current_location) override {
    std::string k = JoinKey(ns, name);
    std::string val(new_location);
    val.push_back(kSep);
    val.append(new_previous_location);
    return WithWrite([&](MDB_txn* txn) -> Result<int64_t> {
      MDB_val kk = ToVal(k);
      MDB_val cur{};
      int rc = mdb_get(txn, dbi_tables_, &kk, &cur);
      if (rc == MDB_NOTFOUND) return static_cast<int64_t>(0);
      if (rc != MDB_SUCCESS) {
        return IOError("lmdb: read for CAS: {}", mdb_strerror(rc));
      }
      // Optimistic compare: only swap when the stored metadata location still
      // equals the expected base.
      if (FirstField(ToView(cur)) != expected_current_location) {
        return static_cast<int64_t>(0);
      }
      MDB_val vv = ToVal(val);
      if (int prc = mdb_put(txn, dbi_tables_, &kk, &vv, /*flags=*/0);
          prc != MDB_SUCCESS) {
        return IOError("lmdb: write for CAS: {}", mdb_strerror(prc));
      }
      return static_cast<int64_t>(1);
    });
  }

  Result<int64_t> DeleteTable(std::string_view ns, std::string_view name) override {
    std::string k = JoinKey(ns, name);
    return WithWrite([&](MDB_txn* txn) -> Result<int64_t> {
      MDB_val kk = ToVal(k);
      int rc = mdb_del(txn, dbi_tables_, &kk, nullptr);
      if (rc == MDB_SUCCESS) return static_cast<int64_t>(1);
      if (rc == MDB_NOTFOUND) return static_cast<int64_t>(0);
      return IOError("lmdb: delete table: {}", mdb_strerror(rc));
    });
  }

  Result<int64_t> RenameTable(std::string_view from_ns, std::string_view from_name,
                              std::string_view to_ns,
                              std::string_view to_name) override {
    std::string from_key = JoinKey(from_ns, from_name);
    std::string to_key = JoinKey(to_ns, to_name);
    return WithWrite([&](MDB_txn* txn) -> Result<int64_t> {
      MDB_val fk = ToVal(from_key);
      MDB_val fv{};
      int rc = mdb_get(txn, dbi_tables_, &fk, &fv);
      if (rc == MDB_NOTFOUND) return static_cast<int64_t>(0);
      if (rc != MDB_SUCCESS) {
        return IOError("lmdb: rename read source: {}", mdb_strerror(rc));
      }
      // Copy the value out before mutating the DB: fv points into the mmap and
      // is invalidated by the put/del below.
      std::string moved(ToView(fv));

      MDB_val tk = ToVal(to_key);
      MDB_val tv = ToVal(moved);
      int prc = mdb_put(txn, dbi_tables_, &tk, &tv, MDB_NOOVERWRITE);
      if (prc == MDB_KEYEXIST) {
        return AlreadyExists("table already exists: {}.{}", to_ns, to_name);
      }
      if (prc != MDB_SUCCESS) {
        return IOError("lmdb: rename write target: {}", mdb_strerror(prc));
      }
      if (int drc = mdb_del(txn, dbi_tables_, &fk, nullptr); drc != MDB_SUCCESS) {
        return IOError("lmdb: rename delete source: {}", mdb_strerror(drc));
      }
      return static_cast<int64_t>(1);
    });
  }

  // Renames every table and namespace-property entry from `from_ns` to `to_ns`.
  // Returns the total number of entries moved.
  //
  // Fails with AlreadyExists if `to_ns` already contains any tables or
  // properties.  The operation is atomic: on any failure the catalog is
  // left in exactly its prior state.
  Result<int64_t> RenameNamespace(std::string_view from_ns,
                                  std::string_view to_ns) {
    // Identity rename is a no-op; avoids unnecessary transaction overhead
    // and sidesteps the (correct) "target already exists" check below.
    if (from_ns == to_ns) {
      return static_cast<int64_t>(0);
    }

    std::string from_prefix = NsPrefix(from_ns);
    std::string to_prefix = NsPrefix(to_ns);

    return WithWrite([&](MDB_txn* txn) -> Result<int64_t> {
      // ── Phase 1: Guard ──────────────────────────────────────────────
      // Reject the rename if the target namespace already owns *any* key
      // in either DBI.  This prevents silent merging of two namespaces.
      for (MDB_dbi dbi : {dbi_tables_, dbi_nsprops_}) {
        MDB_cursor* cur = nullptr;
        if (int rc = mdb_cursor_open(txn, dbi, &cur); rc != MDB_SUCCESS) {
          return IOError("lmdb: cursor_open: {}", mdb_strerror(rc));
        }
        MDB_val k = ToVal(to_prefix);
        MDB_val v{};
        int rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
        bool exists = false;
        if (rc == MDB_SUCCESS) {
          std::string_view key = ToView(k);
          if (key.size() >= to_prefix.size() &&
              key.substr(0, to_prefix.size()) == std::string_view(to_prefix)) {
            exists = true;
          }
        }
        mdb_cursor_close(cur);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
          return IOError("lmdb: check target namespace: {}", mdb_strerror(rc));
        }
        if (exists) {
          return AlreadyExists("target namespace already exists: {}", to_ns);
        }
      }

      // ── Phase 2: Collect ────────────────────────────────────────────
      // Snapshot every key/value belonging to `from_ns` in both DBIs.
      //
      // We materialise into a vector rather than mutating in-place during
      // cursor iteration because LMDB cursors can behave subtly when the
      // underlying B-tree pages are split by concurrent inserts within the
      // same transaction.  A two-pass collect-then-apply is safer and
      // keeps the logic easy to audit.
      struct Entry {
        std::string old_key;
        std::string new_key;
        std::string value;
        MDB_dbi dbi;
      };
      std::vector<Entry> entries;

      for (MDB_dbi dbi : {dbi_tables_, dbi_nsprops_}) {
        MDB_cursor* cur = nullptr;
        if (int rc = mdb_cursor_open(txn, dbi, &cur); rc != MDB_SUCCESS) {
          return IOError("lmdb: cursor_open: {}", mdb_strerror(rc));
        }
        MDB_val k = ToVal(from_prefix);
        MDB_val v{};
        int rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
        while (rc == MDB_SUCCESS) {
          std::string_view key = ToView(k);
          if (key.size() < from_prefix.size() ||
              key.substr(0, from_prefix.size()) !=
                  std::string_view(from_prefix)) {
            break;  // Past the source prefix — done with this DBI.
          }
          Entry e;
          e.old_key = std::string(key);
          // Replace only the namespace portion; the trailing name stays.
          e.new_key = to_prefix + std::string(key.substr(from_prefix.size()));
          e.value = std::string(ToView(v));
          e.dbi = dbi;
          entries.push_back(std::move(e));
          rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
        }
        mdb_cursor_close(cur);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
          return IOError("lmdb: scan source namespace: {}", mdb_strerror(rc));
        }
      }

      // ── Phase 3: Apply ──────────────────────────────────────────────
      // Insert all new keys first, then delete all old keys.
      //
      // Ordering inserts before deletes means that if an insert fails
      // (e.g. MDB_KEYEXIST from a concurrent writer that slipped in after
      // our Phase-1 check), the transaction is aborted before any source
      // data is removed.  The catalog retains the original namespace
      // intact.
      for (auto& entry : entries) {
        MDB_val nk = ToVal(entry.new_key);
        MDB_val nv = ToVal(entry.value);
        int rc = mdb_put(txn, entry.dbi, &nk, &nv, MDB_NOOVERWRITE);
        if (rc == MDB_KEYEXIST) {
          return AlreadyExists(
              "target key collision during namespace rename");
        }
        if (rc != MDB_SUCCESS) {
          return IOError("lmdb: namespace rename insert: {}",
                         mdb_strerror(rc));
        }
      }

      for (auto& entry : entries) {
        MDB_val ok = ToVal(entry.old_key);
        int rc = mdb_del(txn, entry.dbi, &ok, nullptr);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
          return IOError("lmdb: namespace rename delete: {}",
                         mdb_strerror(rc));
        }
      }

      return static_cast<int64_t>(entries.size());
    });
  }

  Status RunInTransaction(const std::function<Status()>& body) override {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (active_txn_ != nullptr) {
      return InvalidArgument("Nested catalog transactions are not supported");
    }
    MDB_txn* txn = nullptr;
    if (int rc = mdb_txn_begin(env_, nullptr, /*flags=*/0, &txn); rc != MDB_SUCCESS) {
      return IOError("lmdb: begin transaction: {}", mdb_strerror(rc));
    }
    active_txn_ = txn;
    Status status = [&]() -> Status {
      try {
        return body();
      } catch (const std::exception& e) {
        return IOError("lmdb: transaction body threw: {}", e.what());
      }
    }();
    active_txn_ = nullptr;

    if (status.has_value()) {
      if (int rc = mdb_txn_commit(txn); rc != MDB_SUCCESS) {
        return IOError("lmdb: commit transaction: {}", mdb_strerror(rc));
      }
    } else {
      mdb_txn_abort(txn);
    }
    return status;
  }

 private:
  // Run `body(txn)` against a write transaction. Standalone calls begin a
  // private write txn and commit on success / abort on failure; calls nested
  // inside RunInTransaction() reuse the active txn and leave its lifecycle to
  // the outer RunInTransaction().
  template <typename Body>
  auto WithWrite(Body&& body) -> decltype(body(std::declval<MDB_txn*>())) {
    using R = decltype(body(std::declval<MDB_txn*>()));
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (active_txn_ != nullptr) {
      return body(active_txn_);
    }
    MDB_txn* txn = nullptr;
    if (int rc = mdb_txn_begin(env_, nullptr, /*flags=*/0, &txn); rc != MDB_SUCCESS) {
      return IOError("lmdb: begin write txn: {}", mdb_strerror(rc));
    }
    R result = body(txn);
    if (result.has_value()) {
      if (int rc = mdb_txn_commit(txn); rc != MDB_SUCCESS) {
        return IOError("lmdb: commit write txn: {}", mdb_strerror(rc));
      }
    } else {
      mdb_txn_abort(txn);
    }
    return result;
  }

  // Run `body(txn)` against a read snapshot. Reuses the active write txn (which
  // sees its own uncommitted writes) when one is in flight.
  template <typename Body>
  auto WithRead(Body&& body) -> decltype(body(std::declval<MDB_txn*>())) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (active_txn_ != nullptr) {
      return body(active_txn_);
    }
    MDB_txn* txn = nullptr;
    if (int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn); rc != MDB_SUCCESS) {
      return IOError("lmdb: begin read txn: {}", mdb_strerror(rc));
    }
    auto result = body(txn);
    mdb_txn_abort(txn);  // read txns are aborted to release the reader slot
    return result;
  }

  std::filesystem::path path_;
  std::string catalog_name_;
  std::size_t map_size_;

  MDB_env* env_ = nullptr;
  MDB_dbi dbi_tables_ = 0;
  MDB_dbi dbi_nsprops_ = 0;

  std::recursive_mutex mutex_;
  MDB_txn* active_txn_ = nullptr;  // non-null only inside RunInTransaction()
};

}  // namespace

Result<std::shared_ptr<iceberg::sql::CatalogStore>> MakeLmdbCatalogStore(
    std::filesystem::path path, std::string catalog_name,
    std::size_t map_size_bytes) {
  return std::make_shared<LmdbCatalogStore>(std::move(path), std::move(catalog_name),
                                            map_size_bytes);
}

// Test-only seam. RenameNamespace is not part of the iceberg::sql::CatalogStore
// interface (it is a concrete-store extension), so the smoke driver — which only
// holds a CatalogStore base pointer — cannot reach it directly. This downcasts
// to the concrete store. Valid because MakeLmdbCatalogStore only ever returns an
// LmdbCatalogStore.
Result<int64_t> SmokeRenameNamespace(iceberg::sql::CatalogStore& store,
                                     std::string_view from_ns,
                                     std::string_view to_ns) {
  return static_cast<LmdbCatalogStore&>(store).RenameNamespace(from_ns, to_ns);
}

}  // namespace primeparts::catalog
