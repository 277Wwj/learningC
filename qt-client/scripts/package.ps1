[CmdletBinding()]
param([string]$QtPath = $env:QT_ROOT)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$clientRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $clientRoot '..'))
if ([string]::IsNullOrWhiteSpace($QtPath)) {
    $QtPath = Join-Path (Split-Path -Parent $repoRoot) '.qt\6.8.3\msvc2022_64'
}
$QtPath = (Resolve-Path -LiteralPath $QtPath -ErrorAction Stop).Path
$deployTool = Join-Path $QtPath 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployTool)) { throw "windeployqt was not found: $deployTool" }

# Always rebuild and run all four test suites before preparing a distributable.
& (Join-Path $PSScriptRoot 'build.ps1') -QtPath $QtPath -Configuration Release
if (-not $?) { throw 'Build or tests failed; packaging stopped.' }
$appPath = Join-Path $repoRoot 'build-qt\Release\learningc_studio.exe'
if (-not (Test-Path -LiteralPath $appPath -PathType Leaf)) { throw "Application not found: $appPath" }

$distRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
$packageRoot = Join-Path $distRoot 'LearningCStudio'
$zipPath = Join-Path $distRoot 'LearningCStudio-windows-x64.zip'
$stageRoot = Join-Path $distRoot ('.LearningCStudio-stage-' + [Guid]::NewGuid().ToString('N'))
$stageZip = "$stageRoot.zip"

function Assert-SafeDistPath {
    param([string]$Path)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $prefix = $distRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing filesystem operation outside the package directory: $fullPath"
    }
    # Do not follow a redirected dist directory or a junction inside a previous package.
    if (Test-Path -LiteralPath $distRoot) {
        if ((Get-Item -LiteralPath $distRoot -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Refusing a redirected dist directory: $distRoot"
        }
    }
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw "Refusing a reparse point: $fullPath" }
        if ($item.PSIsContainer) {
            $redirect = Get-ChildItem -LiteralPath $fullPath -Recurse -Force |
                Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint } |
                Select-Object -First 1
            if ($redirect) { throw "Refusing a package containing a reparse point: $($redirect.FullName)" }
        }
    }
}

function Find-MsvcRuntime {
    param([string]$VisualStudioPath)
    $redistRoot = Join-Path $VisualStudioPath 'VC\Redist\MSVC'
    if (-not (Test-Path -LiteralPath $redistRoot -PathType Container)) {
        throw "MSVC redistributable directory not found: $redistRoot"
    }
    $versions = @(Get-ChildItem -LiteralPath $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+(\.\d+)?$' } |
        Sort-Object { [version]$_.Name } -Descending)
    foreach ($version in $versions) {
        $crtPath = Join-Path $version.FullName 'x64\Microsoft.VC143.CRT'
        $missing = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') |
            Where-Object { -not (Test-Path -LiteralPath (Join-Path $crtPath $_) -PathType Leaf) }
        if (-not $missing) {
            return [PSCustomObject]@{ Directory = $crtPath; VersionRoot = $version.FullName; Version = $version.Name }
        }
    }
    throw "No complete MSVC x64 release CRT was found below '$redistRoot'. Install the VS2022 C++ runtime components."
}

foreach ($candidate in @($packageRoot, $zipPath, $stageRoot, $stageZip)) { Assert-SafeDistPath $candidate }
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
try {
    Copy-Item -LiteralPath $appPath -Destination (Join-Path $stageRoot 'learningc_studio.exe')
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsPath)) { throw 'MSVC runtime discovery failed.' }
    $vsPath = ([string]$vsPath).Trim()
    $originalVcInstallDir = $env:VCINSTALLDIR
    $originalPath = $env:PATH
    try {
        $env:VCINSTALLDIR = Join-Path ([string]$vsPath).Trim() 'VC\'
        $env:PATH = "$(Join-Path $QtPath 'bin');$originalPath"
        & $deployTool --release --no-translations --no-opengl-sw --compiler-runtime (Join-Path $stageRoot 'learningc_studio.exe')
        if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
    } finally {
        $env:VCINSTALLDIR = $originalVcInstallDir
        $env:PATH = $originalPath
    }
    # windeployqt may supply only vc_redist.x64.exe. A portable ZIP also needs
    # app-local x64 release CRT DLLs, selected from the installed VS toolchain.
    $msvcRuntime = Find-MsvcRuntime $vsPath
    Write-Host "MSVC app-local runtime: $($msvcRuntime.Directory)"
    Get-ChildItem -LiteralPath $msvcRuntime.Directory -Filter '*.dll' -File |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $stageRoot -Force }
    foreach ($requiredFile in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'platforms\qwindows.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $stageRoot $requiredFile))) {
            throw "Deployment is incomplete; missing $requiredFile."
        }
    }
    Copy-Item -LiteralPath (Join-Path $clientRoot 'README.md') -Destination $stageRoot
    Copy-Item -LiteralPath (Join-Path $clientRoot 'docs') -Destination (Join-Path $stageRoot 'docs') -Recurse

    $msvcLicenseRoot = Join-Path $stageRoot 'licenses\msvc'
    New-Item -ItemType Directory -Path $msvcLicenseRoot -Force | Out-Null
    $runtimeNotices = @(Get-ChildItem -LiteralPath $msvcRuntime.VersionRoot -Recurse -File |
        Where-Object { $_.Name -match '^(LICENSE|LICENCE|EULA|NOTICE)([._-]|$)' })
    foreach ($notice in $runtimeNotices) {
        $relative = $notice.FullName.Substring($msvcRuntime.VersionRoot.Length + 1)
        $target = Join-Path $msvcLicenseRoot (Join-Path 'runtime' $relative)
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $notice.FullName -Destination $target
    }
    $vsLicenseRoot = Join-Path $vsPath 'Licenses'
    if (Test-Path -LiteralPath $vsLicenseRoot -PathType Container) {
        foreach ($redistNotice in @(Get-ChildItem -LiteralPath $vsLicenseRoot -Recurse -File -Filter 'Redist.txt')) {
            $relative = $redistNotice.FullName.Substring($vsLicenseRoot.Length + 1)
            $target = Join-Path $msvcLicenseRoot (Join-Path 'visual-studio' $relative)
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $redistNotice.FullName -Destination $target
        }
    }
    $msvcNotice = @"
This package includes app-local Microsoft Visual C++ x64 release CRT DLLs.
Source: Visual Studio 2022, VC/Redist/MSVC/$($msvcRuntime.Version)/x64/Microsoft.VC143.CRT.
Copyright Microsoft Corporation. Microsoft's applicable redistribution terms apply.
Available installed runtime license/notice files are copied under runtime/.
Available Visual Studio redistribution information is copied under visual-studio/.
Redistribution information: https://aka.ms/vs/17/redist.txt
These informational files do not grant additional rights or replace the applicable license.
"@
    Set-Content -LiteralPath (Join-Path $msvcLicenseRoot 'README.txt') -Value $msvcNotice -Encoding UTF8

    $licenseRoot = Join-Path $stageRoot 'licenses\qt-sdk'
    $bundledLicenses = Join-Path $clientRoot 'docs\LICENSES'
    foreach ($licenseName in @('LGPL-3.0-only.txt', 'GPL-3.0-only.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $bundledLicenses $licenseName))) {
            throw "Required bundled Qt license is missing: $licenseName"
        }
    }
    New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
    Copy-Item -LiteralPath $bundledLicenses -Destination (Join-Path $stageRoot 'licenses\qt') -Recurse
    $qtPrefix = $QtPath.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $licenseFiles = @(Get-ChildItem -LiteralPath $QtPath -File -Recurse |
        Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|LGPL|GPL|NOTICE)([._-]|$)' })
    foreach ($licenseFile in $licenseFiles) {
        $relativePath = $licenseFile.FullName.Substring($qtPrefix.Length)
        $target = Join-Path $licenseRoot $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $licenseFile.FullName -Destination $target
    }
    $sdkSbom = Join-Path $QtPath 'sbom'
    if (Test-Path -LiteralPath $sdkSbom -PathType Container) {
        Copy-Item -LiteralPath $sdkSbom -Destination (Join-Path $licenseRoot 'sbom') -Recurse
    }
    $qtVersion = & (Join-Path $QtPath 'bin\qmake.exe') -query QT_VERSION
    if ($LASTEXITCODE -ne 0 -or ([string]$qtVersion).Trim() -notmatch '^\d+\.\d+\.\d+$') {
        throw 'Cannot determine the deployed Qt version for the source notice.'
    }
    $qtVersion = ([string]$qtVersion).Trim()
    $qtSeries = ($qtVersion.Split('.')[0..1] -join '.')
    $licenseNotice = @"
LearningC Studio dynamically links Qt $qtVersion (Core, Gui, Widgets and Network).
Qt copyright: The Qt Company Ltd. and contributors. Third-party copyrights remain
with their respective owners. Qt and its use are available under LGPL version 3
or other applicable Qt licensing terms; see licenses/qt for LGPLv3 and GPLv3.

Available SDK notices and SBOM are under licenses/qt-sdk. See docs/LICENSES/README.md
for the license-text provenance. Keep the notices applicable to your deployed Qt
plugins and third-party dependencies when distributing this package.

Qt source for this version (qtbase contains the modules used by this application):
https://download.qt.io/archive/qt/$qtSeries/$qtVersion/submodules/
Qt license and third-party information: https://doc.qt.io/qt-6/licensing.html

This application uses shared Qt DLLs and plugins. Compatible modified Qt libraries
can be substituted in the deployed directory. Supply any corresponding source and
installation materials required by the license under which you redistribute Qt.
The MSVC runtime is distributed under Microsoft's applicable runtime license.
App-local x64 CRT DLLs are included; see licenses/msvc for available notices.
"@
    Set-Content -LiteralPath (Join-Path $stageRoot 'QT-NOTICES.txt') -Value $licenseNotice -Encoding UTF8

    Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $stageZip -CompressionLevel Optimal
    # Only replace old generated outputs after deployment and compression succeeded.
    foreach ($oldPath in @($packageRoot, $zipPath)) {
        Assert-SafeDistPath $oldPath
        if (Test-Path -LiteralPath $oldPath) { Remove-Item -LiteralPath $oldPath -Recurse -Force }
    }
    Assert-SafeDistPath $stageRoot
    Assert-SafeDistPath $packageRoot
    Move-Item -LiteralPath $stageRoot -Destination $packageRoot
    Assert-SafeDistPath $stageZip
    Assert-SafeDistPath $zipPath
    Move-Item -LiteralPath $stageZip -Destination $zipPath
    Write-Host "Application: $(Join-Path $packageRoot 'learningc_studio.exe')"
    Write-Host "Archive: $zipPath"
} finally {
    foreach ($temporaryPath in @($stageRoot, $stageZip)) {
        Assert-SafeDistPath $temporaryPath
        if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Recurse -Force }
    }
}
