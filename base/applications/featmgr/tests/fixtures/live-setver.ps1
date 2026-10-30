# live-setver.ps1 <index-file> <app> <version>
# Test helper for scoop-live.cmd: set the catalog version of one app in a featmgr Scoop index file
# (UTF-8, tab separated: id, app, version, ...), so that the installed copy looks outdated.
param([string]$Idx, [string]$App, [string]$Ver)
$enc = New-Object System.Text.UTF8Encoding($false)
$lines = [System.IO.File]::ReadAllLines($Idx, $enc)
$n = 0
for ($i = 0; $i -lt $lines.Length; $i++) {
    $f = $lines[$i].Split("`t")
    if ($f.Length -ge 7 -and $f[1] -eq $App -and -not $lines[$i].StartsWith('#')) {
        $f[2] = $Ver
        $lines[$i] = [string]::Join("`t", $f)
        $n++
    }
}
[System.IO.File]::WriteAllLines($Idx, $lines, $enc)
"live-setver: $n line(s) changed in $Idx"
