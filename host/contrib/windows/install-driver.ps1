# nocry: one-time USB driver setup for Windows. Needs administrator rights.
#   install-driver.ps1             install
#   install-driver.ps1 -Uninstall  remove the driver package and the certificate
#
# What it does: Windows only accepts a driver package that is signed by a publisher the
# computer trusts. This script creates a signing certificate on this computer, signs
# nocry.inf with it, tells Windows to trust that certificate, and then DELETES the
# certificate's private key, so nothing else can ever be signed with it. Then it hands the
# package to Windows (pnputil). The driver itself is Windows' own WinUSB.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$subject = 'CN=nocry USB driver (this computer only)'
$log = Join-Path $PSScriptRoot 'install-driver.log'
function Say($m) { Write-Host $m; Add-Content -Path $log -Value $m -ErrorAction SilentlyContinue }
function Remove-Trust {
    foreach ($store in 'Root', 'TrustedPublisher', 'My') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $subject } | ForEach-Object {
            Say "removing certificate $($_.Thumbprint) from $store"
            Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue
            if (Test-Path $_.PSPath) { Remove-Item $_.PSPath }
        }
    }
}
function Remove-Package {
    # published names (oemNN.inf) of packages whose original name is nocry.inf
    $name = $null
    foreach ($line in (& pnputil.exe /enum-drivers)) {
        if ($line -match '(oem\d+\.inf)') { $name = $Matches[1] }
        elseif ($line -match 'nocry\.inf' -and $name) {
            Say "removing driver package $name"
            & pnputil.exe /delete-driver $name /uninstall /force | ForEach-Object { Say $_ }
            $name = $null
        }
    }
}
try {
    Set-Content -Path $log -Value "nocry driver setup $(Get-Date -Format s)" -ErrorAction SilentlyContinue
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { throw 'This script needs administrator rights.' }
    if ($Uninstall) {
        Remove-Package
        Remove-Trust
        Say 'done'
        exit 0
    }
    $work = Join-Path $env:TEMP ("nocry-driver-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $work | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'nocry.inf') $work
    $inf = Join-Path $work 'nocry.inf'
    $cat = Join-Path $work 'nocry.cat'
    Remove-Package
    Remove-Trust
    Say 'creating a signing certificate'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
        -KeyUsage DigitalSignature -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(30)
    try {
        Say 'signing the driver package'
        New-FileCatalog -Path $inf -CatalogFilePath $cat -CatalogVersion 2.0 | Out-Null
        $sig = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
        if (-not $sig.SignerCertificate) { throw "signing failed: $($sig.StatusMessage)" }
        $cer = Join-Path $work 'nocry.cer'
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    } finally {
        # the private key is never needed again: destroy it
        Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
        Say 'signing key destroyed'
    }
    Say 'installing the driver package'
    & pnputil.exe /add-driver $inf /install | ForEach-Object { Say $_ }
    # 0 = done, 3010 = done, restart suggested, 259 = added, no matching device connected right now
    if ($LASTEXITCODE -notin 0, 259, 3010) { throw "pnputil failed with code $LASTEXITCODE" }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    Say 'done'
    exit 0
} catch {
    Say "ERROR: $_"
    exit 1
}
