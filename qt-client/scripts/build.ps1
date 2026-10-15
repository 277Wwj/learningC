[CmdletBinding()]
param(
    [string]$QtPath = $env:QT_ROOT,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$clientRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $clientRoot '..'))
$buildRoot = Join-Path $repoRoot 'build-qt'
if ([string]::IsNullOrWhiteSpace($QtPath)) {
    $workspaceRoot = Split-Path -Parent $repoRoot
    $QtPath = Join-Path $workspaceRoot '.qt\6.8.3\msvc2022_64'
}
$QtPath = (Resolve-Path -LiteralPath $QtPath -ErrorAction Stop).Path
if (-not (Test-Path -LiteralPath (Join-Path $QtPath 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw "Qt6 SDK not found in '$QtPath'. Supply -QtPath or set QT_ROOT to the MSVC x64 SDK."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Install Visual Studio 2022 with Desktop development with C++ (including CMake tools).'
}
$vsPath = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsPath)) {
    throw 'Visual Studio 2022 with the MSVC x64 C++ compiler was not found.'
}
$vsPath = ([string]$vsPath).Trim()
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
foreach ($program in @($cmake, $ctest)) {
    if (-not (Test-Path -LiteralPath $program)) { throw "Required tool not found: $program" }
}

function Invoke-CheckedNative {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

Write-Host "Qt SDK: $QtPath"
Write-Host "Visual Studio: $vsPath"
Write-Host "Build directory: $buildRoot"
Invoke-CheckedNative $cmake @(
    '-S', $clientRoot, '-B', $buildRoot,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_GENERATOR_INSTANCE=$vsPath", "-DCMAKE_PREFIX_PATH=$QtPath", '-DBUILD_TESTING=ON'
)
Invoke-CheckedNative $cmake @('--build', $buildRoot, '--config', $Configuration, '--parallel')

if (-not $SkipTests) {
    # A separate CTest process needs the Qt DLL directory as well as the app.
    $originalPath = $env:PATH
    $originalPluginPath = $env:QT_PLUGIN_PATH
    try {
        $env:PATH = "$(Join-Path $QtPath 'bin');$originalPath"
        $env:QT_PLUGIN_PATH = Join-Path $QtPath 'plugins'
        $testListJson = & $ctest --test-dir $buildRoot -C $Configuration --show-only=json-v1
        if ($LASTEXITCODE -ne 0) { throw 'CTest could not enumerate the configured tests.' }
        $testList = ($testListJson -join "`n") | ConvertFrom-Json
        $testNames = @($testList.tests | ForEach-Object { $_.name })
        foreach ($requiredTest in @('test_http', 'test_benchmark', 'test_tcp', 'test_ui')) {
            if ($testNames -notcontains $requiredTest) { throw "Required CTest test is missing: $requiredTest" }
        }
        Invoke-CheckedNative $ctest @('--test-dir', $buildRoot, '-C', $Configuration, '--output-on-failure', '--no-tests=error')
    } finally {
        $env:PATH = $originalPath
        $env:QT_PLUGIN_PATH = $originalPluginPath
    }
}

Write-Host "Application: $(Join-Path $buildRoot "$Configuration\learningc_studio.exe")"
if ($SkipTests) { Write-Host 'Tests were skipped by request.' }
