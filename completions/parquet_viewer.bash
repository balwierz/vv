# Bash tab completion for parquet_viewer
# Source this file or install to /etc/bash_completion.d/parquet_viewer

_parquet_viewer() {
    local cur prev words cword
    _init_completion || return

    local file_exts='parquet arrow feather bam cram sam vcf vcf.gz gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz tsv tsv.gz csv csv.gz'

    case "$prev" in
        -n|-w|-c)
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
    esac

    case "$cur" in
        --color=*)
            COMPREPLY=( $(compgen -W '--color=auto --color=always --color=never' -- "$cur") )
            return
            ;;
        -*)
            local opts='
                -h --help
                -i --interactive --no-interactive
                -n -w -c
                --no-index
                --color --color=auto --color=always --color=never
                --tsv --csv --delimiter
                --no-header
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

complete -F _parquet_viewer parquet_viewer
