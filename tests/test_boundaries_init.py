import importlib.util
from pathlib import Path

import pyarrow as pa


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "init_boundaries_table.py"


def load_script():
    spec = importlib.util.spec_from_file_location("init_boundaries_table", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeCatalog:
    def __init__(self):
        self.namespaces = []
        self.created = []

    def create_namespace_if_not_exists(self, namespace):
        self.namespaces.append(namespace)

    def create_table_if_not_exists(self, **kwargs):
        self.created.append(kwargs)
        return FakeTable()


class FakeTable:
    def __init__(self):
        self.appended = []

    def append(self, table, snapshot_properties=None):
        self.appended.append((table, snapshot_properties or {}))


def test_parse_boundary_rows_assigns_v1_buckets(tmp_path):
    module = load_script()
    tsv = tmp_path / "boundary_primes.tsv"
    tsv.write_text("2\t3\n3855446407\t93357498629\n", encoding="ascii")

    rows = module.parse_boundary_rows(tsv)

    assert rows == [
        {"p_bucket_version": 1, "p_bucket": 0, "p_min": 3},
        {"p_bucket_version": 1, "p_bucket": 1, "p_min": 93357498629},
    ]


def test_parse_boundary_rows_rejects_non_monotone_primes(tmp_path):
    module = load_script()
    tsv = tmp_path / "boundary_primes.tsv"
    tsv.write_text("2\t3\n3\t3\n", encoding="ascii")

    try:
        module.parse_boundary_rows(tsv)
    except ValueError as exc:
        assert "strictly increasing" in str(exc)
    else:
        raise AssertionError("expected monotonicity failure")


def test_ensure_boundaries_table_creates_unpartitioned_schema_and_appends_rows():
    module = load_script()
    cat = FakeCatalog()
    rows = [
        {"p_bucket_version": 1, "p_bucket": 0, "p_min": 3},
        {"p_bucket_version": 1, "p_bucket": 1, "p_min": 93357498629},
    ]

    table, appended = module.write_boundaries(cat, rows)

    assert appended is True
    assert cat.namespaces == ["funbuns"]
    created = cat.created[0]
    assert created["identifier"] == "funbuns.boundaries"
    assert [field.name for field in created["schema"].fields] == [
        "p_bucket_version",
        "p_bucket",
        "p_min",
    ]
    assert len(created["partition_spec"].fields) == 0
    assert created["properties"]["write.format.default"] == "parquet"
    assert created["properties"]["write.parquet.compression-codec"] == "zstd"
    assert created["properties"]["write.parquet.compression-level"] == "3"

    appended, props = table.appended[0]
    assert appended.schema == pa.schema(
        [
            pa.field("p_bucket_version", pa.int32(), nullable=False),
            pa.field("p_bucket", pa.int32(), nullable=False),
            pa.field("p_min", pa.int64(), nullable=False),
        ]
    )
    assert appended.to_pylist() == rows
    assert props["funbuns.boundary_version"] == "1"
