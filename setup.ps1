# MuuTask の仮想環境を作って依存パッケージを入れる。
#   powershell -ExecutionPolicy Bypass -File setup.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$venv = Join-Path $root '.venv'

if (-not (Test-Path $venv)) {
    Write-Host '仮想環境を作成しています...'
    python -m venv $venv
}

$python = Join-Path $venv 'Scripts\python.exe'
& $python -m pip install --upgrade pip
& $python -m pip install -r (Join-Path $root 'requirements.txt')

Write-Host ''
Write-Host '完了しました。run.vbs をダブルクリックすると常駐します。'
