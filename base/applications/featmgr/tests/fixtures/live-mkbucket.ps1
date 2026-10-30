# live-mkbucket.ps1 old|new
# Test helper for scoop-live.cmd: a local Scoop bucket "fmlive" with one app, fm7z, that is 7-Zip under another
# name (its shim is "fm7z", so it cannot collide with the real 7zip app).
#   old = 7-Zip 24.09 (real download, pinned hash)
#   new = whatever main's 7zip manifest has now (a newer 7-Zip, real download, hash from the manifest)
# Writing "new" over "old" is what "scoop update" does when it pulls a bucket that has a newer manifest.
param([string]$Which)
$root = $env:SCOOP; if (-not $root) { $root = 'C:\scoop' }
$m = Get-Content -Raw (Join-Path $root 'buckets\main\bucket\7zip.json') | ConvertFrom-Json
$a64 = $m.architecture.'64bit'
if ($Which -eq 'old') {
    $ver = '24.09'
    $a64 = [pscustomobject]@{ url = 'https://www.7-zip.org/a/7z2409-x64.msi'; hash = 'ec6af1ea0367d16dde6639a89a080a524cebc4d4bedfe00ed0cac4b865a918d8' }
    if ($m.architecture.'64bit'.extract_dir) { $a64 | Add-Member extract_dir $m.architecture.'64bit'.extract_dir }
} else {
    $ver = $m.version
}
$o = [ordered]@{
    version = $ver
    description = "7-Zip $ver under another name (featmgr live update test)"
    homepage = $m.homepage
    license = $m.license
    architecture = @{ '64bit' = $a64 }
    bin = @(, @('7z.exe', 'fm7z'))
}
if ($m.extract_dir) { $o['extract_dir'] = $m.extract_dir }
$dir = Join-Path $root 'buckets\fmlive\bucket'
New-Item -ItemType Directory -Force $dir | Out-Null
$json = $o | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText((Join-Path $dir 'fm7z.json'), $json, (New-Object System.Text.UTF8Encoding($false)))
"live-mkbucket: fm7z $ver written to $dir"
