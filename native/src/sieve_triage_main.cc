#include "primeparts/source_scan.h"

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <iceberg/expression/expressions.h>

#include <iostream>
#include <vector>
#include <string>
#include <getopt.h>
#include <iomanip>

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string warehouse;
    std::string catalog_table = "obstruction_catalog";
    std::string partition_tag = "uncovered_pass1";
    int32_t threads = 6;
};

void Usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s --warehouse PATH [options]\n"
                         "  --partition TAG        Tag to analyze (default: uncovered_pass1)\n"
                         "  --table NAME           Iceberg table (default: obstruction_catalog)\n"
                         "  --threads N            Number of Arrow threads (default: 6)\n", argv0);
}

std::optional<fs::path> LatestMetadataPath(const fs::path& dir) {
  std::optional<fs::path> latest;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) return std::nullopt;
    if (!entry.is_regular_file()) continue;
    auto path = entry.path();
    if (path.extension() != ".json") continue;
    if (path.filename().string().find(".metadata.json") == std::string::npos) {
      continue;
    }
    if (!latest || path.filename().string() > latest->filename().string()) {
      latest = path;
    }
  }
  return latest;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    static struct option long_options[] = {
        {"warehouse", required_argument, nullptr, 'w'},
        {"partition", required_argument, nullptr, 'p'},
        {"table", required_argument, nullptr, 't'},
        {"threads", required_argument, nullptr, 'n'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:p:t:n:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'w': opts.warehouse = optarg; break;
            case 'p': opts.partition_tag = optarg; break;
            case 't': opts.catalog_table = optarg; break;
            case 'n': opts.threads = std::atoi(optarg); break;
            default: Usage(argv[0]); return 1;
        }
    }

    if (opts.warehouse.empty()) {
        Usage(argv[0]);
        return 1;
    }

    auto cpu_st = arrow::SetCpuThreadPoolCapacity(opts.threads);
    if (!cpu_st.ok()) {
        std::cerr << "Error setting CPU thread pool: " << cpu_st.ToString() << "\n";
        return 1;
    }
    auto io_st = arrow::io::SetIOThreadPoolCapacity(opts.threads);
    if (!io_st.ok()) {
        std::cerr << "Error setting IO thread pool: " << io_st.ToString() << "\n";
        return 1;
    }

    std::string error;
    fs::path metadata_dir = fs::path(opts.warehouse) / "primeparts" / opts.catalog_table / "metadata";
    auto latest_metadata = LatestMetadataPath(metadata_dir);
    if (!latest_metadata) {
        std::cerr << "Error: Could not find table metadata in " << metadata_dir << "\n";
        return 1;
    }

    std::cout << "Triaging gaps in partition: " << opts.partition_tag << "\n";

    auto reader = primeparts::SourceTableReader::OpenMetadata(
        latest_metadata->string(), {"covering_system", "uncovered_mask"}, nullptr, &error);
    if (!reader) {
        std::cerr << "Error opening metadata: " << error << "\n";
        return 1;
    }

    std::vector<int64_t> bit_counts(64, 0);
    int64_t total_rows = 0;

    std::shared_ptr<arrow::RecordBatch> batch;
    while (reader->Next(&batch, &error) && batch) {
        auto sys_arr = std::static_pointer_cast<arrow::StringArray>(batch->column(0));
        auto mask_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(1));
        const int64_t num_rows = batch->num_rows();

        for (int64_t i = 0; i < num_rows; ++i) {
            if (sys_arr->GetString(i) != opts.partition_tag) continue;
            
            total_rows++;
            uint64_t mask = static_cast<uint64_t>(mask_arr->Value(i));
            // Accumulate counts for each set bit
            while (mask != 0) {
                int bit_idx = __builtin_ctzll(mask);
                // __builtin_ctzll returns the number of trailing 0-bits.
                // 1 << bit_idx is the lowest set bit.
                // Since our mask uses 1-based indexing for m (bit 0 corresponds to m=1),
                // bit_idx 0 means m=1, bit_idx 1 means m=2, etc.
                bit_counts[bit_idx]++;
                mask &= (mask - 1); // clear lowest set bit
            }
        }
    }

    std::cout << "\nGap Triage Results (Total Rows: " << total_rows << ")\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << std::left << std::setw(10) << "m" << "Missing Count\n";
    std::cout << "--------------------------------------------------\n";
    for (int i = 0; i < 64; ++i) {
        if (bit_counts[i] > 0) {
            std::cout << std::left << std::setw(10) << (i + 1) << bit_counts[i] << "\n";
        }
    }
    std::cout << "--------------------------------------------------\n";

    return 0;
}