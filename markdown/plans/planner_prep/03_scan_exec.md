# 03_scan_exec

status: done (merged 0bb87bd; reworked post-review)

SourceTableReader over ScanPlan; non-MOR split tasks open through vendored `ReaderFactoryRegistry` (split→row-group mapping, field-id projection, FileIO reads — all vendored); MOR path via FileScanTaskReader; executor binary-slices batches to the plan key window; `reader->residual()` is the consumer contract. Selecting identity-partition columns errors on BOTH paths (00 holes registry).
