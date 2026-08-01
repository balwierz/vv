# Security policy

## Supported versions

Only the latest minor release receives security fixes.

## Reporting a vulnerability

Please **do not** file public GitHub issues for security vulnerabilities.

Use GitHub's private vulnerability reporting feature:
<https://github.com/balwierz/vv/security/advisories/new>

Alternatively, email the maintainer directly (see commit log for the
contact address). Expect an initial response within 7 days.

## Scope

`vv` is a read-only command-line viewer; it does not write or modify any
input files. The classes of issues we care about most:

- Memory-safety bugs (buffer overflow, UAF) when parsing untrusted input
  files (Parquet, BAM/CRAM, VCF/BED/GFF/SAM, FASTA/FASTQ, CSV/TSV).
- Crashes triggered by malformed inputs that should be rejected gracefully.
- Excessive resource use (memory, CPU) on inputs of bounded size.

Most parsing is delegated to upstream libraries (Apache Arrow, htslib).
Issues that originate in those libraries should be reported upstream;
we will coordinate where useful.
