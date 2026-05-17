# Fish tab completion for vv (and the vh symlink)
# Install: cp vv.fish ~/.config/fish/completions/

# Disable default file completion so we can control extensions
complete -c vv -c vh -F

# ── File arguments ───────────────────────────────────────────────────────────
for ext in parquet arrow feather lociss bam cram sam vcf vcf.gz bcf gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz narrowPeak narrowPeak.gz broadPeak broadPeak.gz gappedPeak gappedPeak.gz bedGraph bedGraph.gz bg bg.gz tagAlign tagAlign.gz tsv tsv.gz csv csv.gz fa fa.gz fasta fasta.gz fna fna.gz faa faa.gz ffn ffn.gz frn frn.gz fq fq.gz fastq fastq.gz paf paf.gz bb bigBed bw bigWig 2bit sqlite sqlite3 db xlsx xlsm orc
    complete -c vv -c vh -F -a "*.$ext"
end

# ── Flags ────────────────────────────────────────────────────────────────────
complete -c vv -c vh -s h -l help          -d 'Show help'
complete -c vv -c vh -s V -l version       -d 'Print version'
complete -c vv -c vh -s i -l interactive   -d 'Open ncurses row browser'
complete -c vv -c vh -l no-interactive     -d 'Force plain table output'
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
complete -c vv -c vh -l region-cols -r     -d 'chrom,start,end column names for plain Parquet'
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

# Parquet output
complete -c vv -c vh -l parquet -r -F      -d 'Write a Parquet file at this path'
complete -c vv -c vh -l compression -r -a 'zstd snappy gzip lz4 none' -d 'Parquet codec'

# JSON output / projection / filter / describe / schema
complete -c vv -c vh -l json               -d 'Write JSON array of row objects'
complete -c vv -c vh -l ndjson             -d 'Write one JSON object per line'
complete -c vv -c vh -l md                 -d 'Write GitHub-flavored markdown table'
complete -c vv -c vh -l markdown           -d 'Alias of --md'
complete -c vv -c vh -l validate           -d 'Check LociSSD invariants and exit'
complete -c vv -c vh -l select -r          -d 'Project columns by name (comma-separated)'
complete -c vv -c vh -l cols -r            -d 'Alias of --select'
complete -c vv -c vh -l filter -r          -d 'Row predicate: <col> <op> <value> [AND/OR ...]'
complete -c vv -c vh -l schema             -d 'Print schema + metadata and exit'
complete -c vv -c vh -l describe           -d 'Per-column statistics and exit'
complete -c vv -c vh -l stats              -d 'Parquet metadata dump (no data read)'
complete -c vv -c vh -l unique -r          -d 'Distinct value counts per column'
complete -c vv -c vh -l sample -r          -d 'Reservoir-sample N random rows'
