"""
Runtime patches for third-party libraries.

Imported for side effects from ``funbuns.__init__``. Each patch is idempotent
and documents the upstream issue it addresses so the patch can be removed
when the library is fixed.
"""

from __future__ import annotations


def _patch_pyiceberg_sort_order_id() -> None:
    """
    pyiceberg 0.11.1 hardcodes ``sort_order_id=None`` in
    ``parquet_file_to_data_file``. Every file registered via
    ``Table.add_files(...)`` lands in the manifest stamped as unsorted even
    when the writer pre-sorts rows. Downstream SQL engines then plan
    redundant sort steps on scans and MV rebuilds.

    We stamp each new DataFile with the table's ``default_sort_order_id``
    after construction. Safe for funbuns because ``write_batch`` enforces
    the declared sort order before calling ``add_files``. If pyiceberg ever
    adds the field to its constructor or changes the ``_data`` layout, this
    patch should be revisited (slot 15 is the sort_order_id position in
    pyiceberg 0.11.1's DataFile Record).
    """
    import pyiceberg.io.pyarrow as _m

    if getattr(_m.parquet_file_to_data_file, "_funbuns_patched", False):
        return

    _orig = _m.parquet_file_to_data_file

    def _patched(io, table_metadata, file_path):
        df = _orig(io, table_metadata, file_path)
        df._data[15] = table_metadata.default_sort_order_id
        return df

    _patched._funbuns_patched = True  # type: ignore[attr-defined]
    _m.parquet_file_to_data_file = _patched


def _patch_pyiceberg_extra_parquet_columns() -> None:
    """
    pyiceberg 0.11.1 assumes every physical Parquet column maps to an Iceberg
    table field while collecting file metrics. Native compacted files retain
    ``batch_id`` as a physical provenance column, but the Iceberg tables expose
    only the analytical schema. Skip unmapped physical columns for metrics.
    """
    import pyarrow
    import pyiceberg.io.pyarrow as _m
    from pyiceberg.types import DecimalType
    from pyiceberg.utils.decimal import unscaled_to_decimal

    if getattr(
        _m.data_file_statistics_from_parquet_metadata,
        "_funbuns_extra_columns_patched",
        False,
    ):
        return

    def _patched(parquet_metadata, stats_columns, parquet_column_mapping):
        column_sizes = {}
        value_counts = {}
        split_offsets = []
        null_value_counts = {}
        nan_value_counts = {}
        col_aggs = {}
        invalidate_col = set()

        for r in range(parquet_metadata.num_row_groups):
            row_group = parquet_metadata.row_group(r)
            data_offset = row_group.column(0).data_page_offset
            dictionary_offset = row_group.column(0).dictionary_page_offset
            if row_group.column(0).has_dictionary_page and dictionary_offset < data_offset:
                split_offsets.append(dictionary_offset)
            else:
                split_offsets.append(data_offset)

            for pos in range(parquet_metadata.num_columns):
                column = row_group.column(pos)
                field_id = parquet_column_mapping.get(column.path_in_schema)
                if field_id is None or field_id not in stats_columns:
                    continue

                stats_col = stats_columns[field_id]
                column_sizes.setdefault(field_id, 0)
                column_sizes[field_id] += column.total_compressed_size

                if stats_col.mode == _m.MetricsMode(_m.MetricModeTypes.NONE):
                    continue

                value_counts[field_id] = value_counts.get(field_id, 0) + column.num_values

                if column.is_stats_set:
                    try:
                        statistics = column.statistics
                        if statistics.has_null_count:
                            null_value_counts[field_id] = (
                                null_value_counts.get(field_id, 0)
                                + statistics.null_count
                            )

                        if stats_col.mode == _m.MetricsMode(_m.MetricModeTypes.COUNTS):
                            continue

                        if field_id not in col_aggs:
                            try:
                                col_aggs[field_id] = _m.StatsAggregator(
                                    stats_col.iceberg_type,
                                    statistics.physical_type,
                                    stats_col.mode.length,
                                )
                            except ValueError as e:
                                raise ValueError(
                                    f"{e} for column '{stats_col.column_name}'"
                                ) from e

                        if (
                            isinstance(stats_col.iceberg_type, DecimalType)
                            and statistics.physical_type != "FIXED_LEN_BYTE_ARRAY"
                        ):
                            scale = stats_col.iceberg_type.scale
                            if statistics.min_raw is not None:
                                col_aggs[field_id].update_min(
                                    unscaled_to_decimal(statistics.min_raw, scale)
                                )
                            if statistics.max_raw is not None:
                                col_aggs[field_id].update_max(
                                    unscaled_to_decimal(statistics.max_raw, scale)
                                )
                        else:
                            col_aggs[field_id].update_min(statistics.min)
                            col_aggs[field_id].update_max(statistics.max)
                    except pyarrow.lib.ArrowNotImplementedError as e:
                        invalidate_col.add(field_id)
                        _m.logger.warning(e)
                else:
                    invalidate_col.add(field_id)
                    _m.logger.warning(
                        "PyArrow statistics missing for column %d when writing file",
                        pos,
                    )

        split_offsets.sort()
        for field_id in invalidate_col:
            col_aggs.pop(field_id, None)
            null_value_counts.pop(field_id, None)

        return _m.DataFileStatistics(
            record_count=parquet_metadata.num_rows,
            column_sizes=column_sizes,
            value_counts=value_counts,
            null_value_counts=null_value_counts,
            nan_value_counts=nan_value_counts,
            column_aggregates=col_aggs,
            split_offsets=split_offsets,
        )

    _patched._funbuns_extra_columns_patched = True  # type: ignore[attr-defined]
    _m.data_file_statistics_from_parquet_metadata = _patched


def _patch_hive_metastore_get_table() -> None:
    """
    pyiceberg 0.11.1's ``HiveCatalog`` calls the deprecated Thrift method
    ``get_table(dbname=..., tbl_name=...)``. Hive Metastore 4 no longer
    exposes that method and raises ``TApplicationException: Invalid method
    name: 'get_table'``. The newer ``get_table_req(GetTableRequest)`` is
    already present in pyiceberg's bundled ``hive_metastore`` IDL, so we
    redirect the old call to it at the Thrift ``Client`` class level. All
    pyiceberg load/rename paths then work against HMS 4 transparently.
    Remove when pyiceberg ships a version that uses ``get_table_req``
    natively.
    """
    from hive_metastore.ThriftHiveMetastore import Client
    from hive_metastore.ttypes import GetTableRequest

    if getattr(Client.get_table, "_funbuns_patched", False):
        return

    def _patched(self, dbname=None, tbl_name=None):
        return self.get_table_req(
            GetTableRequest(dbName=dbname, tblName=tbl_name)
        ).table

    _patched._funbuns_patched = True  # type: ignore[attr-defined]
    Client.get_table = _patched


_patch_pyiceberg_sort_order_id()
_patch_pyiceberg_extra_parquet_columns()
_patch_hive_metastore_get_table()
