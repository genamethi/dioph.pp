//Needs to remove writer.h and any direct arrow/io. May be obsolete,
//But out of the whole primitive factors/covering system work
//The most general useful tool (it came first.)
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <flint/ulong_extras.h>

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <cmath>

int main() {
    arrow::Int32Builder m_builder;
    arrow::StringBuilder mm_str_builder;
    arrow::StringBuilder factors_builder;
    arrow::StringBuilder primitive_builder;

    std::map<uint64_t, int32_t> first_seen;

    for (int32_t m = 1; m <= 64; ++m) {
        uint64_t mm;
        if (m == 64) {
            // 2^64 - 1 is special for uint64_t
            mm = ~static_cast<uint64_t>(0);
        } else {
            mm = (static_cast<uint64_t>(1) << m) - 1;
        }

        n_factor_t factors;
        n_factor_init(&factors);
        n_factor(&factors, static_cast<ulong>(mm), 1);

        std::string all_factors_str = "";
        std::string primitive_factors_str = "";

        for (int i = 0; i < factors.num; ++i) {
            uint64_t f = static_cast<uint64_t>(factors.p[i]);
            int32_t exp = static_cast<int32_t>(factors.exp[i]);
            
            std::string part = std::to_string(f);
            if (exp > 1) part += "^" + std::to_string(exp);
            
            if (!all_factors_str.empty()) all_factors_str += ", ";
            all_factors_str += part;

            // Check if primitive
            if (first_seen.find(f) == first_seen.end()) {
                first_seen[f] = m;
                if (!primitive_factors_str.empty()) primitive_factors_str += ", ";
                primitive_factors_str += part;
            }
        }

        m_builder.Append(m);
        mm_str_builder.Append(std::to_string(mm));
        factors_builder.Append(all_factors_str);
        primitive_builder.Append(primitive_factors_str);
    }

    auto schema = arrow::schema({
        arrow::field("m", arrow::int32()),
        arrow::field("M_m", arrow::utf8()),
        arrow::field("factors", arrow::utf8()),
        arrow::field("primitive_factors", arrow::utf8())
    });

    std::shared_ptr<arrow::Table> table = arrow::Table::Make(schema, {
        m_builder.Finish().ValueOrDie(),
        mm_str_builder.Finish().ValueOrDie(),
        factors_builder.Finish().ValueOrDie(),
        primitive_builder.Finish().ValueOrDie()
    });

    auto outfile = arrow::io::FileOutputStream::Open("mersenne_reference.parquet").ValueOrDie();
    parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 64);

    std::cout << "Created mersenne_reference.parquet with 64 rows.\n";

    return 0;
}
