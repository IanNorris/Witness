#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Configure TLS certificates for the Witness server.

.DESCRIPTION
    Sets up TLS certificates for Witness. Supports:
    - Self-signed certificate generation (for development/testing)
    - Let's Encrypt via certbot (for production)
    - User-provided PEM certificate and key files

    Certificate paths are written to the Witness database settings.
    Optionally sets up a scheduled task for automatic certbot renewal.

.NOTES
    TODO: Provide a Linux variant (setup-tls.sh) when Linux port is done.
#>

param(
    [ValidateSet("SelfSigned", "LetsEncrypt", "Manual")]
    [string]$Mode,

    [string]$Hostname,
    [string]$Email,
    [string]$CertPath,
    [string]$KeyPath,
    [string]$DatabasePath
)

$ErrorActionPreference = "Stop"

# Default database path
if (-not $DatabasePath) {
    $DatabasePath = Join-Path $env:ProgramData "Witness\server.db"
}

function Write-Setting {
    param([string]$DbPath, [string]$Name, [string]$Value)

    $escapedValue = $Value -replace "'", "''"
    $sql = "INSERT OR REPLACE INTO Setting (Name, Value) VALUES ('$Name', '$escapedValue');"

    # Try sqlite3 CLI first, fall back to System.Data.SQLite
    $sqlite3 = Get-Command sqlite3 -ErrorAction SilentlyContinue
    if ($sqlite3) {
        $sql | & sqlite3 $DbPath
    } else {
        # Try loading SQLite .NET assembly
        try {
            Add-Type -Path (Join-Path $PSScriptRoot "..\Installer\packages\sqlite-net-pcl.1.5.231\lib\netstandard2.0\SQLite-net.dll") -ErrorAction SilentlyContinue
        } catch {}

        # Fall back to direct ADO.NET if available
        $connectionString = "Data Source=$DbPath;Version=3;"
        $connection = New-Object System.Data.SQLite.SQLiteConnection($connectionString)
        $connection.Open()
        $command = $connection.CreateCommand()
        $command.CommandText = $sql
        $command.ExecuteNonQuery() | Out-Null
        $connection.Close()
    }
}

function Get-CertOutputDir {
    $dir = Join-Path $env:ProgramData "Witness\tls"
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    return $dir
}

function New-SelfSignedPemCert {
    param([string]$Hostname)

    $outputDir = Get-CertOutputDir
    $certFile = Join-Path $outputDir "cert.pem"
    $keyFile = Join-Path $outputDir "key.pem"

    Write-Host "Generating self-signed certificate for '$Hostname'..."

    # Check for openssl
    $openssl = Get-Command openssl -ErrorAction SilentlyContinue
    if (-not $openssl) {
        # Try common vcpkg/build locations
        $candidates = @(
            (Join-Path $PSScriptRoot "..\build\vcpkg_installed\x64-windows\tools\openssl\openssl.exe"),
            (Join-Path $PSScriptRoot "..\vcpkg_installed\x64-windows\tools\openssl\openssl.exe")
        )
        foreach ($c in $candidates) {
            if (Test-Path $c) {
                $openssl = Get-Item $c
                break
            }
        }
    }

    if (-not $openssl) {
        Write-Error "OpenSSL not found. Install OpenSSL or ensure it's on your PATH."
        return $null
    }

    $opensslPath = if ($openssl -is [System.Management.Automation.ApplicationInfo]) { $openssl.Source } else { $openssl.FullName }

    & $opensslPath req -x509 -newkey rsa:2048 -keyout $keyFile -out $certFile `
        -days 365 -nodes -subj "/CN=$Hostname" `
        -addext "subjectAltName=DNS:$Hostname,DNS:localhost,IP:127.0.0.1" 2>&1

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to generate self-signed certificate."
        return $null
    }

    Write-Host "Certificate: $certFile"
    Write-Host "Private key: $keyFile"
    Write-Host ""
    Write-Host "WARNING: Self-signed certificates will show browser warnings."
    Write-Host "         For production use, use Let's Encrypt or a trusted CA."

    return @{ Cert = $certFile; Key = $keyFile }
}

function Invoke-CertbotSetup {
    param([string]$Hostname, [string]$Email)

    # Check for certbot
    $certbot = Get-Command certbot -ErrorAction SilentlyContinue
    if (-not $certbot) {
        Write-Host ""
        Write-Host "certbot is not installed. Install it with one of:"
        Write-Host "  winget install EFF.Certbot"
        Write-Host "  scoop install certbot"
        Write-Host "  https://certbot.eff.org/"
        Write-Error "certbot not found."
        return $null
    }

    if (-not $Email) {
        Write-Error "Email address is required for Let's Encrypt. Use -Email parameter."
        return $null
    }

    Write-Host "Requesting Let's Encrypt certificate for '$Hostname'..."
    Write-Host "Ensure port 80 is forwarded to this machine and not in use."
    Write-Host ""

    & certbot certonly --standalone -d $Hostname --agree-tos --email $Email --non-interactive 2>&1

    if ($LASTEXITCODE -ne 0) {
        Write-Error "certbot failed. Check the output above."
        return $null
    }

    # certbot stores certs in /etc/letsencrypt/live/ on Linux, on Windows in C:\Certbot\live\
    $certbotLive = if ($IsWindows -or $env:OS -eq "Windows_NT") {
        "C:\Certbot\live\$Hostname"
    } else {
        "/etc/letsencrypt/live/$Hostname"
    }

    $certFile = Join-Path $certbotLive "fullchain.pem"
    $keyFile = Join-Path $certbotLive "privkey.pem"

    if (-not (Test-Path $certFile) -or -not (Test-Path $keyFile)) {
        Write-Error "Certificate files not found at expected location: $certbotLive"
        return $null
    }

    Write-Host "Certificate: $certFile"
    Write-Host "Private key: $keyFile"

    return @{ Cert = $certFile; Key = $keyFile }
}

function Install-RenewalTask {
    param([string]$Hostname)

    $taskName = "Witness TLS Certificate Renewal"

    # Remove existing task if present
    $existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($existing) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }

    $certbot = (Get-Command certbot -ErrorAction SilentlyContinue).Source
    if (-not $certbot) {
        Write-Warning "certbot not found — skipping renewal task setup."
        return
    }

    # Build deploy hook command to call the reload endpoint
    $deployHook = "curl -s -k -X POST https://localhost/debug/reload_tls -d `"{}`""

    $action = New-ScheduledTaskAction -Execute $certbot `
        -Argument "renew --deploy-hook `"$deployHook`""

    $trigger = New-ScheduledTaskTrigger -Daily -At "03:00"

    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest

    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -RunOnlyIfNetworkAvailable

    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings -Description "Renew Let's Encrypt TLS certificate for Witness" | Out-Null

    Write-Host "Scheduled task '$taskName' created (runs daily at 3:00 AM)."
}

# Interactive mode selection if not provided
if (-not $Mode) {
    Write-Host "=== Witness TLS Certificate Setup ==="
    Write-Host ""
    Write-Host "  1. Self-Signed  (development/testing - browser warnings)"
    Write-Host "  2. Let's Encrypt (production - requires port 80 + public DNS)"
    Write-Host "  3. Manual       (provide your own PEM cert and key files)"
    Write-Host ""
    $choice = Read-Host "Select option (1-3)"

    switch ($choice) {
        "1" { $Mode = "SelfSigned" }
        "2" { $Mode = "LetsEncrypt" }
        "3" { $Mode = "Manual" }
        default { Write-Error "Invalid choice."; exit 1 }
    }
}

if (-not $Hostname -and $Mode -ne "Manual") {
    $Hostname = Read-Host "Enter hostname (e.g., cameras.example.com)"
}

# Execute selected mode
$result = $null

switch ($Mode) {
    "SelfSigned" {
        $result = New-SelfSignedPemCert -Hostname $Hostname
    }
    "LetsEncrypt" {
        if (-not $Email) {
            $Email = Read-Host "Enter email for Let's Encrypt notifications"
        }
        $result = Invoke-CertbotSetup -Hostname $Hostname -Email $Email

        if ($result) {
            $setupRenewal = Read-Host "Set up automatic renewal? (Y/n)"
            if ($setupRenewal -ne "n" -and $setupRenewal -ne "N") {
                Install-RenewalTask -Hostname $Hostname
            }
        }
    }
    "Manual" {
        if (-not $CertPath) {
            $CertPath = Read-Host "Enter path to PEM certificate file"
        }
        if (-not $KeyPath) {
            $KeyPath = Read-Host "Enter path to PEM private key file"
        }

        if (-not (Test-Path $CertPath)) {
            Write-Error "Certificate file not found: $CertPath"
            exit 1
        }
        if (-not (Test-Path $KeyPath)) {
            Write-Error "Key file not found: $KeyPath"
            exit 1
        }

        $result = @{ Cert = $CertPath; Key = $KeyPath }
    }
}

if (-not $result) {
    Write-Error "TLS setup failed."
    exit 1
}

# Write settings to database
if (Test-Path $DatabasePath) {
    Write-Host ""
    Write-Host "Writing certificate paths to database..."
    Write-Setting -DbPath $DatabasePath -Name "server_tls_cert" -Value $result.Cert
    Write-Setting -DbPath $DatabasePath -Name "server_tls_key" -Value $result.Key
    Write-Setting -DbPath $DatabasePath -Name "server_tls_mode" -Value "TLS"
    Write-Host "Database updated."
} else {
    Write-Warning "Database not found at $DatabasePath"
    Write-Host "Manually add these settings to your Witness database:"
    Write-Host "  server_tls_cert = $($result.Cert)"
    Write-Host "  server_tls_key  = $($result.Key)"
    Write-Host "  server_tls_mode = TLS"
}

Write-Host ""
Write-Host "TLS setup complete. Restart the Witness server to apply changes."
Write-Host "  Or call POST /debug/reload_tls to hot-reload the certificate."
