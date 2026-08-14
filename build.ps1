# MuuTask を単体の exe に固めて、README.txt と一緒に zip にする。
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# 出来上がるもの:
#   dist\MuuTask.exe              単体で動く実行ファイル
#   dist\MuuTask-<版>.zip         配布用 (MuuTask.exe + README.txt)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = Join-Path $root '.venv\Scripts\python.exe'

if (-not (Test-Path $python)) {
    throw '.venv がありません。先に setup.ps1 を実行してください。'
}

# ビルド専用の依存 (requirements.txt には入れない)
& $python -c 'import PyInstaller' 2>$null
if (-not $?) {
    Write-Host 'PyInstaller をインストールしています...'
    & $python -m pip install pyinstaller
}

# 版番号は config.py の APP_VERSION を唯一の出どころにする
$version = & $python -c 'import config; print(config.APP_VERSION)'
Write-Host "MuuTask $version をビルドします"

& $python (Join-Path $root 'make_icon.py')

# 使っていないのに大きいものを外す。外した分は下のコメントの実測値。
# アルバム アートは WinRT から JPEG / PNG / BMP で来るので、それ以外の
# 重い画像コーデックは要らない。ネットワークも一切使わない。
$excludes = @(
    'PIL.AvifImagePlugin', 'PIL._avif',      # AVIF コーデック (4.1MB)
    'PIL._imagingft',                        # FreeType (0.9MB)。文字は Tk が描く
    'PIL.ImageQt',                           # Qt 連携 (未使用)
    'ssl', '_ssl', '_hashlib',               # OpenSSL 一式 (2.1MB)
    'setuptools', 'pkg_resources', '_distutils_hack',
    'unittest', 'doctest', 'pydoc', 'pdb', 'lib2to3',
    'email', 'http', 'urllib.request', 'xmlrpc', 'ftplib',
    'sqlite3', 'curses', 'idlelib', 'test', 'tkinter.test'
)
$excludeArgs = $excludes | ForEach-Object { '--exclude-module'; $_ }

$dist = Join-Path $root 'dist'
$work = Join-Path $root 'build'
& $python -m PyInstaller `
    --noconfirm --clean `
    --onefile --windowed `
    --name MuuTask `
    --icon (Join-Path $root 'muutask.ico') `
    --hidden-import pystray._win32 `
    @excludeArgs `
    --distpath $dist --workpath $work --specpath $work `
    (Join-Path $root 'app.py')

$exe = Join-Path $dist 'MuuTask.exe'
if (-not (Test-Path $exe)) { throw 'exe が生成されませんでした。' }

# zip の中は MuuTask-<版>\ の 1 階層にまとめる (展開時に散らからないように)
$stage = Join-Path $work "package\MuuTask-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $root 'README.txt') $stage

$zip = Join-Path $dist "MuuTask-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip

Write-Host ''
Write-Host "完了しました:"
Write-Host "  $exe"
Write-Host "  $zip"
