$ErrorActionPreference = 'Stop'

$bluetooth = Get-PnpDevice -Class Bluetooth -Status OK -ErrorAction SilentlyContinue
if (-not $bluetooth) {
    Write-Error 'Bluetooth laptop belum aktif atau adapter tidak ditemukan. Aktifkan Bluetooth lalu jalankan kembali.'
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Error 'Python tidak ditemukan di PATH.'
}
& $python.Source (Join-Path $PSScriptRoot 'server.py')
