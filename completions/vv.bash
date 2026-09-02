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
    local val_opts=' -n -w -c -@ --threads --decode-threads --delimiter --color
        --theme --box -r --region --window --regions-file --region-cols --slop --coords
        --tail --sort --tags --expand --parquet --arrow --feather --compression --image-mode
        -f --fasta --select --cols --filter --tab --unique --sample '
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

    local file_exts='parquet arrow feather lociss bam cram sam vcf vcf.gz vcf.zst bcf gff gff.gz gff.zst gff3 gff3.gz gff3.zst gtf gtf.gz gtf.zst bed bed.gz bed.zst narrowPeak narrowPeak.gz narrowPeak.zst broadPeak broadPeak.gz broadPeak.zst gappedPeak gappedPeak.gz gappedPeak.zst bedGraph bedGraph.gz bedGraph.zst bg bg.gz bg.zst tagAlign tagAlign.gz tagAlign.zst tsv tsv.gz tsv.zst csv csv.gz csv.zst fa fa.gz fasta fasta.gz fna fna.gz faa faa.gz ffn ffn.gz frn frn.gz fq fq.gz fastq fastq.gz paf paf.gz paf.zst json json.gz json.zst ndjson ndjson.gz ndjson.zst jsonl jsonl.gz jsonl.zst bb bigBed bigbed bw bigWig bigwig 2bit sqlite sqlite3 db xlsx xlsm ods fods orc npz npy pileup pileup.gz pileup.zst mpileup mpileup.gz mpileup.zst pile pile.gz pile.zst md markdown mdown mkd h5 h5ad hdf5 loom txt txt.gz txt.zst text text.gz text.zst log log.gz log.zst'

    case "$prev" in
        -n|-w|-c|-@|--threads|--decode-threads)
            # Numeric argument — no completion
            return
            ;;
        --delimiter)
            # Single-character delimiter — no completion
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
            COMPREPLY=( $(compgen -W 'auto kitty sixel halfblock ascii' -- "$cur") )
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
                --schema --describe --count --stats --contigs --gt-stats --validate --decode-pileup --pileup --text
                --expand --formats --list-columns --list-tabs
                -f --fasta
                --tab
                --unique --distinct --sample --sort --tags
                --box
                --vertical
                --no-header
                --heatmap --image-mode
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
