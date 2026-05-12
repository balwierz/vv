# Bash tab completion for vv
# Source this file or install to /etc/bash_completion.d/vv

_vv() {
    local cur prev words cword
    _init_completion || return

    local file_exts='parquet arrow feather lociss bam cram sam vcf vcf.gz bcf gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz tsv tsv.gz csv csv.gz fa fa.gz fasta fasta.gz fna fna.gz faa faa.gz ffn ffn.gz frn frn.gz fq fq.gz fastq fastq.gz paf paf.gz'

    case "$prev" in
        -n|-w|-c|-@|--threads)
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
        -r|--region)
            # Free-form region string — no completion
            return
            ;;
        --parquet)
            _filedir
            return
            ;;
        --compression)
            COMPREPLY=( $(compgen -W 'zstd snappy gzip lz4 none' -- "$cur") )
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
                -i --interactive --no-interactive
                -n -w -c
                -r --region
                -@ --threads
                --no-index
                --color --color=auto --color=always --color=never
                --tsv --csv --delimiter
                --parquet --compression
                --json --ndjson
                --select --cols --filter
                --schema --describe
                --vertical
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

complete -F _vv vv
complete -F _vv vh
