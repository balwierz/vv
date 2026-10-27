# Run vv and vvg from an installed or unzipped folder as a user would: with
# only Windows itself on PATH, so a DLL missing from the package cannot be
# found elsewhere on the machine. Run from the repository root (the test
# files are tests\data\*). Used by .github/actions/windows-build for the MSI
# (C:\Program Files\vv) and the portable zip (its unzipped folder).
param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'

function Expect($what, $out, $want) {
    if (-not ("$out" -match $want)) { throw "${what}: got '$out', want /$want/" }
    Write-Output "ok  $what"
}

$savedPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    Expect 'vv --version' (& "$Dir\vv.exe" --version) '^vv \d+\.\d+\.\d+$'
    Expect 'vv --count' (& "$Dir\vv.exe" --count tests\data\tiny.parquet) '^20$'
    Expect 'vv HDF5' (& "$Dir\vv.exe" --count --tab obs tests\data\tiny.h5ad) '^\d+$'
    Expect 'vv BAM' (& "$Dir\vv.exe" --count tests\data\tiny.bam) '^\d+$'
    # A non-ASCII file name (the UTF-8 code page from the manifest).
    $u = Join-Path $env:RUNNER_TEMP "t$([char]0xEB)st_$([char]0x65E5)$([char]0x672C).parquet"
    Copy-Item tests\data\tiny.parquet $u -Force
    Expect 'vv on a UTF-8 file name' (& "$Dir\vv.exe" --count $u) '^20$'
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:VVG_SELFTEST = '1'
    # vvg is a GUI-subsystem program: piping its output makes pwsh wait.
    $o = (& "$Dir\vvg.exe" tests\data\tiny.parquet | Out-String)
    Write-Output $o
    Expect 'vvg self-test' $o 'icon=ok'
    Expect 'vvg reads the file' $o 'tabs=1 rows=20 cols=5'
    $o = (& "$Dir\vvg.exe" $u | Out-String)
    Expect 'vvg on a UTF-8 file name' $o 'tabs=1 rows=20 cols=5'
} finally {
    Remove-Item Env:VVG_SELFTEST -ErrorAction SilentlyContinue
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    $env:PATH = $savedPath
}
