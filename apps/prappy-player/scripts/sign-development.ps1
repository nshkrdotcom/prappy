#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$Configuration = "$env:USERPROFILE/.config/prappy/development-signing.json"
)
$ErrorActionPreference='Stop'
if (!(Test-Path -LiteralPath $Configuration)) {
    throw "Development signing isn't configured. Expected $Configuration. See docs/development-signing.md."
}
$settings=Get-Content -LiteralPath $Configuration -Raw | ConvertFrom-Json
$thumbprint=([string]$settings.certificate_thumbprint).Replace(' ','').ToUpperInvariant()
if ($thumbprint -notmatch '^[0-9A-F]{40}$') { throw 'Invalid development certificate thumbprint.' }
$certificate=Get-Item -LiteralPath "Cert:/CurrentUser/My/$thumbprint" -ErrorAction Stop
if (!$certificate.HasPrivateKey -or $certificate.NotBefore -gt (Get-Date) -or $certificate.NotAfter -le (Get-Date)) {
    throw 'The configured development signing certificate has no private key or is outside its validity period.'
}
if ($certificate.NotAfter -lt (Get-Date).AddDays(14)) {
    Write-Warning "Development certificate expires $($certificate.NotAfter). Renew and explicitly trust its replacement before expiry."
}
$sdk=Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/signtool.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1
if (!$sdk) { throw 'Windows SDK x64 SignTool is missing.' }
$root=(Resolve-Path -LiteralPath $BuildDirectory).Path
$files=@(Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Extension -in '.exe','.dll' })
if (!$files.Count) { throw "No executable artifacts in $root" }
foreach ($file in $files) {
    $signature=Get-AuthenticodeSignature -LiteralPath $file.FullName
    # Retain valid Microsoft/vendor signatures, and signatures on unchanged local builds.
    if ($signature.Status -eq 'Valid') { continue }
    if ($signature.Status -ne 'NotSigned') {
        throw "Refusing to replace an invalid existing signature on $($file.FullName): $($signature.Status)"
    }
    & $sdk.FullName sign /fd SHA256 /s My /sha1 $thumbprint $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Development signing failed: $($file.FullName)" }
    & $sdk.FullName verify /pa $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Development signature validation failed: $($file.FullName)" }
}
$artifacts=@($files | ForEach-Object {
    $signature=Get-AuthenticodeSignature -LiteralPath $_.FullName
    if ($signature.Status -ne 'Valid') { throw "Invalid final signature: $($_.FullName)" }
    [ordered]@{file=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;
        signer=$signature.SignerCertificate.Subject;thumbprint=$signature.SignerCertificate.Thumbprint}
})
[ordered]@{signed_at=(Get-Date).ToUniversalTime().ToString('o');artifacts=$artifacts} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'development-signatures.json') -Encoding utf8
Write-Host 'PASS verified local development signatures; Windows security policy unchanged.'
