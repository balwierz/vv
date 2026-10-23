// SPDX-License-Identifier: MIT
// tests/run_tests.sh: file_features / tab_traits for each file given, one
// line per file and one per tab:
//   file <name> region=<none|ready|needs-index> [by-columns] pileup=0|1 tags=…
//        contigs=… variants=… samples=… expand=<column> hint=<region_hint>
//   tab <label> matrix=0|1 slices=0|1 coords=0|1
// --matrix-long opens the files after it with --matrix long.
// --narrow FILE COLUMN keep|drop CURRENT VALUE…: the filter
// narrow_filter_by_values builds (a VALUE of \N is a null), or "error: …".
#include "vv/vvcore.hpp"

#include <cstdio>
#include <string>

static int narrow(int argc, char** argv) {
    if (argc < 7) { std::fprintf(stderr, "--narrow FILE COLUMN keep|drop CURRENT VALUE...\n"); return 2; }
    Config cfg;
    cfg.path = argv[2];
    std::unique_ptr<TabularSource> src;
    if (std::string e = open_source(cfg.path, cfg, &src); !e.empty() || !src) {
        std::printf("error: %s\n", e.c_str());
        return 1;
    }
    const auto schema = src->schema();
    const int col = schema->GetFieldIndex(argv[3]);
    std::vector<ValueCount> values;
    for (int i = 6; i < argc; ++i) {
        ValueCount v;
        if (std::string(argv[i]) == "\\N") v.null = true;
        else v.value = argv[i];
        values.push_back(v);
    }
    std::string out, why;
    if (narrow_filter_by_values(argv[5], *schema, col, values, std::string(argv[4]) == "drop", &out, &why))
        std::printf("%s\n", out.c_str());
    else
        std::printf("error: %s\n", why.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--narrow") return narrow(argc, argv);
    int rc = 0;
    bool matrix_long = false;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        if (path == "--matrix-long") { matrix_long = true; continue; }
        const FileFeatures f = file_features(path);
        const char* region = f.region == FileFeatures::Region::Ready      ? "ready"
                           : f.region == FileFeatures::Region::NeedsIndex ? "needs-index"
                                                                          : "none";
        std::string name = path.substr(path.find_last_of('/') + 1);
        std::printf("file %s region=%s%s pileup=%d tags=%d contigs=%d variants=%d samples=%d "
                    "expand=%s hint=%s\n",
                    name.c_str(), region, f.region_by_columns ? " by-columns" : "", f.pileup,
                    f.tags, f.contigs, f.variants, f.has_samples,
                    f.expand_column.empty() ? "-" : f.expand_column.c_str(),
                    f.region_hint.empty() ? "-" : f.region_hint.c_str());
        Config cfg;
        cfg.path = path;
        if (matrix_long) cfg.matrix = "long";
        std::unique_ptr<TabularSource> src;
        if (std::string e = open_source(path, cfg, &src); !e.empty() || !src) {
            std::printf("open %s: %s\n", name.c_str(), e.c_str());
            rc = 1;
            continue;
        }
        auto tabs = src->expand_tabs();
        tabs.insert(tabs.begin(), std::move(src));
        for (const auto& t : tabs) {
            const TabTraits tt = tab_traits(*t);
            std::printf("tab %s matrix=%d slices=%d coords=%d\n", t->tab_label().c_str(),
                        tt.matrix_tab, tt.slices, tt.coord_columns);
        }
    }
    return rc;
}
