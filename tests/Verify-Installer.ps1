[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$MsiPath,

    [Parameter(Mandatory)]
    [string]$ExpectedVersion,

    [Parameter(Mandatory)]
    [string]$ExpectedUpgradeCode
)

$ErrorActionPreference = 'Stop'

function Open-MsiDatabase {
    param(
        [Parameter(Mandatory)] $Installer,
        [Parameter(Mandatory)] [string]$Path
    )

    $Installer.GetType().InvokeMember(
        'OpenDatabase',
        [Reflection.BindingFlags]::InvokeMethod,
        $null,
        $Installer,
        @([IO.Path]::GetFullPath($Path), 0))
}

function Get-MsiRows {
    param(
        [Parameter(Mandatory)] $Database,
        [Parameter(Mandatory)] [string]$Query,
        [Parameter(Mandatory)] [int]$ColumnCount
    )

    $view = $Database.GetType().InvokeMember(
        'OpenView',
        [Reflection.BindingFlags]::InvokeMethod,
        $null,
        $Database,
        @($Query))
    try {
        $view.GetType().InvokeMember(
            'Execute',
            [Reflection.BindingFlags]::InvokeMethod,
            $null,
            $view,
            $null) | Out-Null

        $rows = @()
        while ($record = $view.GetType().InvokeMember(
                'Fetch',
                [Reflection.BindingFlags]::InvokeMethod,
                $null,
                $view,
                $null)) {
            try {
                $values = for ($column = 1; $column -le $ColumnCount; $column++) {
                    $record.GetType().InvokeMember(
                        'StringData',
                        [Reflection.BindingFlags]::GetProperty,
                        $null,
                        $record,
                        $column)
                }
                $rows += [pscustomobject]@{ Values = $values }
            } finally {
                [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) | Out-Null
            }
        }
        return $rows
    } finally {
        try {
            $view.GetType().InvokeMember(
                'Close',
                [Reflection.BindingFlags]::InvokeMethod,
                $null,
                $view,
                $null) | Out-Null
        } finally {
            [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) | Out-Null
        }
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [AllowNull()] $Actual,
        [AllowNull()] $Expected
    )

    if ($Actual -ne $Expected) {
        throw "$Name was '$Actual'; expected '$Expected'."
    }
}

if (-not (Test-Path -LiteralPath $MsiPath -PathType Leaf)) {
    throw "The MSI does not exist: $MsiPath"
}

$installer = $null
$database = $null
try {
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = Open-MsiDatabase -Installer $installer -Path $MsiPath
$tableRows = Get-MsiRows -Database $database -Query 'SELECT `Name` FROM `_Tables`' -ColumnCount 1
$tableNames = $tableRows | ForEach-Object { $_.Values }

$propertyRows = Get-MsiRows -Database $database -Query 'SELECT `Property`, `Value` FROM `Property`' -ColumnCount 2
$properties = @{}
foreach ($row in $propertyRows) {
    $properties[$row.Values[0]] = $row.Values[1]
}

Assert-Equal -Name 'ProductVersion' -Actual $properties.ProductVersion -Expected $ExpectedVersion
Assert-Equal -Name 'UpgradeCode' -Actual $properties.UpgradeCode -Expected $ExpectedUpgradeCode
Assert-Equal -Name 'ALLUSERS' -Actual $properties.ALLUSERS -Expected '1'
Assert-Equal -Name 'MSIRESTARTMANAGERCONTROL' -Actual $properties.MSIRESTARTMANAGERCONTROL -Expected 'DisableShutdown'

$upgradeRows = Get-MsiRows -Database $database -Query 'SELECT `UpgradeCode`, `VersionMax`, `Attributes`, `ActionProperty` FROM `Upgrade`' -ColumnCount 4
$upgradeDetection = $upgradeRows | Where-Object { $_.Values[3] -eq 'WIX_UPGRADE_DETECTED' }
$downgradeDetection = $upgradeRows | Where-Object { $_.Values[3] -eq 'WIX_DOWNGRADE_DETECTED' }
if (-not $upgradeDetection -or -not $downgradeDetection) {
    throw 'The MSI is missing its major-upgrade or downgrade-detection rows.'
}
Assert-Equal -Name 'Major-upgrade UpgradeCode' -Actual $upgradeDetection.Values[0] -Expected $ExpectedUpgradeCode
Assert-Equal -Name 'Major-upgrade VersionMax' -Actual $upgradeDetection.Values[1] -Expected $ExpectedVersion

$fileRows = Get-MsiRows -Database $database -Query 'SELECT `FileName` FROM `File`' -ColumnCount 1
$fileNames = $fileRows | ForEach-Object { ($_.Values -split '\|')[-1] }
if ($fileNames -notcontains 'QuickYeetModernShell.dll') {
    throw 'The MSI does not install the version-transition-safe QuickYeetModernShell.dll.'
}
if ($fileNames -contains 'QuickYeetShell.dll') {
    throw 'The MSI still attempts to replace the legacy Explorer-loaded QuickYeetShell.dll.'
}

$registryRows = if ($tableNames -contains 'Registry') {
    Get-MsiRows -Database $database -Query 'SELECT `Key` FROM `Registry`' -ColumnCount 1
} else {
    @()
}
$registryKeys = $registryRows | ForEach-Object { $_.Values }
if ($registryKeys | Where-Object { $_ -like '*ContextMenuHandlers\QuickYeet*' -or $_ -like '*3DF187D7-6A2E-4E81-87C4-6B0D28A49678*' }) {
    throw 'The MSI still registers the legacy in-process Explorer handler.'
}

$removeRegistryRows = Get-MsiRows -Database $database -Query 'SELECT `Key` FROM `RemoveRegistry`' -ColumnCount 1
$removeRegistryKeys = $removeRegistryRows | ForEach-Object { $_.Values }
foreach ($requiredKey in @(
        'Software\Classes\CLSID\{3DF187D7-6A2E-4E81-87C4-6B0D28A49678}',
        'Software\Classes\*\shellex\ContextMenuHandlers\QuickYeet')) {
    if ($removeRegistryKeys -notcontains $requiredKey) {
        throw "The MSI does not remove the legacy registry key: $requiredKey"
    }
}

$sequenceRows = Get-MsiRows -Database $database -Query "SELECT `Action`, `Sequence` FROM `InstallExecuteSequence` WHERE `Action`='RemoveExistingProducts'" -ColumnCount 2
if ($sequenceRows.Count -ne 1) {
    throw 'The MSI does not schedule exactly one RemoveExistingProducts action.'
}
$removeExistingProductsSequence = [int]$sequenceRows[0].Values[1]
if ($removeExistingProductsSequence -le 1500 -or $removeExistingProductsSequence -ge 4000) {
    throw "RemoveExistingProducts has unexpected sequence $removeExistingProductsSequence; expected transactional early removal."
}

Write-Host "PASS  MSI upgrade contract ($ExpectedVersion, $ExpectedUpgradeCode)"
} finally {
    if ($database) {
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null
    }
    if ($installer) {
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
    }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
