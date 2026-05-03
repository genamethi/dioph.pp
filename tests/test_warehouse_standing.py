from types import SimpleNamespace

import polars as pl

import primeparts.iceberg_schema as schema


class FakeCatalog:
    def __init__(self, table_map):
        self._table_map = table_map

    def load_table(self, ident):
        return self._table_map[ident]


def test_warehouse_standing_accepts_compacted_p_trunc_layout(monkeypatch):
    primes = SimpleNamespace(name="primes")
    decomps = SimpleNamespace(name="decompositions")
    cat = FakeCatalog(
        {
            schema.PRIMES_IDENT: primes,
            schema.DECOMP_IDENT: decomps,
        }
    )

    compacted_frame = pl.DataFrame(
        {
            "file_path": ["/warehouse/funbuns/primes/data/p_trunc=0/primes_compact_000000.parquet"],
            "commit_seq": [None],
            "p_min": [3],
            "p_max": [9_999_999_967],
            "partition_dir": ["/warehouse/funbuns/primes/data/p_trunc=0"],
            "partition_name": ["p_trunc=0"],
            "partition_layout": ["p_trunc"],
        },
        schema={
            "file_path": pl.Utf8,
            "commit_seq": pl.Int64,
            "p_min": pl.Int64,
            "p_max": pl.Int64,
            "partition_dir": pl.Utf8,
            "partition_name": pl.Utf8,
            "partition_layout": pl.Utf8,
        },
    )

    manifest_bounds = {
        id(primes): (None, 564_575_405_239),
        id(decomps): (None, 564_575_405_239),
    }
    summary_bounds = {
        id(primes): (23_679, 564_575_405_239),
        id(decomps): (23_679, 564_575_405_239),
    }

    monkeypatch.setattr(schema, "table_manifest_bounds", lambda tbl: manifest_bounds[id(tbl)])
    monkeypatch.setattr(schema, "_table_summary_bounds", lambda tbl: summary_bounds[id(tbl)])
    monkeypatch.setattr(schema, "table_referenced_data_files", lambda tbl: set())
    monkeypatch.setattr(schema, "_referenced_file_frame", lambda tbl: compacted_frame)
    monkeypatch.setattr(schema, "table_local_data_files", lambda tbl: set())

    state = schema.warehouse_standing(cat)

    assert state["ok"] is True
    assert state["errors"] == []
    assert state["tables"]["primes"]["partition_layout_counts"] == {"p_trunc": 1}
    assert state["tables"]["primes"]["has_manifest_commit_seq_metrics"] is False


def test_warehouse_standing_flags_legacy_commit_seq_reuse(monkeypatch):
    primes = SimpleNamespace(name="primes")
    decomps = SimpleNamespace(name="decompositions")
    cat = FakeCatalog(
        {
            schema.PRIMES_IDENT: primes,
            schema.DECOMP_IDENT: decomps,
        }
    )

    legacy_frame = pl.DataFrame(
        {
            "file_path": [
                "/warehouse/funbuns/primes/data/commit_seq=7/primes_b000007_000.parquet",
                "/warehouse/funbuns/primes/data/commit_seq=8/primes_b000008_000.parquet",
            ],
            "commit_seq": [7, 7],
            "p_min": [3, 101],
            "p_max": [97, 197],
            "partition_dir": [
                "/warehouse/funbuns/primes/data/commit_seq=7",
                "/warehouse/funbuns/primes/data/commit_seq=8",
            ],
            "partition_name": ["commit_seq=7", "commit_seq=8"],
            "partition_layout": ["commit_seq", "commit_seq"],
        }
    )
    empty_decomp_frame = legacy_frame.clear()

    manifest_bounds = {
        id(primes): (7, 197),
        id(decomps): (7, 197),
    }
    summary_bounds = {
        id(primes): (7, 197),
        id(decomps): (7, 197),
    }

    monkeypatch.setattr(schema, "table_manifest_bounds", lambda tbl: manifest_bounds[id(tbl)])
    monkeypatch.setattr(schema, "_table_summary_bounds", lambda tbl: summary_bounds[id(tbl)])
    monkeypatch.setattr(schema, "table_referenced_data_files", lambda tbl: set())
    monkeypatch.setattr(
        schema,
        "_referenced_file_frame",
        lambda tbl: legacy_frame if tbl is primes else empty_decomp_frame,
    )
    monkeypatch.setattr(schema, "table_local_data_files", lambda tbl: set())

    state = schema.warehouse_standing(cat)

    assert state["ok"] is False
    assert any("reuses commit_seq across multiple data directories" in err for err in state["errors"])
