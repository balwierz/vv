# Fish tab completion for vv (and the vh symlink)
# Install: cp vv.fish ~/.config/fish/completions/

# Disable default file completion so we can control extensions
complete -c vv -c vh -F

# ── File arguments ───────────────────────────────────────────────────────────
for ext in parquet arrow arrows feather lociss bam cram sam vcf vcf.gz vcf.bz2 vcf.xz vcf.zst bcf gff gff.gz gff.bz2 gff.xz gff.zst gff3 gff3.gz gff3.bz2 gff3.xz gff3.zst gtf gtf.gz gtf.bz2 gtf.xz gtf.zst bed bed.gz bed.bz2 bed.xz bed.zst narrowPeak narrowPeak.gz narrowPeak.bz2 narrowPeak.xz narrowPeak.zst broadPeak broadPeak.gz broadPeak.bz2 broadPeak.xz broadPeak.zst gappedPeak gappedPeak.gz gappedPeak.bz2 gappedPeak.xz gappedPeak.zst bedGraph bedGraph.gz bedGraph.bz2 bedGraph.xz bedGraph.zst bg bg.gz bg.bz2 bg.xz bg.zst tagAlign tagAlign.gz tagAlign.bz2 tagAlign.xz tagAlign.zst tsv tsv.gz tsv.bz2 tsv.xz tsv.zst csv csv.gz csv.bz2 csv.xz csv.zst fa fa.gz fa.zst fa.bz2 fa.xz fasta fasta.gz fasta.zst fasta.bz2 fasta.xz fna fna.gz fna.zst fna.bz2 fna.xz faa faa.gz faa.zst faa.bz2 faa.xz ffn ffn.gz ffn.zst ffn.bz2 ffn.xz frn frn.gz frn.zst frn.bz2 frn.xz fq fq.gz fq.zst fq.bz2 fq.xz fastq fastq.gz fastq.zst fastq.bz2 fastq.xz paf paf.gz paf.bz2 paf.xz paf.zst mtx mtx.gz mtx.bz2 mtx.xz mtx.zst bedpe bedpe.gz bedpe.bz2 bedpe.xz bedpe.zst pairs pairs.gz pairs.bz2 pairs.xz pairs.zst gct gct.gz gct.bz2 gct.xz gct.zst maf maf.gz maf.bz2 maf.xz maf.zst bim bim.gz bim.bz2 bim.xz bim.zst fam fam.gz fam.bz2 fam.xz fam.zst pvar pvar.gz pvar.bz2 pvar.xz pvar.zst psam psam.gz psam.bz2 psam.xz psam.zst json json.gz json.bz2 json.xz json.zst ndjson ndjson.gz ndjson.bz2 ndjson.xz ndjson.zst jsonl jsonl.gz jsonl.bz2 jsonl.xz jsonl.zst geojson geojson.gz geojson.bz2 geojson.xz geojson.zst ipynb ipynb.gz ipynb.bz2 ipynb.xz ipynb.zst har har.gz har.bz2 har.xz har.zst bb bigBed bigbed bw bigWig bigwig 2bit sqlite sqlite3 db xlsx xlsm ods fods orc npz npy pileup pileup.gz pileup.bz2 pileup.xz pileup.zst mpileup mpileup.gz mpileup.bz2 mpileup.xz mpileup.zst pile pile.gz pile.bz2 pile.xz pile.zst md markdown mdown mkd h5 h5ad hdf5 loom txt txt.gz txt.bz2 txt.xz txt.zst text text.gz text.bz2 text.xz text.zst log log.gz log.bz2 log.xz log.zst m8 m8.gz m8.zst m8.bz2 m8.xz blast6 blast6.gz blast6.zst blast6.bz2 blast6.xz outfmt6 outfmt6.gz outfmt6.zst outfmt6.bz2 outfmt6.xz h5mu h5seurat fast5 nc nc4 wig wig.gz wig.zst wig.bz2 wig.xz rds RData rda cool mcool
    complete -c vv -c vh -F -a "*.$ext"
end

# ── Dynamic completion helpers (columns / tabs from the file on the line) ─────
# The input file already on the command line: the sole positional. Skip the
# argument of any value-taking option so an output path is not taken as input.
function __vv_file
    set -l valopts n w c @ threads decode-threads delimiter in-delimiter d header color theme box r region \
        window regions-file region-cols slop coords tail sort tags expand parquet arrow \
        feather compression image-mode samples matrix f fasta select cols filter tab unique sample \
        exclude-flags ff require-flags rf min-mapq min-bq
    set -l toks (commandline -opc)
    set -l i 2
    while test $i -le (count $toks)
        set -l tok $toks[$i]
        if string match -q -- '-*' $tok
            if contains -- (string replace -r '^-+' '' -- $tok) $valopts
                set i (math $i + 1)
            end
        else if test -f $tok
            echo $tok
            return 0
        end
        set i (math $i + 1)
    end
end

# Bounded vv invocation so a large or slow file cannot hang the prompt.
function __vv_run
    set -l vv (commandline -opc)[1]
    test -n "$vv"; or set vv vv
    if type -q timeout
        timeout 1s $vv $argv 2>/dev/null
    else if type -q gtimeout
        gtimeout 1s $vv $argv 2>/dev/null
    else
        $vv $argv 2>/dev/null
    end
end

function __vv_tabs
    set -l f (__vv_file); test -n "$f"; and __vv_run --list-tabs $f
end

function __vv_columns
    set -l f (__vv_file); test -n "$f"; and __vv_run --list-columns $f
end

# Comma-aware variant for list flags (--select etc.): prepend the already-typed
# comma prefix so fish replaces the whole token rather than a single item.
function __vv_columns_csv
    set -l f (__vv_file); test -n "$f"; or return
    set -l cur (commandline -ct)
    set -l pre ''
    string match -q -- '*,*' $cur; and set pre (string replace -r '[^,]*$' '' -- $cur)
    for c in (__vv_run --list-columns $f)
        echo $pre$c
    end
end

# ── Flags ────────────────────────────────────────────────────────────────────
complete -c vv -c vh -s h -l help          -d 'Show help'
complete -c vv -c vh -s V -l version       -d 'Print version'
complete -c vv -c vh -s i -l interactive   -d 'Open ncurses row browser'
complete -c vv -c vh -l no-interactive     -d 'Force plain table output'
complete -c vv -c vh -s t -l table         -d 'Force plain table output (alias of --no-interactive)'
complete -c vv -c vh -l vertical           -d 'Transposed (vertical-head) preview'

# Table options
complete -c vv -c vh -s n -r               -d 'Rows to display (default: 10, 0 = all)'
complete -c vv -c vh -s w -r               -d 'Max cell width (default: 32)'
complete -c vv -c vh -s c -r               -d 'Max columns to show (default: all)'
complete -c vv -c vh -l no-index           -d 'Suppress the row-index column'

# Region (tabix / LociSSD / BCF)
complete -c vv -c vh -s r -l region -r     -d 'Region for tabix-indexed file (e.g. chr1:1000-2000)'
complete -c vv -c vh -l window -r          -d 'Alias of --region'
complete -c vv -c vh -l regions-file -r -F -d 'BED file with additional windows'
complete -c vv -c vh -l region-cols -x -a '(__vv_columns_csv)' -d 'chrom,start,end column names for plain Parquet'
complete -c vv -c vh -l slop -r            -d 'Pad each window by N bp'
complete -c vv -c vh -l coords -r -a 'UCSC Kent NCBI GenBank 0-based 1-based bed tabix' -d 'Coordinate convention for -r (UCSC default, NCBI = 1-based inclusive)'
complete -c vv -c vh -l tail -r            -d 'Show the last N rows'

# Performance
complete -c vv -c vh -s @ -l threads -r    -d 'Worker threads for I/O and decode (0 = auto)'
complete -c vv -c vh -l decode-threads -r  -d 'Arrow CPU pool size for Parquet/CSV decode (0 = follow --threads)'

# --color
complete -c vv -c vh -l color              -d 'Colorize output (auto/always/never)'
complete -c vv -c vh -l color -r -a 'auto always never' -d 'Color mode'
complete -c vv -c vh -l theme -r -a 'default dark light solarized-dark solarized-light' -d 'Color palette'
complete -c vv -c vh -l box -x -a 'unicode ascii' -d 'Table frame style (ascii for a C locale / plain-text paste)'

# Delimited output
complete -c vv -c vh -l tsv                -d 'Write tab-separated values'
complete -c vv -c vh -l csv                -d 'Write comma-separated values'
complete -c vv -c vh -l delimiter -r       -d 'Write with a custom single-character delimiter'
complete -c vv -c vh -s d -l in-delimiter -x -a 'tab space comma semicolon pipe' -d 'Read input with this field separator (overrides extension)'
complete -c vv -c vh -l header -x -a 'auto on off' -d 'Is the CSV/TSV first row a header? (auto-detect by default)'
complete -c vv -c vh -l no-header          -d 'Omit the header row in delimited output'

# Visualization
complete -c vv -c vh -l heatmap            -d 'Render numeric columns as a terminal heatmap'
complete -c vv -c vh -l image-mode -r -a 'auto kitty iterm sixel halfblock ascii' -d 'Heatmap backend'
complete -c vv -c vh -l samples -r -a 'struct long text' -d 'VCF/BCF sample columns: typed structs, one row per sample, or text'
complete -c vv -c vh -l matrix -r -a 'wide long' -d 'HDF5 matrix tabs: cells x genes, or one row per non-zero'

# Parquet output
complete -c vv -c vh -l parquet -r -F      -d 'Write a Parquet file at this path'
complete -c vv -c vh -l arrow   -r -F      -d 'Write an Arrow IPC file (Feather v2) at this path'
complete -c vv -c vh -l feather -r -F      -d 'Write an Arrow IPC file (Feather v2) at this path'
complete -c vv -c vh -l compression -r -a 'zstd snappy gzip lz4 none' -d 'Parquet codec'

# JSON output / projection / filter / describe / schema
complete -c vv -c vh -l json               -d 'Write JSON array of row objects'
complete -c vv -c vh -l ndjson             -d 'Write one JSON object per line'
complete -c vv -c vh -l text               -d 'Read the file as plain text whatever its extension'
complete -c vv -c vh -l expand -x -a '(__vv_columns)' -d 'Unpack a packed key=value column into real columns'
complete -c vv -c vh -l sort -x -a '(__vv_columns)' -d 'Order rows by a column before output (COL or COL:desc)'
complete -c vv -c vh -l formats            -d 'Print the supported-format table and exit'
complete -c vv -c vh -l list-columns       -d 'Print column names, one per line, and exit'
complete -c vv -c vh -l list-tabs          -d 'Print component tab names and exit'
complete -c vv -c vh -l md                 -d 'Write GitHub-flavored markdown table'
complete -c vv -c vh -l markdown           -d 'Alias of --md'
complete -c vv -c vh -l validate           -d 'Check LociSSD invariants and exit'
complete -c vv -c vh -l decode-pileup      -d 'mpileup: explode bases into A/C/G/T/N + ins/del + strand + mean_qual columns'
complete -c vv -c vh -l pileup             -d 'BAM/CRAM: emit mpileup-style per-base rows via htslib bam_plp'
set -l __vv_flags PAIRED PROPER_PAIR UNMAP MUNMAP REVERSE MREVERSE READ1 READ2 SECONDARY QCFAIL DUP SUPPLEMENTARY
complete -c vv -c vh -l exclude-flags -x -a "$__vv_flags" -d '--pileup: skip reads with any of these flags (default UNMAP,SECONDARY,QCFAIL,DUP)'
complete -c vv -c vh -l ff -x -a "$__vv_flags" -d 'Alias of --exclude-flags'
complete -c vv -c vh -l require-flags -x -a "$__vv_flags" -d '--pileup: keep only reads with all of these flags'
complete -c vv -c vh -l rf -x -a "$__vv_flags" -d 'Alias of --require-flags'
complete -c vv -c vh -l min-mapq -x        -d '--pileup: skip reads below this mapping quality (default 0)'
complete -c vv -c vh -l min-bq -x          -d '--pileup: drop bases below this base quality (default 13)'
complete -c vv -c vh -l count-orphans      -d '--pileup: keep paired reads that are not properly paired'
complete -c vv -c vh -l ignore-overlaps    -d '--pileup: no mate-overlap quality merging'
complete -c vv -c vh -l tags -x            -d 'BAM/CRAM/SAM/PAF: add a typed column per aux tag (comma-separated, e.g. NM,AS,RG)'
complete -c vv -c vh -s f -l fasta -r -F   -d 'reference FASTA (.fai): --pileup ref column + ./, notation, or CRAM decoding'
complete -c vv -c vh -l select -x -a '(__vv_columns_csv)' -d 'Project columns: names, globs, N-M ranges, @types, !exclusions'
complete -c vv -c vh -l cols -x -a '(__vv_columns_csv)' -d 'Alias of --select'
complete -c vv -c vh -l filter -x -a '(__vv_columns)' -d 'Row predicate: <col> <op> <value> [AND/OR ...]'
complete -c vv -c vh -l schema             -d 'Print schema + metadata and exit'
complete -c vv -c vh -l tab -x -a '(__vv_tabs)' -d 'View a named component tab (AnnData obs/var/X, sheet)'
complete -c vv -c vh -l describe           -d 'Per-column statistics (add --json/--ndjson for machine-readable)'
complete -c vv -c vh -l count              -d 'Print the row count and exit'
complete -c vv -c vh -l stats              -d 'Parquet metadata dump (no data read)'
complete -c vv -c vh -l contigs            -d 'BAM/CRAM/SAM, VCF/BCF: list reference sequences + detect assembly'
complete -c vv -c vh -l seq-stats          -d 'FASTA/FASTQ: count, lengths, N50, GC, Q20/Q30'
complete -c vv -c vh -l flatten            -d 'struct columns as one column per leaf (st.a, st.b.c)'
complete -c vv -c vh -l tree               -d 'JSON: open the tree viewer'
complete -c vv -c vh -l pretty             -d 'JSON: print the document re-indented'
complete -c vv -c vh -l json-paths         -d 'JSON: one path = value line per leaf'
complete -c vv -c vh -l no-tree            -d 'JSON: read it as a table of records'
complete -c vv -c vh -l gt-stats           -d 'VCF/BCF: add per-variant genotype summary columns (het/hom/missing, AC/AN/AF, call_rate)'
complete -c vv -c vh -l unique -r          -d 'Distinct value counts per column'
complete -c vv -c vh -l distinct           -d 'Drop duplicate rows (SQL SELECT DISTINCT), over the shown columns'
complete -c vv -c vh -l sample -r          -d 'Reservoir-sample N random rows'
