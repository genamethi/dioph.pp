# 04_query_verify

status: done (merged 0bb87bd)

QueryService on the new seams: column_binder everywhere; GroupCount takes GroupKey descriptors (Column | Derived) with product vocab ("bits"/"r") mapped at composition roots; Extent over PlanTableScan (key_max/key_name from traits); order-requiring paths (ScanByK, windowed GroupCount) check traits and fail loudly. verify: AllChecks() registry, CheckSpec.requires_ascending enforced. LookupPrime/LookupPartitions keep per-row equality checks — that IS residual application for the unsorted case, correct before and after any sort declaration.
