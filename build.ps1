# MuuTask を exe 1 つに固めて、README.txt と一緒に zip にする。
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# 出来上がるもの:
#   dist\MuuTask\                 そのまま動く配布フォルダー
#     MuuTask.exe                 実行ファイル (CRT は静的リンク・約 0.4 MB)
#     README.txt                  取扱説明 (版番号を埋めたもの)
#     config.json                 設定の初期値
#   dist\MuuTask-<版>.zip         配布用 (上のフォルダーを固めたもの)
#
# 古い zip は新しい方から KEEP_ZIPS 個だけ残し、それより古いものは消す。

$ErrorActionPreference = 'Stop'
#: dist に残しておく配布物の数。1 つ前の版に戻れる余地は持たせておく
$KEEP_ZIPS = 3
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# ---------------------------------------------------------------- 道具立て

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio Build Tools が見つかりません。次で入ります:
  winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
}
$vs = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vs) { throw 'MSVC (C++ ビルド ツール) が入っていません。' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat がありません: $vcvars" }

# CMake は VS 同梱のものを優先する (MinGW 側のものが PATH に居ても迷わない)
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

# 動いている exe は上書きできない。先に気づかないと、古い exe が新しい版番号の
# zip に入ってしまう
$running = Get-Process -Name MuuTask -ErrorAction SilentlyContinue
if ($running) {
    throw "MuuTask が起動中です (PID $($running.Id -join ', '))。終了してからビルドしてください。"
}

# ---------------------------------------------------------------- 版番号

# 版番号は src\version.h の MUUTASK_VERSION_NUM を唯一の出どころにする
$header = Get-Content (Join-Path $root 'src\version.h') -Raw
if ($header -notmatch '#define\s+MUUTASK_VERSION_NUM\s+(\d+)') {
    throw 'src\version.h から版番号を読めませんでした。'
}
$version = 'v' + $Matches[1]
Write-Host "MuuTask $version をビルドします"

# アイコンは Python 版と同じ音符を使う (make_icon.py が書き出したもの)
$icon = Join-Path $root 'muutask.ico'
if (-not (Test-Path $icon)) { throw "アイコンがありません: $icon" }

# ---------------------------------------------------------------- ビルド

$dist = Join-Path $root 'dist'
$work = Join-Path $root 'build'
$started = Get-Date

$configure = "`"$cmake`" -S `"$root`" -B `"$work`" -G Ninja -DCMAKE_BUILD_TYPE=Release"
$compile = "`"$cmake`" --build `"$work`""
cmd /c "`"$vcvars`" >nul 2>&1 && $configure && $compile"
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました (終了コード $LASTEXITCODE)。" }

$built = Join-Path $work 'MuuTask.exe'
if (-not (Test-Path $built)) { throw 'exe が生成されませんでした。' }
# 前回の exe が残っているだけ、という取り違えを防ぐ。ninja は変更が無ければ
# 作り直さないので「今回より新しいか」では見られない。ソースより新しいかで見る。
$newest = Get-ChildItem (Join-Path $root 'src') -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ((Get-Item $built).LastWriteTime -lt $newest.LastWriteTime) {
    throw "exe がソースより古いままです ($($newest.Name) の方が新しい): $built"
}

# ---------------------------------------------------------------- 配布フォルダー

$appdir = Join-Path $dist 'MuuTask'
# ここから直に動かしていることがあるので、設定は作り直しても持ち越す。
# (zip に入れる方は、後で既定値に戻したものを書き直す)
$keep = Join-Path $appdir 'config.json'
$kept = if (Test-Path $keep) { [IO.File]::ReadAllBytes($keep) } else { $null }
if (Test-Path $appdir) { Remove-Item -Recurse -Force $appdir }
New-Item -ItemType Directory -Force $appdir | Out-Null
Copy-Item $built $appdir

# README.txt の版番号は置換で埋める (version.h と二重管理にしないため)
$readme = [IO.File]::ReadAllText((Join-Path $root 'README.txt'))
if ($readme -notmatch '@VERSION@') { throw 'README.txt に @VERSION@ がありません。' }
$readme = $readme -replace '@VERSION@', $version.TrimStart('v')
[IO.File]::WriteAllText((Join-Path $appdir 'README.txt'), $readme, (New-Object Text.UTF8Encoding $true))

# config.json も同梱する。手で編集する項目 (bar_width など) の見本になるし、
# 設定が exe の隣にあることも見て分かる。値は exe 自身に書かせるので、Config の
# 定義と二重管理にならない。
#
# GUI サブシステムの exe なので、& で呼ぶと終わるのを待たずに戻ってしまう。
# Start-Process -Wait で待つこと。
function Write-DefaultConfig([string]$folder) {
    $emit = Start-Process -FilePath (Join-Path $folder 'MuuTask.exe') `
        -ArgumentList '--emit-config' -PassThru -Wait
    if ($emit.ExitCode -ne 0) {
        throw "config.json の初期値を作れませんでした (終了コード $($emit.ExitCode))。"
    }
    if (-not (Test-Path (Join-Path $folder 'config.json'))) {
        throw 'config.json が書き出されませんでした。'
    }
}

if ($null -ne $kept) {
    [IO.File]::WriteAllBytes((Join-Path $appdir 'config.json'), $kept)
    Write-Host '  (dist の config.json は前のものを引き継ぎました)'
} else {
    Write-DefaultConfig $appdir
}

# ---------------------------------------------------------------- zip

# zip の中は MuuTask-<版>\ の 1 階層にまとめる (展開時に散らからないように)。
# その 1 階層だけを入れた親フォルダーを丸ごと固めると、そのまま中身になる
$package = Join-Path $work 'package'
if (Test-Path $package) { Remove-Item -Recurse -Force $package }
$stage = Join-Path $package "MuuTask-$version"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse (Join-Path $appdir '*') $stage
# 配る方は必ず既定値にする (開発機の設定を混ぜない)
Write-DefaultConfig $stage

$zip = Join-Path $dist "MuuTask-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# Compress-Archive はコピー直後のファイルを走査しているウイルス対策と鉢合わせて
# "used by another process" で落ちることがある (v21 で実際に発生)。.NET の
# CreateFromDirectory は速く、掴まれていても少し待てば通る
Add-Type -AssemblyName System.IO.Compression.FileSystem
for ($try = 1; ; $try++) {
    try {
        [IO.Compression.ZipFile]::CreateFromDirectory($package, $zip)
        break
    } catch [IO.IOException], [UnauthorizedAccessException] {
        if ($try -ge 5) { throw }
        if (Test-Path $zip) { Remove-Item -Force $zip }
        Write-Host "zip を作れませんでした。1 秒待って作り直します ($try/5)"
        Start-Sleep -Seconds 1
    }
}

# 古い配布物は溜め込まない。名前順だと v10 が v9 より前に来てしまうので、
# 作られた順で見る
$stale = Get-ChildItem -Path $dist -Filter 'MuuTask-v*.zip' |
    Sort-Object LastWriteTime -Descending | Select-Object -Skip $KEEP_ZIPS
foreach ($file in $stale) {
    Write-Host "古い配布物を消します: $($file.Name)"
    Remove-Item -Force $file.FullName
}

$size = (Get-Item (Join-Path $appdir 'MuuTask.exe')).Length / 1KB
Write-Host ''
Write-Host "完了しました:"
Write-Host ("  {0}  (exe {1:N0} KB)" -f $appdir, $size)
Write-Host "  $zip"
