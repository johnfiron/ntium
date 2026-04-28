[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('baseline', 'idle', 'active', 'burst', 'all')]
    [string]$Scenario,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [string]$Profile = 'GeneralProfile',

    [int]$BaselineDurationSec = 15,
    [int]$IdleDurationSec = 45,
    [int]$ActiveDurationSec = 60,
    [int]$BurstDurationSec = 20,

    [string]$WprPath = 'wpr.exe',

    [switch]$SkipUserPrompt,

    [string]$SessionTag = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-ScenarioList {
    param([string]$RequestedScenario)

    if ($RequestedScenario -eq 'all') {
        return @('baseline', 'idle', 'active', 'burst')
    }

    return @($RequestedScenario)
}

function Get-ScenarioDurationSec {
    param([string]$Name)

    switch ($Name) {
        'baseline' { return $BaselineDurationSec }
        'idle'     { return $IdleDurationSec }
        'active'   { return $ActiveDurationSec }
        'burst'    { return $BurstDurationSec }
        default    { throw "Unsupported scenario: $Name" }
    }
}

function Ensure-WprInstalled {
    param([string]$Executable)

    $null = Get-Command $Executable -ErrorAction Stop
}

function New-OutputDirectory {
    param([string]$Path)

    if (-not (Test-Path -Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Invoke-WprStart {
    param(
        [string]$Executable,
        [string]$ProfileName
    )

    Write-Host "[perf] Starting WPR with profile '$ProfileName'"
    & $Executable -start $ProfileName -filemode
    if ($LASTEXITCODE -ne 0) {
        throw "wpr start failed with exit code $LASTEXITCODE"
    }
}

function Invoke-WprStop {
    param(
        [string]$Executable,
        [string]$TracePath
    )

    Write-Host "[perf] Stopping WPR and saving trace: $TracePath"
    & $Executable -stop $TracePath
    if ($LASTEXITCODE -ne 0) {
        throw "wpr stop failed with exit code $LASTEXITCODE"
    }
}

function Write-ScenarioMarker {
    param(
        [string]$Name,
        [string]$Tag,
        [string]$Phase
    )

    # Placeholder marker. Integrate with app-specific marker API or log channel if available.
    # Example desired marker events:
    #   Perf.ScenarioStart {scenario=idle, tag=...}
    #   Perf.ScenarioEnd   {scenario=idle, tag=...}
    $ts = Get-Date -Format o
    Write-Host "[perf] [$ts] marker phase=$Phase scenario=$Name tag=$Tag"
}

function Invoke-ScenarioCapture {
    param(
        [string]$Name,
        [int]$DurationSec,
        [string]$Executable,
        [string]$ProfileName,
        [string]$Dir,
        [string]$Tag,
        [bool]$PromptUser
    )

    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $suffix = if ([string]::IsNullOrWhiteSpace($Tag)) { '' } else { "_$Tag" }
    $tracePath = Join-Path $Dir ("wpr_{0}_{1}{2}.etl" -f $Name, $stamp, $suffix)

    if ($PromptUser) {
        Write-Host "[perf] Prepare workload for scenario '$Name'. Press Enter to begin capture..."
        [void](Read-Host)
    }

    Write-Host "[perf] Scenario '$Name' duration=${DurationSec}s"
    Write-ScenarioMarker -Name $Name -Tag $Tag -Phase 'start'

    Invoke-WprStart -Executable $Executable -ProfileName $ProfileName
    Start-Sleep -Seconds $DurationSec
    Invoke-WprStop -Executable $Executable -TracePath $tracePath

    Write-ScenarioMarker -Name $Name -Tag $Tag -Phase 'end'

    return $tracePath
}

Ensure-WprInstalled -Executable $WprPath
New-OutputDirectory -Path $OutputDir

$scenarios = Resolve-ScenarioList -RequestedScenario $Scenario
$promptUser = -not $SkipUserPrompt.IsPresent

$manifest = @()

foreach ($name in $scenarios) {
    $durationSec = Get-ScenarioDurationSec -Name $name
    $trace = Invoke-ScenarioCapture `
        -Name $name `
        -DurationSec $durationSec `
        -Executable $WprPath `
        -ProfileName $Profile `
        -Dir $OutputDir `
        -Tag $SessionTag `
        -PromptUser $promptUser

    $manifest += [pscustomobject]@{
        scenario = $name
        duration_sec = $durationSec
        profile = $Profile
        trace_path = $trace
        session_tag = $SessionTag
        captured_at_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
}

$manifestPath = Join-Path $OutputDir 'capture_manifest.json'
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Path $manifestPath -Encoding UTF8

Write-Host "[perf] Capture complete. Manifest: $manifestPath"
Write-Host "[perf] Next step: run ETW parser to emit metrics JSON/CSV for check_thresholds.py"
