#requires -Version 7.0
param([string]$PythonVersion = '3.12', [string]$PythonExecutable = '', [string]$Wheel = 'git+https://github.com/raspie10032/SWEGCA-VRS-MCP.git@main')
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'This installer is for Windows. Use the README Linux commands on Linux.' }
$vrsBase = Join-Path $env:LOCALAPPDATA 'SWEGCA'
$vrsEnv = Join-Path $vrsBase 'VRS2-venv'
$vrsStore = Join-Path $vrsBase 'VRS2'
if ($PythonExecutable) { & $PythonExecutable -m venv $vrsEnv }
else { & py "-$PythonVersion" -m venv $vrsEnv }
if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed.' }
$vrsPython = Join-Path $vrsEnv 'Scripts/python.exe'
& $vrsPython -m pip install --upgrade $Wheel
if ($LASTEXITCODE -ne 0) { throw 'VRS2 wheel installation failed.' }
$vrsCommand = Join-Path $vrsEnv 'Scripts/swegca-vrs2-mcp.exe'
& $vrsCommand --help
if ($LASTEXITCODE -ne 0) { throw 'VRS2 executable check failed.' }
$vrsConfig = @{ mcpServers = @{ 'vrs2-memory' = @{ command = $vrsCommand; args = @('--state-dir', $vrsStore, '--allow-ingest') } } }
$vrsSnippet = Join-Path $vrsBase 'vrs2-claude-config.json'
$vrsJson = $vrsConfig | ConvertTo-Json -Depth 8
Set-Content -LiteralPath $vrsSnippet -Value $vrsJson -Encoding utf8NoBOM
Write-Host "Merge this snippet into Claude Desktop's existing mcpServers configuration: $vrsSnippet"
Write-Output $vrsJson
