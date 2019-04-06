param(
	[Parameter(mandatory=$true)]
	[string]$PfxCertificate,
	[Parameter(mandatory=$true)]
	[string]$Hostname,
	[Parameter(mandatory=$true)]
	[string]$Port,
	[Parameter(mandatory=$true)]
	[string]$ServerUsernameWithDomain
)

Write-Output "Installing certificate..."
Import-PfxCertificate -FilePath $PfxCertificate -CertStoreLocation 'Cert:\LocalMachine\My' -Verbose

$AppId="{790C242A-DAA6-46FF-8E04-B1EB280F3BA2}"
$AllBindAndPort="0.0.0.0:$Port"

Write-Output "Installing listener..."
netsh http add iplisten ipaddress=$AllBindAndPort

Write-Output "Deleting existing certificates for that port..."
netsh http delete sslcert ipport=$AllBindAndPort

Write-Output "Selecting most recent certificate..."
$Thumbprint = (Get-ChildItem -path cert:\LocalMachine -recurse | where { $_.Subject -match "CN\=$Hostname" } | Sort-Object -Descending { $_.NotAfter } | Select-Object -Index 0 | ForEach-Object { $_.Thumbprint })

Write-Output "Binding app and port to certificate..."
netsh http add sslcert ipport=$AllBindAndPort certhash=$Thumbprint "appid=$AppId"

$HostnameAndPort = $Hostname + ":" + $Port

Write-Output "Deleting existing URL ACL..."
netsh http delete urlacl url=https://$HostnameAndPort/

Write-Output $ServerUsernameWithDomain

Write-Output "Setting up URL ACL for user..."
netsh http add urlacl url=https://$HostnameAndPort/ user=$ServerUsernameWithDomain

Write-Output "All done!"