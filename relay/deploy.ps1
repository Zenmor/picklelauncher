# PowerShell Deploy Script for PickleLauncher Shared Packs Relay
# Reads secrets from the file named by CF_DEPLOY_SECRETS (keep it outside the repository) without echoing them.

$ErrorActionPreference = "Stop"

$SecretsPath = $env:CF_DEPLOY_SECRETS
if (-not $SecretsPath) {
    Write-Error "Set CF_DEPLOY_SECRETS to the path of your Cloudflare secrets file."
    exit 1
}

if (-not (Test-Path $SecretsPath)) {
    Write-Error "Secrets file not found: $SecretsPath`nPlease create it with fields 'account_id' and 'api_token'."
    exit 1
}

try {
    $raw = Get-Content -Raw -Path $SecretsPath
    $secrets = ConvertFrom-Json $raw
} catch {
    Write-Error "Failed to parse secrets file: $SecretsPath"
    exit 1
}

if (-not $secrets.api_token -or -not $secrets.account_id) {
    Write-Error "Secrets file must contain 'api_token' and 'account_id'."
    exit 1
}

Write-Host "Configuring Cloudflare environment..."
$env:CLOUDFLARE_API_TOKEN = $secrets.api_token
$env:CLOUDFLARE_ACCOUNT_ID = $secrets.account_id

Set-Location -Path $PSScriptRoot

Write-Host "Deploying Relay Worker with Wrangler..."
# wrangler.local.toml (not committed) holds the real resource ids; wrangler.toml only has placeholders.
$config = @()
if (Test-Path (Join-Path $PSScriptRoot "wrangler.local.toml")) { $config = @("--config", "wrangler.local.toml") }
npx wrangler deploy @config

Write-Host "Deployment completed."
