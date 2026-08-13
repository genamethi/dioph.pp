# 01_writer

status: done (merged 0bb87bd)

Writer takes declared stat columns (`WriterConfig.stat_columns`, StatColumn{name,sorted}) and computes typed Literal bounds itself; `WrittenFile.bounds` map; partition-tuple fallback deleted; `partition_spec` required at Make. Detail in git history of writer.{h,cc}.
