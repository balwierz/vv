# Bash tab completion for vv
# Source this file or install to /etc/bash_completion.d/vv

# Bounded vv invocation for introspecting the file on the command line
# (column / tab names). A large or slow file must never hang the prompt, so cap
# it with timeout; errors are silent, so an empty result just yields no
# candidates. Uses the actually-invoked command (vv or the vh symlink).
_vv_run() {
    local vv=${words[0]:-vv}
    if command -v timeout >/dev/null 2>&1; then
        timeout 1s "$vv" "$@" 2>/dev/null
    elif command -v gtimeout >/dev/null 2>&1; then
        gtimeout 1s "$vv" "$@" 2>/dev/null
    else
        "$vv" "$@" 2>/dev/null
    fi
}

# The input file already present on the command line: the sole positional
# argument. Skip options and the argument of any value-taking option so an
# output path (e.g. --parquet out.parquet) is not mistaken for the input.
_vv_input_file() {
    local i tok
    local val_opts=' -n -w -c -@ --threads --decode-threads --delimiter --in-delimiter -d --header --color
        --theme --box -r --region --window --regions-file --region-cols --slop --coords
        --tail --sort --tags --expand --parquet --arrow --feather --compression --image-mode
        --samples --matrix -f --fasta --select --cols --filter --tab --unique --sample
        --exclude-flags --ff --require-flags --rf --min-mapq --min-bq '
    for (( i = 1; i < ${#words[@]}; i++ )); do
        tok=${words[i]}
        (( i == cword )) && continue          # the word being completed
        if [[ $tok == -* ]]; then
            [[ $val_opts == *" $tok "* ]] && (( i++ ))   # skip its value
            continue
        fi
        [[ -f $tok ]] && { printf '%s' "$tok"; return; }
    done
}

# Offer the input file's column names for the current segment. $1 = delimiter
# that separates items in this flag ("," for column lists, " " for --filter,
# empty for a single value); a leading '!' (exclusion) is preserved.
_vv_complete_columns() {
    local delim=$1 file cols pre seg bang='' c
    file=$(_vv_input_file)
    [[ -n $file ]] || return
    cols=$(_vv_run --list-columns "$file")
    [[ -n $cols ]] || return
    pre='' ; seg=$cur
    if [[ -n $delim && $cur == *"$delim"* ]]; then
        pre=${cur%"$delim"*}$delim
        seg=${cur##*"$delim"}
    fi
    [[ $seg == '!'* ]] && { bang='!'; seg=${seg#!}; }
    COMPREPLY=()
    while IFS= read -r c; do
        [[ -n $c && $c == "$seg"* ]] && COMPREPLY+=( "${pre}${bang}${c}" )
    done <<< "$cols"
}

_vv() {
    local cur prev words cword
    _init_completion || return

    local file_exts='parquet arrow arrows feather lociss bam cram sam vcf vcf.gz vcf.bz2 vcf.xz vcf.zst bcf gff gff.gz gff.bz2 gff.xz gff.zst gff3 gff3.gz gff3.bz2 gff3.xz gff3.zst gtf gtf.gz gtf.bz2 gtf.xz gtf.zst bed bed.gz bed.bz2 bed.xz bed.zst narrowPeak narrowPeak.gz narrowPeak.bz2 narrowPeak.xz narrowPeak.zst broadPeak broadPeak.gz broadPeak.bz2 broadPeak.xz broadPeak.zst gappedPeak gappedPeak.gz gappedPeak.bz2 gappedPeak.xz gappedPeak.zst bedGraph bedGraph.gz bedGraph.bz2 bedGraph.xz bedGraph.zst bg bg.gz bg.bz2 bg.xz bg.zst tagAlign tagAlign.gz tagAlign.bz2 tagAlign.xz tagAlign.zst tsv tsv.gz tsv.bz2 tsv.xz tsv.zst csv csv.gz csv.bz2 csv.xz csv.zst fa fa.gz fa.zst fa.bz2 fa.xz fasta fasta.gz fasta.zst fasta.bz2 fasta.xz fna fna.gz fna.zst fna.bz2 fna.xz faa faa.gz faa.zst faa.bz2 faa.xz ffn ffn.gz ffn.zst ffn.bz2 ffn.xz frn frn.gz frn.zst frn.bz2 frn.xz fq fq.gz fq.zst fq.bz2 fq.xz fastq fastq.gz fastq.zst fastq.bz2 fastq.xz paf paf.gz paf.bz2 paf.xz paf.zst mtx mtx.gz mtx.bz2 mtx.xz mtx.zst bedpe bedpe.gz bedpe.bz2 bedpe.xz bedpe.zst pairs pairs.gz pairs.bz2 pairs.xz pairs.zst gct gct.gz gct.bz2 gct.xz gct.zst maf maf.gz maf.bz2 maf.xz maf.zst bim bim.gz bim.bz2 bim.xz bim.zst fam fam.gz fam.bz2 fam.xz fam.zst pvar pvar.gz pvar.bz2 pvar.xz pvar.zst psam psam.gz psam.bz2 psam.xz psam.zst json json.gz json.bz2 json.xz json.zst ndjson ndjson.gz ndjson.bz2 ndjson.xz ndjson.zst jsonl jsonl.gz jsonl.bz2 jsonl.xz jsonl.zst geojson geojson.gz geojson.bz2 geojson.xz geojson.zst ipynb ipynb.gz ipynb.bz2 ipynb.xz ipynb.zst har har.gz har.bz2 har.xz har.zst bb bigBed bigbed bw bigWig bigwig 2bit sqlite sqlite3 db xlsx xlsm ods fods orc npz npy pileup pileup.gz pileup.bz2 pileup.xz pileup.zst mpileup mpileup.gz mpileup.bz2 mpileup.xz mpileup.zst pile pile.gz pile.bz2 pile.xz pile.zst md markdown mdown mkd h5 h5ad hdf5 loom txt txt.gz txt.bz2 txt.xz txt.zst text text.gz text.bz2 text.xz text.zst log log.gz log.bz2 log.xz log.zst m8 m8.gz m8.zst m8.bz2 m8.xz blast6 blast6.gz blast6.zst blast6.bz2 blast6.xz outfmt6 outfmt6.gz outfmt6.zst outfmt6.bz2 outfmt6.xz h5mu h5seurat fast5 nc nc4 wig wig.gz wig.zst wig.bz2 wig.xz rds RData rda'

    case "$prev" in
        -n|-w|-c|-@|--threads|--decode-threads)
            # Numeric argument — no completion
            return
            ;;
        --delimiter)
            # Single-character delimiter — no completion
            return
            ;;
        -d|--in-delimiter)
            COMPREPLY=( $(compgen -W 'tab space comma semicolon pipe' -- "$cur") )
            return
            ;;
        --header)
            COMPREPLY=( $(compgen -W 'auto on off' -- "$cur") )
            return
            ;;
        --color)
            COMPREPLY=( $(compgen -W 'auto always never' -- "$cur") )
            return
            ;;
        --theme)
            COMPREPLY=( $(compgen -W 'default dark light solarized-dark solarized-light' -- "$cur") )
            return
            ;;
        --box)
            COMPREPLY=( $(compgen -W 'unicode ascii' -- "$cur") )
            return
            ;;
        -r|--region|--window)
            # Free-form region string — no completion
            return
            ;;
        --regions-file)
            _filedir bed
            return
            ;;
        # Column-name lists: complete the current comma-segment from the file's
        # actual columns. -o nospace so the user can keep appending with a comma.
        --select|--cols|--region-cols)
            _vv_complete_columns ','
            (( ${#COMPREPLY[@]} )) && compopt -o nospace 2>/dev/null
            return
            ;;
        --expand)
            # A single packed column name (e.g. INFO, attributes).
            _vv_complete_columns ''
            return
            ;;
        --sort)
            # A single column name (an optional :asc/:desc the user adds).
            _vv_complete_columns ''
            return
            ;;
        --filter)
            # Complete a column name at a token boundary; the rest of the
            # predicate grammar (operators, values) is free-form.
            _vv_complete_columns ' '
            return
            ;;
        --tab)
            local _f
            _f=$(_vv_input_file)
            [[ -n $_f ]] && \
                COMPREPLY=( $(compgen -W "$(_vv_run --list-tabs "$_f")" -- "$cur") )
            return
            ;;
        --exclude-flags|--ff|--require-flags|--rf)
            # A comma-separated SAM flag list; complete the current member.
            local pre='' seg=$cur
            [[ $cur == *,* ]] && { pre=${cur%,*},; seg=${cur##*,}; }
            COMPREPLY=( $(compgen -P "$pre" -W 'PAIRED PROPER_PAIR UNMAP MUNMAP REVERSE MREVERSE READ1 READ2 SECONDARY QCFAIL DUP SUPPLEMENTARY' -- "$seg") )
            (( ${#COMPREPLY[@]} )) && compopt -o nospace 2>/dev/null
            return
            ;;
        --min-mapq|--min-bq)
            return
            ;;
        --slop|--sample|--tail|--unique)
            # Numeric / free-form argument — no completion
            return
            ;;
        --coords)
            COMPREPLY=( $(compgen -W 'UCSC Kent NCBI GenBank 0-based 1-based bed tabix' -- "$cur") )
            return
            ;;
        --parquet|--arrow|--feather)
            _filedir
            return
            ;;
        -f|--fasta)
            _filedir 'fa|fasta|fa.gz|fna'
            return
            ;;
        --compression)
            COMPREPLY=( $(compgen -W 'zstd snappy gzip lz4 none' -- "$cur") )
            return
            ;;
        --image-mode)
            COMPREPLY=( $(compgen -W 'auto kitty iterm sixel halfblock ascii' -- "$cur") )
            return
            ;;
        --samples)
            COMPREPLY=( $(compgen -W 'struct long text' -- "$cur") )
            return
            ;;
        --matrix)
            COMPREPLY=( $(compgen -W 'wide long' -- "$cur") )
            return
            ;;
    esac

    case "$cur" in
        --color=*)
            COMPREPLY=( $(compgen -W '--color=auto --color=always --color=never' -- "$cur") )
            return
            ;;
        -*)
            local opts='
                -h --help -V --version
                -i --interactive --no-interactive --table -t
                -n -w -c
                -r --region --window --regions-file --region-cols --slop --coords
                --tail
                -@ --threads --decode-threads
                --no-index
                --color --color=auto --color=always --color=never
                --theme
                --tsv --csv --delimiter
                --parquet --arrow --feather --compression
                --json --ndjson --md --markdown
                --select --cols --filter
                --schema --describe --count --stats --contigs --seq-stats --flatten --tree --pretty --json-paths --no-tree --gt-stats --validate --decode-pileup --pileup --text
                --exclude-flags --ff --require-flags --rf --min-mapq --min-bq --count-orphans --ignore-overlaps
                --in-delimiter --header
                --expand --formats --list-columns --list-tabs
                -f --fasta
                --tab
                --unique --distinct --sample --sort --tags
                --box
                --vertical
                --no-header
                --heatmap --image-mode
                --samples --matrix
            '
            COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
            return
            ;;
    esac

    # Default: complete filenames with supported extensions
    local exts_pattern
    exts_pattern=$(printf '@(%s)' "$(echo $file_exts | tr ' ' '|')")
    _filedir "$exts_pattern"
}

complete -F _vv vv
complete -F _vv vh
