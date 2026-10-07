# Regenerates the R fixtures (R is not needed to run the tests):
#   Rscript tests/data/make_rdata.R
# (Matrix, bit64, GenomicRanges, SingleCellExperiment and SeuratObject must be
# installed; set R_LIBS for a private library.)
# tiny.rds: a data frame with character, double, integer, logical, factor,
# Date and POSIXct columns and an NA in each; tiny.xz.rds: the same, xz-
# compressed; tiny.RData: that data frame, a second one with row names, an
# integer vector and a matrix with dimnames; tiny.list.rds: a list of two
# scalars (a structure tab only).
# tiny.ascii.rds / tiny.native.rds / tiny.nocomp.rds: a small data frame in
# the ASCII, native-binary and uncompressed XDR serializations.
# tiny.nested.rds: a list holding a data frame, a nested matrix and vectors,
# an environment, a function, GRanges, bit64 integer64, complex, raw, a
# compact sequence (ALTREP) and a deferred string vector.
# tiny.sparse.RData: Matrix-package matrices (dgC / dgT / dgR / lgC / ngC /
# dge) of one 6 x 4 pattern, a 1200 x 250 dgCMatrix (larger than the
# preview) and a list (`lst`).
# tiny.seurat.rds (Seurat v5 assay: counts and data layers, meta.data, a PCA
# reduction, an SNN graph), tiny.seurat3.rds (a v3 assay), tiny.sce.rds
# (SingleCellExperiment: counts, logcounts, colData, rowData, a PCA).
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

small <- data.frame(a = 1:3, b = c("x", "y", NA))
saveRDS(small, "tiny.ascii.rds", ascii = TRUE)
saveRDS(small, "tiny.nocomp.rds", compress = FALSE)
con <- gzfile("tiny.native.rds", "wb"); serialize(small, con, xdr = FALSE); close(con)

suppressPackageStartupMessages({
    library(Matrix); library(GenomicRanges); library(SingleCellExperiment); library(SeuratObject)
})
env <- new.env()
assign("counts", c(a = 1L, b = 2L), envir = env)
gr <- GRanges(c("chr1", "chr1", "chr2"), IRanges(c(100, 200, 50), width = c(10, 20, 5)),
              strand = c("+", "-", "*"), score = c(1.5, 2, NA))
names(gr) <- c("r1", "r2", "r3")
nested <- list(df = data.frame(id = c("s1", "s2"), dose = c(0.5, 1)),
               inner = list(m = matrix(1:4, 2, dimnames = list(c("x", "y"), c("p", "q"))),
                            when = as.Date(c("2024-01-01", "2024-06-30"))),
               env = env, f = function(x) x + 1, ranges = gr,
               big = bit64::as.integer64(c("9007199254740993", NA)),
               z = complex(real = c(1, -2), imaginary = c(0.5, -1)), bytes = as.raw(c(0, 255)),
               seq = 1:100000, chars = as.character(c(10L, 20L, 30L)))
saveRDS(nested, "tiny.nested.rds")

pat <- sparseMatrix(i = c(1, 3, 6, 2, 4), j = c(1, 1, 2, 3, 4), x = c(1, 2, 3, 4, 5), dims = c(6, 4),
                    dimnames = list(paste0("g", 1:6), paste0("c", 1:4)))
dgc <- pat
dgt <- as(pat, "TsparseMatrix")
dgr <- as(pat, "RsparseMatrix")
lgc <- pat > 2
ngc <- as(pat, "nMatrix")
dge <- Matrix(matrix(c(1.5, 2, 3, 4), 2), sparse = FALSE)
set.seed(7)
wide <- rsparsematrix(1200, 250, 0.01, rand.x = function(n) round(runif(n, 1, 9)))
dimnames(wide) <- list(paste0("G", 1:1200), paste0("C", 1:250))
lst <- list(df = data.frame(k = 1:2))
save(dgc, dgt, dgr, lgc, ngc, dge, wide, lst, file = "tiny.sparse.RData")

counts <- sparseMatrix(i = c(1, 2, 2, 4, 5, 6, 3), j = c(1, 1, 2, 3, 3, 4, 5), x = c(3, 1, 7, 2, 5, 4, 6),
                       dims = c(6, 5), dimnames = list(paste0("gene", 1:6), paste0("cell", 1:5)))
so <- CreateSeuratObject(counts = counts, project = "toy")
LayerData(so, "data") <- log1p(counts)
so$group <- factor(c("a", "b", "a", "b", "a"))
emb <- matrix(c(0.1, 0.2, 0.3, 0.4, 0.5, 1, 2, 3, 4, 5), 5, 2, dimnames = list(colnames(counts), c("PC_1", "PC_2")))
so[["pca"]] <- CreateDimReducObject(embeddings = emb, key = "PC_", assay = "RNA")
g <- sparseMatrix(i = c(1, 2, 3), j = c(2, 3, 1), x = c(0.5, 1, 0.25), dims = c(5, 5),
                  dimnames = list(colnames(counts), colnames(counts)))
so[["RNA_snn"]] <- as.Graph(g)
saveRDS(so, "tiny.seurat.rds")
options(Seurat.object.assay.version = "v3")
saveRDS(CreateSeuratObject(counts = counts, project = "toy3"), "tiny.seurat3.rds")
sce <- SingleCellExperiment(assays = list(counts = counts, logcounts = log1p(counts)),
                            colData = DataFrame(group = c("a", "b", "a", "b", "a"), n = 1:5),
                            rowData = DataFrame(symbol = paste0("S", 1:6)))
reducedDim(sce, "PCA") <- matrix(c(1, 2, 3, 4, 5, -1, -2, -3, -4, -5), 5, 2)
saveRDS(sce, "tiny.sce.rds")
