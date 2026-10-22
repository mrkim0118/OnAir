param(
    [string]$Version = "",
    [string]$PackId = "TechArchive.SimpleScreenRecorder",
    [string]$PackTitle = "Tech Archive Simple Screen Recorder",
    [string]$Channel = "win",
    [string]$Runtime = "win-x64",
    [string]$MainExe = "TechArchiveSimpleScreenRecorder.exe",
    [string]$Authors = "TechArchive",
    [string]$DefaultUpdateSource = "https://tech.trendarchive.io/Utility/TechArchiveSimpleScreenRecorder/",
    [string]$PublishDir = "",
    [string]$OutputDir = "",
    [string]$ReleaseNotes = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "Path value cannot be empty."
    }

    if ([System.IO.Path]::IsPathRooted($Value)) {
        return [System.IO.Path]::GetFullPath($Value)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Value))
}

function Convert-ToFileSafeName {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    $safe = $Value -replace '[^A-Za-z0-9]+', '-'
    return $safe.Trim('-')
}

function Get-VersionFromFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectDir
    )

    $versionPath = Join-Path $ProjectDir "latest-version.txt"
    if (-not (Test-Path -LiteralPath $versionPath)) {
        throw "Version file was not found: $versionPath"
    }

    $version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "Version file is empty: $versionPath"
    }

    return $version
}

function Get-MSBuildPath {
    $vsWherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vsWherePath)) {
        throw "vswhere.exe was not found."
    }

    $installationPath = (& $vsWherePath -latest -products * -requires Microsoft.Component.MSBuild -property installationPath).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
        throw "An MSBuild installation could not be found."
    }

    $msbuildPath = Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"
    if (-not (Test-Path -LiteralPath $msbuildPath)) {
        throw "MSBuild.exe was not found at $msbuildPath"
    }

    return $msbuildPath
}

function Invoke-NativeReleaseBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MSBuildPath,
        [Parameter(Mandatory = $true)]
        [string]$SolutionPath
    )

    & $MSBuildPath $SolutionPath /restore /t:Build /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw "Native release build failed."
    }
}

function Invoke-NativePublish {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildOutputPath,
        [Parameter(Mandatory = $true)]
        [string]$OutputPath
    )

    if (-not (Test-Path -LiteralPath $BuildOutputPath)) {
        throw "Native build output was not found: $BuildOutputPath"
    }

    Copy-Item -Path (Join-Path $BuildOutputPath '*') -Destination $OutputPath -Recurse -Force
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-FullPath -BasePath $scriptRoot -Value ".."
$projectDir = Resolve-FullPath -BasePath $repoRoot -Value "TechArchiveSimpleScreenRecorder"
$solutionPath = Resolve-FullPath -BasePath $repoRoot -Value "TechArchiveSimpleScreenRecorder.sln"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-VersionFromFile -ProjectDir $projectDir
}

if ([string]::IsNullOrWhiteSpace($PublishDir)) {
    $PublishDir = "artifacts\publish\$Version\$Runtime"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = "artifacts\velopack\Releases"
}

if ([string]::IsNullOrWhiteSpace($ReleaseNotes)) {
    $ReleaseNotes = "artifacts\velopack\release-notes-$Version.md"
}

$publishDirPath = Resolve-FullPath -BasePath $repoRoot -Value $PublishDir
$outputDirPath = Resolve-FullPath -BasePath $repoRoot -Value $OutputDir
$releaseNotesPath = Resolve-FullPath -BasePath $repoRoot -Value $ReleaseNotes
$iconPath = Join-Path $projectDir "app.ico"
$defaultsFilePath = Join-Path $publishDirPath "app-defaults.json"
$latestVersionOutputPath = Join-Path $outputDirPath "latest-version.txt"
$installerUrlOutputPath = Join-Path $outputDirPath "installer-url.txt"

New-Item -ItemType Directory -Force -Path $publishDirPath | Out-Null
New-Item -ItemType Directory -Force -Path $outputDirPath | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $releaseNotesPath) | Out-Null

if (Test-Path -LiteralPath $publishDirPath) {
    Get-ChildItem -LiteralPath $publishDirPath -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path -LiteralPath $releaseNotesPath)) {
    @(
        "# Tech Archive Simple Screen Recorder $Version"
        ""
        "- Packaged release."
    ) | Set-Content -LiteralPath $releaseNotesPath -Encoding UTF8
}

$msbuildPath = Get-MSBuildPath
Write-Host "1/3 Native release build"
Invoke-NativeReleaseBuild -MSBuildPath $msbuildPath -SolutionPath $solutionPath

$buildOutputDir = Join-Path $projectDir "build\Release"
if (-not (Test-Path -LiteralPath $buildOutputDir)) {
    throw "Build output directory was not found: $buildOutputDir"
}

Write-Host "2/3 Publishing native app"
Invoke-NativePublish -BuildOutputPath $buildOutputDir -OutputPath $publishDirPath

Copy-Item -LiteralPath (Join-Path $projectDir "latest-version.txt") -Destination $publishDirPath -Force
if (Test-Path -LiteralPath (Join-Path $projectDir "update-url.txt")) {
    Copy-Item -LiteralPath (Join-Path $projectDir "update-url.txt") -Destination $publishDirPath -Force
}
if (Test-Path -LiteralPath (Join-Path $projectDir "installer-url.txt")) {
    Copy-Item -LiteralPath (Join-Path $projectDir "installer-url.txt") -Destination $publishDirPath -Force
}

@{
    DefaultUpdateSource = $DefaultUpdateSource
} | ConvertTo-Json | Set-Content -LiteralPath $defaultsFilePath -Encoding UTF8

Set-Content -LiteralPath $latestVersionOutputPath -Value $Version -Encoding UTF8
$setupUrl = "$DefaultUpdateSource$PackId-$Channel-Setup.exe"
Set-Content -LiteralPath $installerUrlOutputPath -Value $setupUrl -Encoding UTF8

$vpkCommand = Get-Command vpk.exe -ErrorAction SilentlyContinue
if (-not $vpkCommand) {
    $fallbackVpkPath = Join-Path $env:USERPROFILE ".dotnet\tools\vpk.exe"
    if (Test-Path -LiteralPath $fallbackVpkPath) {
        $vpkCommand = Get-Command $fallbackVpkPath -ErrorAction SilentlyContinue
    }
}

if (-not $vpkCommand) {
    throw "The 'vpk' CLI tool is not installed."
}

Write-Host "3/3 Packing Velopack release"
& $vpkCommand.Source pack `
    --packId $PackId `
    --packVersion $Version `
    --packDir $publishDirPath `
    --mainExe $MainExe `
    --packAuthors $Authors `
    --packTitle $PackTitle `
    --channel $Channel `
    --runtime $Runtime `
    --releaseNotes $releaseNotesPath `
    --outputDir $outputDirPath `
    --icon $iconPath `
    --skipVeloAppCheck `
    --shortcuts "Desktop,StartMenuRoot"

if ($LASTEXITCODE -ne 0) {
    throw "vpk pack failed."
}

$setupFileName = "$PackId-$Channel-Setup.exe"
$setupFilePath = Join-Path $outputDirPath $setupFileName
if (Test-Path -LiteralPath $setupFilePath) {
    $versionedSetupName = "{0}-Setup-{1}.exe" -f (Convert-ToFileSafeName -Value $PackTitle), $Version
    $versionedSetupPath = Join-Path $outputDirPath $versionedSetupName
    Copy-Item -LiteralPath $setupFilePath -Destination $versionedSetupPath -Force
    Write-Host "Versioned setup : $versionedSetupPath"
}

Write-Host ""
Write-Host "Velopack release complete."
Write-Host "Publish files : $publishDirPath"
Write-Host "Release feed  : $outputDirPath"
Write-Host "Update source : $DefaultUpdateSource"
