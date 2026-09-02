# Fish tab completion for vv (and the vh symlink)
# Install: cp vv.fish ~/.config/fish/completions/

# Disable default file completion so we can control extensions
complete -c vv -c vh -F

# ── File arguments ───────────────────────────────────────────────────────────
for ext in parquet arrow feather lociss bam cram sam vcf vcf.gz bcf gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz narrowPeak narrowPeak.gz broadPeak broadPeak.gz gappedPeak gappedPeak.gz bedGraph bedGraph.gz bg bg.gz tagAlign tagAlign.gz tsv tsv.gz csv csv.gz fa fa.gz fasta fasta.gz fna fna.gz faa faa.gz ffn ffn.gz frn frn.gz fq fq.gz fastq fastq.gz paf paf.gz bb bigBed bigbed bw bigWig bigwig 2bit sqlite sqlite3 db xlsx xlsm ods fods orc npz npy pileup pileup.gz mpileup mpileup.gz pile pile.gz md markdown mdown mkd h5 h5ad hdf5 loom txt txt.gz text text.gz log log.gz
    complete -c vv -c vh -F -a "*.$ext"
end

# ── Dynamic completion helpers (columns / tabs from the file on the line) ─────
# The input file already on the command line: the sole positional. Skip the
# argument of any value-taking option so an output path is not taken as input.
function __vv_file
    set -l valopts n w c @ threads decode-threads delimiter color theme r region \
        window regions-file region-cols slop coords tail sort expand parquet arrow \
        feather compression image-mode f fasta select cols filter tab unique sample
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

# Delimited output
complete -c vv -c vh -l tsv                -d 'Write tab-separated values'
complete -c vv -c vh -l csv                -d 'Write comma-separated values'
complete -c vv -c vh -l delimiter -r       -d 'Write with a custom single-character delimiter'
complete -c vv -c vh -l no-header          -d 'Omit the header row in delimited output'

# Visualization
complete -c vv -c vh -l heatmap            -d 'Render numeric columns as a terminal heatmap'
complete -c vv -c vh -l image-mode -r -a 'auto kitty sixel halfblock ascii' -d 'Heatmap backend'

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
complete -c vv -c vh -s f -l fasta -r -F   -d 'reference FASTA (.fai): --pileup ref column + ./, notation, or CRAM decoding'
complete -c vv -c vh -l select -x -a '(__vv_columns_csv)' -d 'Project columns: names, globs, N-M ranges, @types, !exclusions'
complete -c vv -c vh -l cols -x -a '(__vv_columns_csv)' -d 'Alias of --select'
complete -c vv -c vh -l filter -x -a '(__vv_columns)' -d 'Row predicate: <col> <op> <value> [AND/OR ...]'
complete -c vv -c vh -l schema             -d 'Print schema + metadata and exit'
complete -c vv -c vh -l tab -x -a '(__vv_tabs)' -d 'View a named component tab (AnnData obs/var/X, sheet)'
complete -c vv -c vh -l describe           -d 'Per-column statistics (add --json/--ndjson for machine-readable)'
complete -c vv -c vh -l count              -d 'Print the row count and exit'
complete -c vv -c vh -l stats              -d 'Parquet metadata dump (no data read)'
complete -c vv -c vh -l unique -r          -d 'Distinct value counts per column'
complete -c vv -c vh -l sample -r          -d 'Reservoir-sample N random rows'
