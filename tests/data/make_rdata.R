# Regenerates the R fixtures (R is not needed to run the tests):
#   Rscript tests/data/make_rdata.R
# tiny.rds: a data frame with character, double, integer, logical, factor,
# Date and POSIXct columns and an NA in each; tiny.xz.rds: the same, xz-
# compressed; tiny.RData: that data frame, a second one with row names, an
# integer vector and a matrix with dimnames; tiny.list.rds: a list (refused).
setwd(dirname(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE))))
df <- data.frame(gene = c("BRCA1", "TP53", NA), log2fc = c(1.5, -0.25, NA),
                 n = c(10L, NA, 3L), sig = c(TRUE, FALSE, NA),
                 tissue = factor(c("liver", "brain", "liver")),
                 day = as.Date(c("2024-01-02", "2024-03-04", NA)),
                 ts = as.POSIXct(c("2024-01-02 10:00:00", "2024-03-04 12:30:00", NA), tz = "UTC"),
                 stringsAsFactors = FALSE)
saveRDS(df, "tiny.rds")
saveRDS(df, "tiny.xz.rds", compress = "xz")
named <- data.frame(x = 1:3, row.names = c("a", "b", "c"))
counts <- matrix(1:6, nrow = 2, dimnames = list(c("r1", "r2"), c("A", "B", "C")))
ids <- 1:4
save(df, named, ids, counts, file = "tiny.RData")
saveRDS(list(a = 1, b = "x"), "tiny.list.rds")
