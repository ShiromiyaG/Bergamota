# Builds the port's package with Bergamota's port tool (sdk/port.py
# package): dist\<Name>\ and dist\<Name>-<version>-windows-x64.zip. What goes
# in, and from where, is port.toml's (Bergamota's docs/porting.md, "The
# package").
#
#   pwsh packaging/package.ps1 [-Runtime <checkout>] [-RuntimeBuild <folder>] [-Version 0.1.0]
#
# -Runtime is the external/runtime submodule by default.

param(
    [string]$Runtime = (Join-Path $PSScriptRoot '..\external\runtime'),
    # where the runtime is built (its build\ by default)
    [string]$RuntimeBuild = '',
    [string]$Version = '0.1.0'
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$tool = Join-Path (Resolve-Path $Runtime) 'sdk\port.py'
if (-not (Test-Path $tool)) { throw "no ${tool}: a Bergamota with sdk/ is needed (-Runtime <checkout>)" }
$more = @()
if ($RuntimeBuild) { $more += '--runtime-build', $RuntimeBuild }
& python -I $tool package --port $root --version $Version @more
if ($LASTEXITCODE) { throw 'packaging failed' }
