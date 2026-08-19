# MuuTask を単体の exe に固めて、README.txt と一緒に zip にする。
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# 出来上がるもの:
#   dist\MuuTask.exe              単体で動く実行ファイル
#   dist\MuuTask-<版>.zip         配布用 (MuuTask.exe + README.txt)
#
# 古い zip は新しい方から KEEP_ZIPS 個だけ残し、それより古いものは消す。

$ErrorActionPreference = 'Stop'
#: dist に残しておく配布物の数。1 つ前の版に戻れる余地は持たせておく
$KEEP_ZIPS = 3
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

# 動いている exe は上書きできず、PyInstaller が WinError 5 で止まる。先に
# 気づかないと、古い exe が新しい版番号の zip に入ってしまう
$running = Get-Process -Name MuuTask -ErrorAction SilentlyContinue
if ($running) {
    throw "MuuTask が起動中です (PID $($running.Id -join ', '))。終了してからビルドしてください。"
}

# 版番号は config.py の APP_VERSION を唯一の出どころにする (v1 から 1 ずつ)
$version = 'v' + (& $python -c 'import config; print(config.APP_VERSION)')
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
$started = Get-Date

# onefile の exe は、起動のたびに中身を %TEMP% に展開してから走る。展開する
# ファイルが多いほど起動が重く (実測 1008 ファイル/27MB で 1 コアを 1.3 秒
# 占有)、ログイン直後だと他の常駐と重なってマウスが飛ぶ。使わない付属データ
# は spec を作ってから外す — データ ファイルは --exclude-module では消せない。
& $python -m PyInstaller.utils.cliutils.makespec `
    --onefile --windowed `
    --name MuuTask `
    --icon (Join-Path $root 'muutask.ico') `
    --hidden-import pystray._win32 `
    @excludeArgs `
    --specpath $work `
    (Join-Path $root 'app.py')
if ($LASTEXITCODE -ne 0) { throw "spec の作成に失敗しました (終了コード $LASTEXITCODE)。" }

$spec = Join-Path $work 'MuuTask.spec'
$text = [IO.File]::ReadAllText($spec)
$anchor = 'pyz = PYZ('
if ($text -notmatch [regex]::Escape($anchor)) { throw "spec の形が変わりました: $spec" }
# Tcl の時刻帯データ (609 個) と Tcl/Tk の訳文 (145 個)。時刻の書式も
# ダイアログも Tk には投げていないので、どちらも読まれない
$prune = @'
_drop = ('_tcl_data/tzdata', '_tcl_data/msgs', '_tk_data/msgs')
_kept = [d for d in a.datas if not d[0].replace('\\', '/').startswith(_drop)]
if len(_kept) == len(a.datas):
    raise SystemExit('外すつもりの Tcl/Tk データが見つかりませんでした')
a.datas = _kept

'@
[IO.File]::WriteAllText($spec, $text.Replace($anchor, $prune + $anchor))

& $python -m PyInstaller --noconfirm --clean --distpath $dist --workpath $work $spec

# $ErrorActionPreference は exe の失敗までは止めてくれないので、自分で見る
if ($LASTEXITCODE -ne 0) { throw "PyInstaller が失敗しました (終了コード $LASTEXITCODE)。" }

$exe = Join-Path $dist 'MuuTask.exe'
if (-not (Test-Path $exe)) { throw 'exe が生成されませんでした。' }
# 前回の exe が残っているだけ、という取り違えを防ぐ。ここを見ていないと
# 古いバイナリを新しい版番号で配ってしまう
if ((Get-Item $exe).LastWriteTime -lt $started) {
    throw "exe が更新されていません。前回のものが残っています: $exe"
}

# zip の中は MuuTask-<版>\ の 1 階層にまとめる (展開時に散らからないように)
$stage = Join-Path $work "package\MuuTask-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage

# README.txt の版番号は置換で埋める (config.py と二重管理にしないため)
$readme = [IO.File]::ReadAllText((Join-Path $root 'README.txt'))
if ($readme -notmatch '@VERSION@') { throw 'README.txt に @VERSION@ がありません。' }
$readme = $readme -replace '@VERSION@', $version.TrimStart('v')
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme, (New-Object Text.UTF8Encoding $true))

$zip = Join-Path $dist "MuuTask-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip

# 古い配布物は溜め込まない。名前順だと v10 が v9 より前に来てしまうので、
# 作られた順で見る
$stale = Get-ChildItem -Path $dist -Filter 'MuuTask-v*.zip' |
    Sort-Object LastWriteTime -Descending | Select-Object -Skip $KEEP_ZIPS
foreach ($file in $stale) {
    Write-Host "古い配布物を消します: $($file.Name)"
    Remove-Item -Force $file.FullName
}

Write-Host ''
Write-Host "完了しました:"
Write-Host "  $exe"
Write-Host "  $zip"
