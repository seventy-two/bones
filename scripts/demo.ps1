# Runs bones on mock data, for README screenshots. Nothing of yours shows:
# made-up drives (BONES_DEMO), its own state and cache (BONES_DATA), and
# a "Media" drive that's a folder of mock files mapped to M: with subst.
#
#   1. python3 scripts/make-demo.py <dir>\media   (WSL or any Python with Pillow + ffmpeg)
#   2. powershell -File scripts\demo.ps1 -Dir <dir> [-Open M:\Photos] [-View thumbs|details|sizes]
#   3. subst M: /d when done.
#
# The big files listed in media\sparse.txt (films, games, backups) are made
# sparse: they report their full size (~1 TB in all) but take no space.
param(
    [Parameter(Mandatory)][string]$Dir,
    [string]$Open = "",
    [string]$View = "thumbs",
    [string]$Sort = "",
    [string]$Pos = "100,100,1400,860",
    [string]$Exe = "",
    [string]$Letter = "M:"
)
$ErrorActionPreference = 'Stop'
# PowerShell 5 has no $PSScriptRoot in parameter defaults.
if (-not $Exe) { $Exe = Join-Path (Split-Path $PSScriptRoot) 'build\bones.exe' }
$media = Join-Path $Dir 'media'

# Sparse files, once.
$list = Join-Path $media 'sparse.txt'
if (Test-Path $list) {
    foreach ($line in Get-Content $list) {
        if (-not $line) { continue }
        $rel, $size = $line -split "`t"
        $path = Join-Path $media $rel
        if ((Test-Path $path) -and (Get-Item $path).Length -eq [int64]$size) { continue }
        New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
        New-Item -ItemType File -Force $path | Out-Null
        fsutil sparse setflag $path | Out-Null
        $f = [IO.File]::Open($path, 'Open', 'ReadWrite')
        try { $f.SetLength([int64]$size) } finally { $f.Close() }
    }
}

# The drive letter.
$mapped = (subst) -match "^$Letter\\"
if (-not $mapped) { subst $Letter $media }
$root = "$Letter\"

# State: pins and most used folders on the mock drives.
$data = Join-Path $Dir 'data'
New-Item -ItemType Directory -Force $data | Out-Null
$now = [int][double]::Parse((Get-Date -UFormat %s))
$state = [ordered]@{
    view = 'thumbs'; tile = 170; autoView = $false
    pins = @(
        @{ label = 'Desktop'; path = "${root}Desktop" },
        @{ label = 'Downloads'; path = "${root}Downloads" },
        @{ label = 'Documents'; path = "${root}Documents" },
        @{ label = 'Videos'; path = "${root}Videos" }
    )
    uses = @(
        @{ path = "${root}Photos\Iceland 2025"; count = 14; last = $now },
        @{ path = "${root}Films"; count = 9; last = $now },
        @{ path = "${root}Projects\bones"; count = 6; last = $now },
        @{ path = 'C:\Users\me\Downloads'; count = 11; last = $now },
        @{ path = 'C:\Users\me\Documents\Taxes'; count = 4; last = $now },
        @{ path = 'G:\SteamLibrary\steamapps\common'; count = 5; last = $now },
        @{ path = '\\wsl.localhost\Debian\root\src\bones'; count = 8; last = $now },
        @{ path = '\\wsl.localhost\Debian\root\notes'; count = 3; last = $now }
    )
}
[IO.File]::WriteAllText((Join-Path $data 'state.json'), ($state | ConvertTo-Json -Depth 5))

Get-Process bones -ErrorAction SilentlyContinue | ? { $_.Path -eq (Resolve-Path $Exe).Path } | Stop-Process -Force
$env:BONES_DEMO = $root
$env:BONES_DATA = $data
$env:BONES_POS = $Pos
$env:BONES_VIEW = $View
if ($Sort) { $env:BONES_SORT = $Sort }
$argList = if ($Open) { "`"$Open`"" } else { '""' }
Start-Process (Resolve-Path $Exe) -ArgumentList $argList
