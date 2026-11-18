# Install the latest mano release on Windows (x64 or ARM64).
#   irm https://github.com/phdgamer0/mano/releases/latest/download/install.ps1 | iex
# Env: MANO_VERSION (default: latest release), MANO_INSTALL_DIR.
$ErrorActionPreference = 'Stop'
$repo = 'phdgamer0/mano'

$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x86_64' }
$tag = $env:MANO_VERSION
if (-not $tag) { $tag = (Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest").tag_name }
if (-not $tag.StartsWith('v')) { $tag = "v$tag" }
$name = "mano-$($tag.Substring(1))-windows-$arch"
$base = "https://github.com/$repo/releases/download/$tag"
$dest = if ($env:MANO_INSTALL_DIR) { $env:MANO_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\mano' }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Write-Host "downloading $name.zip"
    Invoke-WebRequest "$base/$name.zip" -OutFile "$tmp\$name.zip" -UseBasicParsing
    Invoke-WebRequest "$base/SHA256SUMS" -OutFile "$tmp\SHA256SUMS" -UseBasicParsing
    $want = (Get-Content "$tmp\SHA256SUMS" | Where-Object { $_ -match " $([regex]::Escape("$name.zip"))$" }) -split ' ' | Select-Object -First 1
    $got = (Get-FileHash "$tmp\$name.zip" -Algorithm SHA256).Hash.ToLower()
    if (-not $want -or $want -ne $got) { throw 'mano: checksum mismatch' }

    Expand-Archive "$tmp\$name.zip" -DestinationPath $tmp
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item "$tmp\$name\*" $dest -Recurse -Force
} finally {
    Remove-Item $tmp -Recurse -Force
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $dest) {
    [Environment]::SetEnvironmentVariable('Path', "$userPath;$dest", 'User')
    Write-Host "added $dest to your user PATH (restart the terminal)"
}
Write-Host "installed $dest\mano.exe ($tag)"
