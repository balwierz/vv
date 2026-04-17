# Fish tab completion for parquet_viewer
# Install: cp parquet_viewer.fish ~/.config/fish/completions/

set -l cmd parquet_viewer

# Disable default file completion so we can control extensions
complete -c $cmd -F

# ── File arguments ───────────────────────────────────────────────────────────
for ext in parquet arrow feather bam cram sam vcf vcf.gz gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz tsv tsv.gz csv csv.gz
    complete -c $cmd -F -a "*.$ext"
end

# ── Flags ────────────────────────────────────────────────────────────────────
complete -c $cmd -s h -l help          -d 'Show help'
complete -c $cmd -s i -l interactive   -d 'Open ncurses row browser'
complete -c $cmd -l no-interactive     -d 'Force plain table output'

# Table options
complete -c $cmd -s n -r              -d 'Rows to display (default: 10, 0 = all)'
complete -c $cmd -s w -r              -d 'Max cell width (default: 32)'
complete -c $cmd -s c -r              -d 'Max columns to show (default: all)'
complete -c $cmd -l no-index          -d 'Suppress the row-index column'

# --color
complete -c $cmd -l color             -d 'Colorize output (auto/always/never)'
complete -c $cmd -l color -r -a 'auto always never' -d 'Color mode'

# Delimited output
complete -c $cmd -l tsv               -d 'Write tab-separated values'
complete -c $cmd -l csv               -d 'Write comma-separated values'
complete -c $cmd -l delimiter -r      -d 'Write with a custom single-character delimiter'
complete -c $cmd -l no-header         -d 'Omit the header row in delimited output'
