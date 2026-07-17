# 02_scan_plan

status: done (merged 0bb87bd; reworked post-review)

TableReadTraits (declared non-identity sort transform = loud NotImplemented); spec-shaped ScanPlanRequest/ScanPlan; `scan::FileScanTask` carries `iceberg::Split{offset,length}` — one task per surviving split, boundaries from footer `RowGroup::file_offset()`, the same accessor the vendored reader's split mapping uses, so planner and reader agree by construction; footer reads through FileIO (`OpenArrowInputStream`, internal header via `ICEBERG_SRC_CPPFLAGS`); column_binder for loud typed access. Rework history in git (row_groups ordinals → splits).
