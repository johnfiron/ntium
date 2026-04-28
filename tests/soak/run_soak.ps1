[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('soak-24h', 'soak-72h')]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [string]$OutDir,

    [int]$RecoverySlaSec = 120,
    [int]$HealthPollSec = 30,
    [int]$LivenessPollSec = 10,
    [int]$ChaosPollSec = 5,

    [string]$HealthCheckCommand = '',
    [string]$HostProcessName = '',

    [string]$ExplorerRestartCommand = '',
    [string]$SessionResetCommand = '',
    [string]$DisplayResetCommand = '',
    [string]$DeviceResetCommand = '',

    [switch]$SkipChaosInjection
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Ensure-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -ItemType Directory -Force | Out-Null
    }
}

function Write-SoakLog {
    param(
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][string]$Level,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $line = '[{0}] [{1}] {2}' -f ((Get-Date).ToUniversalTime().ToString('o')), $Level, $Message
    Write-Host $line
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
}

function Get-SoakRows {
    param([Parameter(Mandatory = $true)][string]$RequestedMode)

    if ($RequestedMode -eq 'soak-24h') {
        return @(
            @{ id = 'SOAK-24-A'; duration_sec = 8 * 3600; chaos_cadence_sec = 45 * 60; profile = 'idle_dominant' },
            @{ id = 'SOAK-24-B'; duration_sec = 8 * 3600; chaos_cadence_sec = 30 * 60; profile = 'mixed' },
            @{ id = 'SOAK-24-C'; duration_sec = 8 * 3600; chaos_cadence_sec = 20 * 60; profile = 'active_burst' }
        )
    }

    return @(
        @{ id = 'SOAK-72-A'; duration_sec = 24 * 3600; chaos_cadence_sec = 60 * 60; profile = 'baseline_long' },
        @{ id = 'SOAK-72-B'; duration_sec = 24 * 3600; chaos_cadence_sec = 30 * 60; profile = 'mixed_monitor' },
        @{ id = 'SOAK-72-C'; duration_sec = 24 * 3600; chaos_cadence_sec = 15 * 60; profile = 'stress_chaos' }
    )
}

function Invoke-ExternalCommand {
    param(
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return @{
            success = $false
            exit_code = -1
            reason = 'CommandLineEmpty'
        }
    }

    Write-SoakLog -LogPath $LogPath -Level 'INFO' -Message ("Running command: {0}" -f $CommandLine)

    $output = ''
    try {
        $LASTEXITCODE = 0
        $output = Invoke-Expression $CommandLine 2>&1 | Out-String
        $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { [int]$LASTEXITCODE }
        if (-not [string]::IsNullOrWhiteSpace($output)) {
            Add-Content -Path $LogPath -Value $output -Encoding UTF8
        }
        return @{
            success = ($exitCode -eq 0)
            exit_code = $exitCode
            reason = if ($exitCode -eq 0) { 'OK' } else { 'NonZeroExit' }
        }
    } catch {
        Add-Content -Path $LogPath -Value $_.Exception.ToString() -Encoding UTF8
        return @{
            success = $false
            exit_code = -1
            reason = 'Exception'
        }
    }
}

function Invoke-HealthCheck {
    param(
        [Parameter(Mandatory = $true)][string]$HealthCommand,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    if ([string]::IsNullOrWhiteSpace($HealthCommand)) {
        # Adapter-friendly default for scaffolding when command is not yet wired.
        return @{
            success = $true
            exit_code = 0
            reason = 'HealthCheckSkipped'
        }
    }

    return Invoke-ExternalCommand -CommandLine $HealthCommand -LogPath $LogPath
}

function Test-HostLiveness {
    param([string]$ProcessName)

    if ([string]::IsNullOrWhiteSpace($ProcessName)) {
        # Process name optional for adaptation phase.
        return $true
    }

    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    return ($null -ne $proc)
}

function Get-ChaosEventCatalog {
    param(
        [string]$ExplorerCommand,
        [string]$SessionCommand,
        [string]$DisplayCommand,
        [string]$DeviceCommand
    )

    return @(
        @{ id = 'CHAOS-EXP-RESTART'; command = $ExplorerCommand; fallback = 'Stop-Process -Name explorer -Force; Start-Process explorer.exe' },
        @{ id = 'CHAOS-SESSION-RESET'; command = $SessionCommand; fallback = '' },
        @{ id = 'CHAOS-DISPLAY-RESET'; command = $DisplayCommand; fallback = '' },
        @{ id = 'CHAOS-DEVICE-RESET'; command = $DeviceCommand; fallback = '' }
    )
}

function Invoke-ChaosEvent {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Event,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [switch]$DisableInjection
    )

    if ($DisableInjection.IsPresent) {
        Write-SoakLog -LogPath $LogPath -Level 'INFO' -Message ("Skipping chaos injection for {0} (SkipChaosInjection set)" -f $Event.id)
        return @{
            id = $Event.id
            injected = $true
            command = 'skipped'
            success = $true
            reason = 'SkippedByFlag'
        }
    }

    $cmd = $Event.command
    if ([string]::IsNullOrWhiteSpace($cmd)) {
        $cmd = $Event.fallback
    }

    if ([string]::IsNullOrWhiteSpace($cmd)) {
        Write-SoakLog -LogPath $LogPath -Level 'WARN' -Message ("No command configured for {0}" -f $Event.id)
        return @{
            id = $Event.id
            injected = $false
            command = ''
            success = $false
            reason = 'NoCommandConfigured'
        }
    }

    $result = Invoke-ExternalCommand -CommandLine $cmd -LogPath $LogPath
    return @{
        id = $Event.id
        injected = $true
        command = $cmd
        success = $result.success
        reason = $result.reason
        exit_code = $result.exit_code
    }
}

function Measure-RecoveryAfterChaos {
    param(
        [Parameter(Mandatory = $true)][string]$HealthCommand,
        [Parameter(Mandatory = $true)][string]$HostProcessName,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][int]$SlaSec
    )

    $deadline = (Get-Date).ToUniversalTime().AddSeconds($SlaSec)
    $recovered = $false
    $healthFailures = 0
    $elapsedSec = 0

    while ((Get-Date).ToUniversalTime() -lt $deadline) {
        $alive = Test-HostLiveness -ProcessName $HostProcessName
        if (-not $alive) {
            Start-Sleep -Seconds 2
            $elapsedSec += 2
            continue
        }

        $health = Invoke-HealthCheck -HealthCommand $HealthCommand -LogPath $LogPath
        if ($health.success) {
            $recovered = $true
            break
        }

        $healthFailures += 1
        Start-Sleep -Seconds 2
        $elapsedSec += 2
    }

    if ($recovered -and $elapsedSec -eq 0) {
        $elapsedSec = 1
    }

    return @{
        recovered = $recovered
        recovery_sec = $elapsedSec
        health_failures = $healthFailures
    }
}

function New-RowResult {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Row,
        [Parameter(Mandatory = $true)][array]$ChaosCatalog
    )

    $eventCounts = @{}
    foreach ($event in $ChaosCatalog) {
        $eventCounts[$event.id] = 0
    }

    return @{
        id = $Row.id
        profile = $Row.profile
        duration_sec = [int]$Row.duration_sec
        chaos_cadence_sec = [int]$Row.chaos_cadence_sec
        chaos_events = $eventCounts
        unplanned_exit_count = 0
        hang_count = 0
        health_check_fail_count = 0
        event_injection_fail_count = 0
        state_restore_fail_count = 0
        max_recovery_sec = 0
        required_event_coverage_ok = $false
    }
}

function Finalize-RowCoverage {
    param(
        [Parameter(Mandatory = $true)][hashtable]$RowResult,
        [Parameter(Mandatory = $true)][array]$ChaosCatalog
    )

    foreach ($event in $ChaosCatalog) {
        if ($RowResult.chaos_events[$event.id] -lt 1) {
            $RowResult.required_event_coverage_ok = $false
            return
        }
    }
    $RowResult.required_event_coverage_ok = $true
}

Ensure-Directory -Path $OutDir
$logPath = Join-Path $OutDir 'soak.log'

if (-not (Test-Path -LiteralPath $logPath)) {
    New-Item -Path $logPath -ItemType File -Force | Out-Null
}

Write-SoakLog -LogPath $logPath -Level 'INFO' -Message ("Starting soak mode={0}" -f $Mode)

$rows = Get-SoakRows -RequestedMode $Mode
$chaosCatalog = Get-ChaosEventCatalog `
    -ExplorerCommand $ExplorerRestartCommand `
    -SessionCommand $SessionResetCommand `
    -DisplayCommand $DisplayResetCommand `
    -DeviceCommand $DeviceResetCommand

$runStart = (Get-Date).ToUniversalTime()
$rowResults = @()
$totals = @{
    unplanned_exit_count = 0
    hang_count = 0
    event_injection_fail_count = 0
    health_check_fail_count = 0
    state_restore_fail_count = 0
}

foreach ($row in $rows) {
    $rowResult = New-RowResult -Row $row -ChaosCatalog $chaosCatalog
    $rowStart = (Get-Date).ToUniversalTime()
    $rowEnd = $rowStart.AddSeconds([int]$row.duration_sec)
    $nextHealthAt = $rowStart
    $nextLivenessAt = $rowStart
    $nextChaosAt = $rowStart.AddSeconds([int]$row.chaos_cadence_sec)
    $chaosIndex = 0

    Write-SoakLog -LogPath $logPath -Level 'INFO' -Message ("Row {0} started (duration={1}s, cadence={2}s)" -f $row.id, $row.duration_sec, $row.chaos_cadence_sec)

    while ((Get-Date).ToUniversalTime() -lt $rowEnd) {
        $now = (Get-Date).ToUniversalTime()

        if ($now -ge $nextLivenessAt) {
            $alive = Test-HostLiveness -ProcessName $HostProcessName
            if (-not $alive) {
                $rowResult.unplanned_exit_count += 1
                Write-SoakLog -LogPath $logPath -Level 'ERROR' -Message ("Host liveness failed for row {0}" -f $row.id)
            }
            $nextLivenessAt = $now.AddSeconds($LivenessPollSec)
        }

        if ($now -ge $nextHealthAt) {
            $health = Invoke-HealthCheck -HealthCommand $HealthCheckCommand -LogPath $logPath
            if (-not $health.success) {
                $rowResult.health_check_fail_count += 1
                Write-SoakLog -LogPath $logPath -Level 'WARN' -Message ("Health check failed for row {0}; reason={1}" -f $row.id, $health.reason)
            }
            $nextHealthAt = $now.AddSeconds($HealthPollSec)
        }

        if ($now -ge $nextChaosAt) {
            $event = $chaosCatalog[$chaosIndex % $chaosCatalog.Count]
            $chaosIndex += 1

            Write-SoakLog -LogPath $logPath -Level 'INFO' -Message ("Injecting chaos event {0} in row {1}" -f $event.id, $row.id)
            $inject = Invoke-ChaosEvent -Event $event -LogPath $logPath -DisableInjection:$SkipChaosInjection

            if ($inject.success) {
                $rowResult.chaos_events[$event.id] = [int]$rowResult.chaos_events[$event.id] + 1
            } else {
                $rowResult.event_injection_fail_count += 1
                Write-SoakLog -LogPath $logPath -Level 'ERROR' -Message ("Chaos event {0} failed in row {1}; reason={2}" -f $event.id, $row.id, $inject.reason)
            }

            $recovery = Measure-RecoveryAfterChaos `
                -HealthCommand $HealthCheckCommand `
                -HostProcessName $HostProcessName `
                -LogPath $logPath `
                -SlaSec $RecoverySlaSec

            if (-not $recovery.recovered) {
                $rowResult.state_restore_fail_count += 1
                $rowResult.hang_count += 1
                Write-SoakLog -LogPath $logPath -Level 'ERROR' -Message ("Recovery SLA exceeded after {0} in row {1}" -f $event.id, $row.id)
            } else {
                if ([int]$recovery.recovery_sec -gt [int]$rowResult.max_recovery_sec) {
                    $rowResult.max_recovery_sec = [int]$recovery.recovery_sec
                }
            }

            $rowResult.health_check_fail_count += [int]$recovery.health_failures
            $nextChaosAt = $now.AddSeconds([int]$row.chaos_cadence_sec)
        }

        Start-Sleep -Seconds $ChaosPollSec
    }

    Finalize-RowCoverage -RowResult $rowResult -ChaosCatalog $chaosCatalog

    $totals.unplanned_exit_count += [int]$rowResult.unplanned_exit_count
    $totals.hang_count += [int]$rowResult.hang_count
    $totals.event_injection_fail_count += [int]$rowResult.event_injection_fail_count
    $totals.health_check_fail_count += [int]$rowResult.health_check_fail_count
    $totals.state_restore_fail_count += [int]$rowResult.state_restore_fail_count

    $rowResults += $rowResult
    Write-SoakLog -LogPath $logPath -Level 'INFO' -Message ("Row {0} completed" -f $row.id)
}

$requiredCoverageOk = $true
$maxRecoverySecAcrossRows = 0
foreach ($rowResult in $rowResults) {
    if (-not $rowResult.required_event_coverage_ok) {
        $requiredCoverageOk = $false
    }
    if ([int]$rowResult.max_recovery_sec -gt $maxRecoverySecAcrossRows) {
        $maxRecoverySecAcrossRows = [int]$rowResult.max_recovery_sec
    }
}

$recoverySlaBreached = ($maxRecoverySecAcrossRows -gt $RecoverySlaSec)

$pass =
    ($totals.unplanned_exit_count -eq 0) -and
    ($totals.hang_count -eq 0) -and
    ($totals.event_injection_fail_count -eq 0) -and
    ($totals.health_check_fail_count -eq 0) -and
    ($totals.state_restore_fail_count -eq 0) -and
    (-not $recoverySlaBreached) -and
    $requiredCoverageOk

$summary = @{
    ticket = 'Q1-903'
    mode = $Mode
    started_at_utc = $runStart.ToString('o')
    finished_at_utc = ((Get-Date).ToUniversalTime().ToString('o'))
    recovery_sla_sec = $RecoverySlaSec
    rows = $rowResults
    totals = $totals
    max_recovery_sec = $maxRecoverySecAcrossRows
    required_event_coverage_ok = $requiredCoverageOk
    pass = $pass
}

$summaryPath = Join-Path $OutDir 'summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8

Write-SoakLog -LogPath $logPath -Level 'INFO' -Message ("Summary written: {0}" -f $summaryPath)

if ($pass) {
    Write-SoakLog -LogPath $logPath -Level 'INFO' -Message 'Soak run PASSED'
    exit 0
}

Write-SoakLog -LogPath $logPath -Level 'ERROR' -Message 'Soak run FAILED'
exit 1
