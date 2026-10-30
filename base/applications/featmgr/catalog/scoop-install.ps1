<#
.SYNOPSIS
    featmgr "scoop" feature: install Scoop headless, machine-wide (SCOOP-FEATMGR-01).
.DESCRIPTION
    Run by featmgr with PowerShell 7 (pwsh -NoProfile -NonInteractive -ExecutionPolicy Bypass -File).
    1. Adopts an existing Scoop in the root (exit 0), refuses a non-empty foreign directory.
    2. Downloads the upstream installer from a COMMIT-PINNED URL (not the moving get.scoop.sh) and
       refuses to run it unless its SHA-256 equals $InstallerSha256.
    3. Runs it with -ScoopDir <root> -RunAsAdmin (nitro's account is an administrator).
    4. Makes Scoop machine-wide: machine SCOOP variable + <root>\shims on the machine PATH.
    Exit 0 = Scoop is installed and usable; anything else = failure (message on stdout).

    What the pin covers: this script. The pinned installer itself downloads Scoop and the Main bucket
    as .../archive/master.zip (its own "TODO: Use a specific version"), or git-clones them if git is
    on PATH; those cannot be hashed from outside. Every app download is later verified by Scoop
    against the hash in its manifest.

    Bumping the pin (manual, reviewed, then re-run tests\scoop-live.cmd):
      git ls-remote https://github.com/ScoopInstaller/Install master        -> new commit
      curl -L https://raw.githubusercontent.com/ScoopInstaller/Install/<commit>/install.ps1 | sha256sum
    and change $InstallerCommit / $InstallerSha256 here and Artifact / ArtifactSHA256 in scoop.ini.
#>
param([string] $Root)

$ErrorActionPreference = 'Stop'
$InstallerCommit = '1e2f334083d609986d8c8bc9e31ae8e87c39fab4'
$InstallerSha256 = '94f983b190438311e006b957db7c8422709e0ba62a6c2ac04e278164108f2512'
$InstallerUrl    = "https://raw.githubusercontent.com/ScoopInstaller/Install/$InstallerCommit/install.ps1"

function Say($m) { Write-Output ("scoop-install: " + $m) }
function Die($m) { Write-Output ("scoop-install: ERROR: " + $m); exit 1 }

try {
    if (-not $Root) {
        $Root = [Environment]::GetEnvironmentVariable('SCOOP', 'Machine')
        if (-not $Root) { $Root = Join-Path $env:SystemDrive 'scoop' }
    }
    $shim = Join-Path $Root 'shims\scoop.cmd'
    $core = Join-Path $Root 'apps\scoop\current\bin\scoop.ps1'
    $env:SCOOP = $Root

    if (Test-Path $core) {
        Say "Scoop already present in $Root; adopting it"
    } else {
        if ((Test-Path $Root) -and (Get-ChildItem -Force $Root -ErrorAction SilentlyContinue)) {
            Die "$Root exists and is not empty (and holds no Scoop); move it away first"
        }
        # --- fetch + verify the pinned installer
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ("scoop-install-" + [Guid]::NewGuid().ToString('N') + '.ps1')
        $ok = $false
        for ($try = 1; $try -le 3 -and -not $ok; $try++) {
            try {
                Say "downloading $InstallerUrl (attempt $try)"
                Invoke-WebRequest -Uri $InstallerUrl -UseBasicParsing -OutFile $tmp -TimeoutSec 120
                $ok = $true
            } catch {
                Say ("download failed: " + $_.Exception.Message)
                if ($try -lt 3) { Start-Sleep -Seconds 3 }
            }
        }
        if (-not $ok) { Die "cannot download the Scoop installer (check the network connection): $InstallerUrl" }
        $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash.ToLowerInvariant()
        if ($got -ne $InstallerSha256) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            Die "SHA-256 of the downloaded installer is $got, expected $InstallerSha256; refusing to run it"
        }
        Say "installer SHA-256 verified; running it (root $Root)"
        $global:LASTEXITCODE = 0
        try {
            & $tmp -ScoopDir $Root -RunAsAdmin
        } finally {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        }
        if ($LASTEXITCODE -ne 0) { Die "the Scoop installer exited with code $LASTEXITCODE" }
    }

    if (-not (Test-Path $shim)) { Die "Scoop installer finished but $shim is missing" }

    # --- machine-wide: SCOOP + shims on the machine PATH (the installer only touched the user's)
    [Environment]::SetEnvironmentVariable('SCOOP', $Root, 'Machine')
    $shims = Join-Path $Root 'shims'
    $mp = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (-not (($mp -split ';') -contains $shims)) {
        [Environment]::SetEnvironmentVariable('Path', ($shims + ';' + $mp), 'Machine')
    }
    Say "Scoop is installed in $Root"
    exit 0
} catch {
    Die ($_.Exception.Message)
}
