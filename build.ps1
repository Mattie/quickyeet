[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PackagePublisher = 'CN=QuickYeet',
    [string]$PackageCertificatePath,
    [string]$PackageCertificatePassword,
    [string]$PackageTimestampUrl,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $SkipInstaller -and -not $PackageCertificatePath) {
    throw "An MSIX signing certificate whose subject is '$PackagePublisher' is required for an installable MSI. Pass -PackageCertificatePath, or use -SkipInstaller for an unsigned validation build."
}
if ($PackageCertificatePath -and -not (Test-Path -LiteralPath $PackageCertificatePath -PathType Leaf)) {
    throw "The package signing certificate does not exist: $PackageCertificatePath"
}
if ([string]::IsNullOrWhiteSpace($PackagePublisher)) {
    throw 'The MSIX package publisher cannot be empty.'
}

$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts'))
$generatedManifestDirectory = Join-Path $artifactRoot "obj\QuickYeet.Manifests\$Configuration\x64"
New-Item -ItemType Directory -Path $generatedManifestDirectory -Force | Out-Null
$publisherXml = [Security.SecurityElement]::Escape($PackagePublisher)
$popupManifestSource = Join-Path $repositoryRoot 'src\QuickYeet.Popup\QuickYeet.Popup.manifest'
$popupManifest = Join-Path $generatedManifestDirectory 'QuickYeet.Popup.manifest'
$popupManifestContent = [IO.File]::ReadAllText($popupManifestSource)
if (-not $popupManifestContent.Contains('publisher="CN=QuickYeet"')) {
    throw 'The QuickYeet popup manifest publisher placeholder is missing.'
}
$popupManifestContent =
    $popupManifestContent.Replace('publisher="CN=QuickYeet"', "publisher=`"$publisherXml`"")
[IO.File]::WriteAllText($popupManifest, $popupManifestContent, [Text.UTF8Encoding]::new($false))

function New-QuickYeetLogo {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [int]$Size
    )

    Add-Type -AssemblyName System.Drawing
    $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $pen = $null
    try {
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.Clear([Drawing.Color]::FromArgb(0, 95, 184))
        $pen = [Drawing.Pen]::new([Drawing.Color]::White, [Math]::Max(2, $Size / 11))
        $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
        $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
        $low = [single]($Size * 0.27)
        $high = [single]($Size * 0.73)
        $graphics.DrawLine($pen, $low, $high, $high, $low)
        $graphics.DrawLine($pen, [single]($Size * 0.50), $low, $high, $low)
        $graphics.DrawLine($pen, $high, $low, $high, [single]($Size * 0.50))
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($pen) { $pen.Dispose() }
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools are required to build QuickYeet.'
}

$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'The Visual C++ x64 build tools are required to build QuickYeet.'
}

$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild (Join-Path $repositoryRoot 'QuickYeet.sln') /m /restore "/p:Configuration=$Configuration" /p:Platform=x64 "/p:QuickYeetPopupManifest=$popupManifest"
if ($LASTEXITCODE -ne 0) {
    throw "The QuickYeet $Configuration build failed."
}

$tests = Join-Path $repositoryRoot "artifacts\$Configuration\x64\QuickYeet.Core.Tests.exe"
& $tests
if ($LASTEXITCODE -ne 0) {
    throw 'The QuickYeet core tests failed.'
}

$shellTests = Join-Path $repositoryRoot "artifacts\$Configuration\x64\QuickYeet.Shell.Tests.exe"
$shellBinary = Join-Path $repositoryRoot "artifacts\$Configuration\x64\QuickYeetShell.dll"
& $shellTests $shellBinary
if ($LASTEXITCODE -ne 0) {
    throw 'The QuickYeet shell smoke tests failed.'
}

$windowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
$windowsSdkVersion = Get-ChildItem -LiteralPath $windowsSdkRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $windowsSdkVersion) {
    throw 'The Windows SDK is required to build the QuickYeet identity package.'
}

$makeAppx = Join-Path $windowsSdkVersion.FullName 'x64\makeappx.exe'
$signTool = Join-Path $windowsSdkVersion.FullName 'x64\signtool.exe'
$packageLayout = [IO.Path]::GetFullPath((Join-Path $artifactRoot "obj\QuickYeet.Package\$Configuration\x64"))
if (-not $packageLayout.StartsWith($artifactRoot + [IO.Path]::DirectorySeparatorChar,
                                    [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to stage the package outside the artifact directory: $packageLayout"
}
if (Test-Path -LiteralPath $packageLayout) {
    Remove-Item -LiteralPath $packageLayout -Recurse -Force
}
$assetDirectory = Join-Path $packageLayout 'Assets'
New-Item -ItemType Directory -Path $assetDirectory -Force | Out-Null
$packageManifestSource = Join-Path $repositoryRoot 'packaging\AppxManifest.xml'
$packageManifest = Join-Path $packageLayout 'AppxManifest.xml'
$packageManifestContent = [IO.File]::ReadAllText($packageManifestSource)
if (-not $packageManifestContent.Contains('Publisher="CN=QuickYeet"')) {
    throw 'The QuickYeet package manifest publisher placeholder is missing.'
}
$packageManifestContent =
    $packageManifestContent.Replace('Publisher="CN=QuickYeet"', "Publisher=`"$publisherXml`"")
[IO.File]::WriteAllText($packageManifest, $packageManifestContent, [Text.UTF8Encoding]::new($false))
New-QuickYeetLogo -Path (Join-Path $assetDirectory 'StoreLogo.png') -Size 50
New-QuickYeetLogo -Path (Join-Path $assetDirectory 'Square44x44Logo.png') -Size 44
New-QuickYeetLogo -Path (Join-Path $assetDirectory 'Square150x150Logo.png') -Size 150

$identityPackage = Join-Path $repositoryRoot "artifacts\$Configuration\x64\QuickYeet.Identity.msix"
& $makeAppx pack /o /nv /d $packageLayout /p $identityPackage
if ($LASTEXITCODE -ne 0) {
    throw 'The QuickYeet sparse identity package build failed.'
}

if ($PackageCertificatePath) {
    $signArguments = @('sign', '/fd', 'SHA256', '/f', $PackageCertificatePath)
    if ($PackageCertificatePassword) {
        $signArguments += @('/p', $PackageCertificatePassword)
    }
    if ($PackageTimestampUrl) {
        $signArguments += @('/tr', $PackageTimestampUrl, '/td', 'SHA256')
    }
    $signArguments += $identityPackage
    & $signTool @signArguments
    if ($LASTEXITCODE -ne 0) {
        throw "The QuickYeet identity package could not be signed. Its certificate subject must match '$PackagePublisher'."
    }
    & $signTool verify /pa $identityPackage
    if ($LASTEXITCODE -ne 0) {
        throw 'The QuickYeet identity package signature verification failed.'
    }
}

if (-not $SkipInstaller) {
    $popupBinary = Join-Path $repositoryRoot "artifacts\$Configuration\x64\QuickYeetPopup.exe"
    $packageRegistrar = Join-Path $repositoryRoot "artifacts\$Configuration\x64\QuickYeetPackageRegistrar.exe"
    $installerProject = Join-Path $repositoryRoot 'installer\QuickYeet.Installer.wixproj'
    & dotnet.exe build $installerProject --configuration $Configuration "/p:ShellBinary=$shellBinary" "/p:PopupBinary=$popupBinary" "/p:PackageRegistrar=$packageRegistrar" "/p:IdentityPackage=$identityPackage"
    if ($LASTEXITCODE -ne 0) {
        throw 'The QuickYeet MSI build failed.'
    }

    [xml]$installerSource = Get-Content -LiteralPath (Join-Path $repositoryRoot 'installer\Package.wxs') -Raw
    $package = $installerSource.Wix.Package
    $installerPath = Join-Path $repositoryRoot "installer\bin\$Configuration\QuickYeet.msi"
    & (Join-Path $repositoryRoot 'tests\Verify-Installer.ps1') `
        -MsiPath $installerPath `
        -ExpectedVersion $package.Version `
        -ExpectedUpgradeCode $package.UpgradeCode
    if ($LASTEXITCODE -ne 0) {
        throw 'The QuickYeet MSI upgrade-contract verification failed.'
    }

    $msiSignArguments = @('sign', '/fd', 'SHA256', '/f', $PackageCertificatePath)
    if ($PackageCertificatePassword) {
        $msiSignArguments += @('/p', $PackageCertificatePassword)
    }
    if ($PackageTimestampUrl) {
        $msiSignArguments += @('/tr', $PackageTimestampUrl, '/td', 'SHA256')
    }
    $msiSignArguments += $installerPath
    & $signTool @msiSignArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'The QuickYeet MSI could not be signed.'
    }
    & $signTool verify /pa $installerPath
    if ($LASTEXITCODE -ne 0) {
        throw 'The QuickYeet MSI signature verification failed.'
    }
}
