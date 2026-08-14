---
name: quickyeet-build-and-install
description: Build, locally sign, install, update, and verify QuickYeet from its Windows C++ repository. Use when an agent is asked to compile QuickYeet, create or reuse a personal development signing certificate, produce signed MSIX and MSI artifacts, install the Explorer extension, update an existing local installation, or diagnose post-install QuickYeet registration and Windows shell health.
---

# QuickYeet Build and Install

Build the repository with its own `build.ps1`, sign the sparse MSIX with a certificate owned by the current Windows user, install the MSI through UAC, and verify the installed Explorer contracts.

## Guardrails

- Run only from the QuickYeet repository containing `build.ps1`, `QuickYeet.sln`, `installer/`, and `packaging/`.
- Keep the package publisher exactly `CN=QuickYeet`; the certificate subject and package publisher must match byte-for-byte.
- Keep the PFX and protected password outside the repository under `%LOCALAPPDATA%\QuickYeet\Signing`.
- Never print, log, commit, or return the plaintext PFX password.
- The existing build interface accepts a plaintext password string and passes it to `signtool.exe` with `/p`, so same-machine process inspection can expose it transiently. Run this workflow only on the user's trusted workstation, minimize the plaintext lifetime, and disclose this limitation in the result.
- Preserve the user's source changes and configuration data.
- Expect a visible UAC prompt for the per-machine MSI. Ask the user to approve it; never claim success from a non-elevated error.
- Require same-user UAC consent. If Windows requests credentials for a different administrator account, stop because the sparse MSIX registration is per-user.
- Warn and obtain explicit approval immediately before restarting Explorer. A restart briefly removes the taskbar and open Explorer windows and may interrupt file operations.
- Treat a self-signed, untimestamped development signature as local-only. Public distribution requires a trusted signing provider and timestamp service.

## 1. Confirm the repository and dependencies

Resolve the repository root with Git when available and confirm the required files:

```powershell
$repo = if (git rev-parse --show-toplevel 2>$null) {
    (git rev-parse --show-toplevel).Trim()
} else {
    (Get-Location).Path
}

foreach ($relative in 'build.ps1','QuickYeet.sln','installer','packaging') {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $relative))) {
        throw "This is not a complete QuickYeet repository: $repo"
    }
}
```

The build script checks Visual C++ Build Tools, the Windows SDK, the test executables, and MSI packaging. Confirm `dotnet.exe` is available before the signed build.

## 2. Prepare personal development signing material

Use these paths:

```powershell
$signingDir = Join-Path $env:LOCALAPPDATA 'QuickYeet\Signing'
$pfxPath = Join-Path $signingDir 'QuickYeet-dev.pfx'
$cerPath = Join-Path $signingDir 'QuickYeet-dev.cer'
$passwordPath = Join-Path $signingDir 'QuickYeet-dev-password.clixml'
```

Reuse the files only when all three exist. Load the DPAPI-protected password and verify the exact subject, validity window, Code Signing EKU, matching CER/PFX thumbprints, private-key store entry, and current-user trust entry:

```powershell
$securePassword = Import-Clixml -LiteralPath $passwordPath
try {
    $pfxCertificate = (Get-PfxData -FilePath $pfxPath -Password $securePassword).EndEntityCertificates |
        Select-Object -First 1
    $cerCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cerPath)
    $now = Get-Date
    $privateCertificate = Get-ChildItem 'Cert:\CurrentUser\My' |
        Where-Object Thumbprint -eq $pfxCertificate.Thumbprint |
        Where-Object HasPrivateKey |
        Select-Object -First 1
    $trustedCertificate = Get-ChildItem 'Cert:\CurrentUser\TrustedPeople' |
        Where-Object Thumbprint -eq $pfxCertificate.Thumbprint |
        Select-Object -First 1
    $ekuOids = @(
        $pfxCertificate.Extensions |
            Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] } |
            ForEach-Object { $_.EnhancedKeyUsages } |
            ForEach-Object { $_.Value }
    )

    if (-not $pfxCertificate -or $pfxCertificate.Subject -cne 'CN=QuickYeet' -or
        $pfxCertificate.Thumbprint -ne $cerCertificate.Thumbprint -or
        $now -lt $pfxCertificate.NotBefore -or $now -gt $pfxCertificate.NotAfter -or
        '1.3.6.1.5.5.7.3.3' -notin $ekuOids -or
        -not $privateCertificate -or -not $trustedCertificate) {
        throw 'QuickYeet signing material failed validation.'
    }
} finally {
    $securePassword = $null
}
```

If signing material is partial, expired, unreadable, or has another subject, report the problem and ask before replacing it. After approval, copy the existing files into a timestamped backup below the signing directory and leave old certificate-store entries in place until the replacement has built, signed, installed, and verified successfully. Then offer to remove the old entries by exact thumbprint.

When no signing files exist, create them for the current Windows user:

```powershell
New-Item -ItemType Directory -Path $signingDir -Force | Out-Null

$passwordBytes = New-Object byte[] 32
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($passwordBytes)
$plainPassword = [Convert]::ToBase64String($passwordBytes)
$securePassword = ConvertTo-SecureString $plainPassword -AsPlainText -Force

try {
    $certificate = New-SelfSignedCertificate `
        -Type Custom `
        -Subject 'CN=QuickYeet' `
        -FriendlyName 'QuickYeet personal development signing' `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @(
            '2.5.29.37={text}1.3.6.1.5.5.7.3.3',
            '2.5.29.19={text}'
        )

    Export-PfxCertificate -Cert $certificate -FilePath $pfxPath -Password $securePassword | Out-Null
    Export-Certificate -Cert $certificate -FilePath $cerPath | Out-Null
    $securePassword | Export-Clixml -LiteralPath $passwordPath
    Import-Certificate -FilePath $cerPath `
        -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
} finally {
    $rng.Dispose()
    [Array]::Clear($passwordBytes, 0, $passwordBytes.Length)
    $plainPassword = $null
    $securePassword = $null
}
```

Trust only the public CER. Keep the private PFX protected and outside source control.

## 3. Build, test, sign, and package

Run the repository build after importing the DPAPI-protected password. The current `build.ps1` interface requires a temporary plaintext string and passes it to `signtool.exe` with `/p`; keep its lifetime as short as possible and do not inspect or log process command lines during signing:

```powershell
$securePassword = Import-Clixml -LiteralPath $passwordPath
$plainPassword = [Net.NetworkCredential]::new('', $securePassword).Password

try {
    & (Join-Path $repo 'build.ps1') `
        -Configuration Release `
        -PackagePublisher 'CN=QuickYeet' `
        -PackageCertificatePath $pfxPath `
        -PackageCertificatePassword $plainPassword
    if ($LASTEXITCODE -ne 0) {
        throw 'The QuickYeet signed build failed.'
    }
} finally {
    $plainPassword = $null
    $securePassword = $null
}
```

Require the build's core tests, Explorer COM test, MSIX signature verification, and MSI build to succeed. The expected installer is `installer\bin\Release\QuickYeet.msi`. Treat WiX `WIX1076 / ICE61` as the known same-version local-upgrade warning when the build otherwise succeeds.

## 4. Install through UAC

Use an elevated, waited process from the start. Write the MSI log under `D:\Temp` and inspect it when installation fails.

The user must approve the UAC consent prompt as the currently signed-in user. Treat cancellation, a credential prompt for another administrator, or failure to start the elevated process as an installation failure.

```powershell
$msi = (Resolve-Path -LiteralPath (Join-Path $repo 'installer\bin\Release\QuickYeet.msi')).Path
$logDirectory = 'D:\Temp'
if (-not (Test-Path -LiteralPath $logDirectory)) {
    New-Item -ItemType Directory -Path $logDirectory | Out-Null
}
$log = Join-Path $logDirectory 'QuickYeet-install-elevated.log'
$arguments = @('/i', ('"' + $msi + '"'), '/qn', '/norestart', '/L*v', ('"' + $log + '"'))
$installer = Start-Process -FilePath 'msiexec.exe' -Verb RunAs `
    -ArgumentList $arguments -Wait -PassThru

if ($installer.ExitCode -notin 0, 3010) {
    throw "QuickYeet installation failed with exit code $($installer.ExitCode). Inspect $log"
}
```

Exit code `3010` means installation succeeded and Windows requests a reboot.

## 5. Verify the installed build

Require all of these checks:

1. `Get-AppxPackage -Name QuickYeet.Identity` reports `Status: Ok` and the expected publisher and version.
2. The SHA-256 hashes of these installed files match their Release artifacts:
   - installed `QuickYeetModernShell.dll` matches the `QuickYeetShell.dll` build artifact
   - `QuickYeetPopup.exe`
   - `QuickYeetPackageRegistrar.exe`
   - `QuickYeet.Identity.msix`
3. Run the Explorer contract test against the installed DLL:

```powershell
& (Join-Path $repo 'artifacts\Release\x64\QuickYeet.Shell.Tests.exe') `
    'C:\Program Files\QuickYeet\QuickYeetModernShell.dll'
if ($LASTEXITCODE -ne 0) {
    throw 'The installed QuickYeet Explorer contract failed.'
}
```

4. Confirm `explorer`, `StartMenuExperienceHost`, `ShellExperienceHost`, and `SearchHost` are present. Use `Process.Responding` only as a signal for Explorer; it is not authoritative for packaged shell hosts. When shell health is in doubt, ask the user to confirm that the Windows key opens Start.
5. Confirm the MSI uninstall registration under `HKLM` reports QuickYeet with the expected version from `installer\Package.wxs`.

If Windows Installer leaves shell hosts missing or Explorer unresponsive, explain the disruption and obtain explicit approval immediately before restarting Explorer:

```powershell
$explorerPath = Join-Path $env:WINDIR 'explorer.exe'
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process -FilePath $explorerPath
```

Wait for the shell hosts to return, then rerun the installed Explorer contract. Avoid another restart when all hosts are already healthy.

## 6. Report the result

Report:

- signed MSIX and MSI paths;
- certificate subject and thumbprint, without its password;
- build, test, signature, installation, hash, package, and Explorer-contract results;
- whether UAC was approved;
- whether Explorer was restarted; and
- any remaining warning or requested reboot.
