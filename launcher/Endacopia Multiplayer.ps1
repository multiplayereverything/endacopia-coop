# Endacopia Multiplayer launcher (host / join / solo).
# Runs the multiplayer build of the AGS engine (bin\EndacopiaMP.exe) on the
# game's own unmodified data. Settings are remembered per user.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ModDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$GameDir  = Join-Path (Split-Path -Parent $ModDir) 'Windows'
$GameExe  = Join-Path $GameDir 'Endacopia.exe'
$Engine   = Join-Path $ModDir 'bin\EndacopiaMP.exe'
$CfgDir   = Join-Path $env:LOCALAPPDATA 'EndacopiaMP'
$CfgFile  = Join-Path $CfgDir 'launcher.json'
New-Item -ItemType Directory -Force $CfgDir | Out-Null

$settings = @{ name = $env:USERNAME; port = 27960; address = ''; friend = ''; windowed = $true }
if (Test-Path $CfgFile) {
    try { (Get-Content $CfgFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $settings[$_.Name] = $_.Value } } catch {}
}

function Save-Settings {
    $settings | ConvertTo-Json | Set-Content -Encoding UTF8 $CfgFile
}

function Get-LocalAddresses {
    try {
        Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
            Select-Object -ExpandProperty IPAddress
    } catch { @() }
}

function Start-Engine([string[]]$extra) {
    if (-not (Test-Path $Engine))  { [Windows.Forms.MessageBox]::Show("Missing $Engine"); return }
    if (-not (Test-Path $GameExe)) { [Windows.Forms.MessageBox]::Show("Endacopia not found at $GameExe"); return }
    $argList = @("`"$GameExe`"")
    if ($settings.windowed) { $argList += '--windowed' }
    $argList += $extra
    $logDir = Join-Path $CfgDir 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $argList += "--log-file=all:info", "--log-file-path=`"$(Join-Path $logDir 'last-session.log')`""
    Start-Process -FilePath $Engine -WorkingDirectory $GameDir -ArgumentList $argList | Out-Null
}

$form = New-Object Windows.Forms.Form
$form.Text = 'Endacopia Multiplayer'
$form.Size = New-Object Drawing.Size(430, 470)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false

function Add-Label($text, $x, $y, $w = 380, $h = 20) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $h)
    $form.Controls.Add($l); $l
}
function Add-Box($text, $x, $y, $w) {
    $b = New-Object Windows.Forms.TextBox
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, 22)
    $form.Controls.Add($b); $b
}
function Add-Button($text, $x, $y, $w = 120) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, 30)
    $form.Controls.Add($b); $b
}

Add-Label 'Your name' 15 15 100 | Out-Null
$nameBox = Add-Box $settings.name 120 12 150
Add-Label 'Port' 285 15 40 | Out-Null
$portBox = Add-Box ([string]$settings.port) 325 12 70

$winBox = New-Object Windows.Forms.CheckBox
$winBox.Text = 'Windowed'; $winBox.Checked = [bool]$settings.windowed
$winBox.Location = New-Object Drawing.Point(120, 40); $form.Controls.Add($winBox)

Add-Label 'STEAM (easiest) - no IPs, no port forwarding. Steam must be running.' 15 72 | Out-Null
$steamHostBtn = Add-Button 'Host on Steam' 15 95 130
Add-Label 'Friend (optional):' 160 101 105 | Out-Null
$friendBox = Add-Box $settings.friend 265 98 140
$steamJoinBtn = Add-Button 'Join Steam friend' 275 128 130

Add-Label 'BY IP - LAN, or the host forwards the port / uses Tailscale or ZeroTier.' 15 175 | Out-Null
$ips = (Get-LocalAddresses) -join ', '
Add-Label "Your local IP: $ips" 15 195 400 | Out-Null
$hostBtn = Add-Button 'Host game' 15 218
Add-Label 'Host''s IP address (e.g. 192.168.1.20 or 100.x.y.z):' 15 262 | Out-Null
$addrBox = Add-Box $settings.address 15 284 255
$joinBtn = Add-Button 'Join game' 280 281

Add-Label 'Solo - play Endacopia normally with this engine.' 15 335 | Out-Null
$soloBtn = Add-Button 'Play solo' 15 357

function Collect {
    $settings.name = $nameBox.Text.Trim()
    $p = 0; if ([int]::TryParse($portBox.Text, [ref]$p) -and $p -gt 0 -and $p -lt 65536) { $settings.port = $p }
    $settings.address = $addrBox.Text.Trim()
    $settings.friend = $friendBox.Text.Trim()
    $settings.windowed = $winBox.Checked
    Save-Settings
}

$hostBtn.Add_Click({
    Collect
    Start-Engine @('--mp-host', [string]$settings.port, '--mp-name', "`"$($settings.name)`"")
    $form.Close()
})
$joinBtn.Add_Click({
    Collect
    if (-not $settings.address) { [Windows.Forms.MessageBox]::Show('Enter the host''s IP address first.'); return }
    $target = $settings.address
    if ($target -notmatch ':\d+$') { $target = "$($target):$($settings.port)" }
    Start-Engine @('--mp-join', $target, '--mp-name', "`"$($settings.name)`"")
    $form.Close()
})
$steamHostBtn.Add_Click({
    Collect
    Start-Engine @('--mp-host', [string]$settings.port, '--mp-steam', '--mp-name', "`"$($settings.name)`"")
    $form.Close()
})
$steamJoinBtn.Add_Click({
    Collect
    $target = if ($settings.friend) { "`"steam:$($settings.friend)`"" } else { 'steam' }
    Start-Engine @('--mp-join', $target, '--mp-name', "`"$($settings.name)`"")
    $form.Close()
})
$soloBtn.Add_Click({ Collect; Start-Engine @(); $form.Close() })

[void]$form.ShowDialog()
