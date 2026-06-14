# Bash tab completion for vv
# Source this file or install to /etc/bash_completion.d/vv

_vv() {
    local cur prev words cword
    _init_completion || return

    local file_exts='parquet arrow feather lociss bam cram sam vcf vcf.gz bcf gff gff.gz gff3 gff3.gz gtf gtf.gz bed bed.gz narrowPeak narrowPeak.gz broadPeak broadPeak.gz gappedPeak gappedPeak.gz bedGraph bedGraph.gz bg bg.gz tagAlign tagAlign.gz tsv tsv.gz csv csv.gz fa fa.gz fasta fasta.gz fna fna.gz faa faa.gz ffn ffn.gz frn frn.gz fq fq.gz fastq fastq.gz paf paf.gz bb bigBed bigbed bw bigWig bigwig 2bit sqlite sqlite3 db xlsx xlsm ods orc pileup pileup.gz mpileup mpileup.gz pile pile.gz md markdown mdown mkd h5 h5ad hdf5 loom'

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
        -r|--region|--window)
            # Free-form region string — no completion
            return
            ;;
        --regions-file)
            _filedir bed
            return
            ;;
        --region-cols)
            # Comma-separated column names — no completion
            return
            ;;
        --slop|--sample|--tail)
            # Numeric argument — no completion
            return
            ;;
        --coords)
            COMPREPLY=( $(compgen -W 'UCSC Kent NCBI GenBank 0-based 1-based bed tabix' -- "$cur") )
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
                -i --interactive --no-interactive
                -n -w -c
                -r --region --window --regions-file --region-cols --slop --coords
                --tail
                -@ --threads --decode-threads
                --no-index
                --color --color=auto --color=always --color=never
                --theme
                --tsv --csv --delimiter
                --parquet --compression
                --json --ndjson --md --markdown
                --select --cols --filter
                --schema --describe --stats --validate --decode-pileup --pileup
                --unique --sample
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
