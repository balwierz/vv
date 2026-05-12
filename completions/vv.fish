# Fish tab completion for vv (and the vh symlink)
# Install: cp vv.fish ~/.config/fish/completions/

# Disable default file completion so we can control extensions
complete -c vv -c vh -F

# ── File arguments ───────────────────────────────────────────────────────────
for ext in parquet arrow feather lociss bam cram sam vcf vcf.gz bcf gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz tsv tsv.gz csv csv.gz fa fa.gz fasta fasta.gz fna fna.gz faa faa.gz ffn ffn.gz frn frn.gz fq fq.gz fastq fastq.gz paf paf.gz
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

# Region (tabix)
complete -c vv -c vh -s r -l region -r     -d 'Region for tabix-indexed file (e.g. chr1:1000-2000)'

# Performance
complete -c vv -c vh -s @ -l threads -r    -d 'Worker threads for I/O and decode (0 = auto)'

# --color
complete -c vv -c vh -l color              -d 'Colorize output (auto/always/never)'
complete -c vv -c vh -l color -r -a 'auto always never' -d 'Color mode'

# Delimited output
complete -c vv -c vh -l tsv                -d 'Write tab-separated values'
complete -c vv -c vh -l csv                -d 'Write comma-separated values'
complete -c vv -c vh -l delimiter -r       -d 'Write with a custom single-character delimiter'
complete -c vv -c vh -l no-header          -d 'Omit the header row in delimited output'

# Parquet output
complete -c vv -c vh -l parquet -r -F      -d 'Write a Parquet file at this path'
complete -c vv -c vh -l compression -r -a 'zstd snappy gzip lz4 none' -d 'Parquet codec'
