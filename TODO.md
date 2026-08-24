# TODO

やると決めたが、まだ手を付けていないもの。**「A-1 をやって」**のように番号で
指示できるようにしてある。

---

## A-1. バーをクリックして、鳴らしているアプリを前に出す

**状態:** 方針決定済み・未着手 (2026-08-23 に検討、2026-08-25 に決定)

ボタン以外のところを左クリックしたら、いま鳴らしているアプリのウィンドウを
アクティブ化する。UI Automation でブラウザーのタブまで切り替える案は
**入れない** (日本語 UI 依存・Chrome の UI 改変で壊れる・クロス プロセス UIA が
数百 ms かかる・comtypes を抱き込んで exe が膨らむ、で割に合わない)。
ブラウザーは**ウィンドウまで**で割り切る。

### 1. AUMID からウィンドウを引き当てる (winapi.py)

GSMTC がくれるのは AUMID (`source_app_user_model_id`) だけで、HWND は
付いてこない ([media_session.py](media_session.py) の `_app_id`)。自前で引く。

- **Win32 アプリ** (foobar2000 / VLC / AIMP / Chrome / Edge): AUMID は実行
  ファイル名 (`chrome.exe` など)。`EnumWindows` → `GetWindowThreadProcessId`
  → `QueryFullProcessImageNameW` のベース名で照合する。
- **パッケージ アプリ** (メディア プレーヤー / ストア版 Spotify): ウィンドウの
  `SHGetPropertyStoreForWindow` から `PKEY_AppUserModel_ID` を読む。無ければ
  プロセスの `GetApplicationUserModelId` (kernel32) で拾う。タスクバーが
  ボタンをまとめるのに使っているのと同じ値なので、これが一番素直。
- 候補が複数出たら `EnumWindows` の Z オーダー (前から後ろ) の先頭で、
  可視・`WS_EX_TOOLWINDOW` でない・`DWMWA_CLOAKED` でないものを採る。
  cloaked の除外は UWP で必須 (裏に空のウィンドウが残る)。

全部 ctypes で書けるので**依存は増やさない**。exe を 13.3 MB まで削った分を
壊さないこと。

### 2. 前に出す

バーは `WS_EX_NOACTIVATE` 付きのツール ウィンドウなので
([winapi.py](winapi.py) の `make_tool_window`)、クリックしても MuuTask 自身は
フォアグラウンドにならない。ただし `SetForegroundWindow` の制限は「そのプロセスが
最後の入力イベントを受け取ったか」でも通るので、クリック直後なら呼べるはず。

- `IsIconic` なら `ShowWindow(SW_RESTORE)` → `SetForegroundWindow`
- 失敗したら `SwitchToThisWindow(hwnd, True)` にフォールバック
- NOACTIVATE のウィンドウが「最後の入力」扱いになるかは**実機で要確認**。
  フォールバックがあるので詰みはしない。

### 3. ブラウザーは「タイトル一致でウィンドウを選ぶ」まで

Chrome / Edge は**タブごとに別々の GSMTC セッション**を出すが、AUMID は全部
同じ。セッションからタブへの参照は API に存在せず、タブは HWND でもないので
ウィンドウ列挙にも出てこない。CDP (`--remote-debugging-port`) なら
`Target.activateTarget` で切り替えられるが、ブラウザーの起動オプションを
変えてもらう前提なので採らない。

ブラウザーのウィンドウ タイトルは「アクティブ タブのタイトル - Google Chrome」
なので、そのプロセスのウィンドウを列挙して、**タイトルが `state.title` で
始まるものがあればそれを**前に出す。無ければ一番手前のウィンドウ。追加コストは
ゼロで、「鳴っているタブが既にそのウィンドウの表示中タブ」というよくある場合に
ちゃんと当たる。

### 4. 触るところ

- `_on_release` ([taskbar_bar.py](taskbar_bar.py)) で `_hit` が `None` かつ
  押した位置と離した位置が同じ (ドラッグでない) ときにアクティブ化。押下時に
  `_hit` が `None` だったことも記録が要る — 今は `self._pressed` に None が
  入るので、**ボタン外の押下**と**無効なボタンの押下**の区別が付かない。
- アートの小窓は最前面なので、アクティブ化の前に `popup.hide()`。
- 誤爆を嫌う人向けに、右クリック メニューに `bar_click_activates` (既定 on) を
  1 つ足す。ホイール音量 (`bar_wheel_volume`) と同じ扱い。

### 分かっている隣の穴 (A-1 の範囲外)

タブごとにセッションが立つのに AUMID が同じなので、右クリック メニューの
**「再生元の切り替え」**でも複数タブが同じ名前で並んで見分けが付かない
([media_session.py](media_session.py) の `_pick_session` の `listing`)。
A-1 で直す必要はないが、同じ根っこの問題。

---

## A-2. Python をやめて C 化し、軽くする

**状態:** やると決定・未着手 (2026-08-25)

いまの重さはほぼ全部 Python と PyInstaller のせい。

- onefile の exe は**起動のたびに中身を `%TEMP%\_MEI<番号>\` に展開**してから
  走る。この展開が起動時の重さのほぼ全て。
- 13.3 MB のうち中身は Python 本体 (2.6 MB)・Tcl/Tk・Pillow・WinRT の射影。
  不要な重い依存を `build.ps1` の `$excludes` で外して 21 MB → 13.3 MB まで
  削ったが、これ以上は Python のままでは頭打ち。

C (または C++) で書き直せば、展開なしで即起動・数百 KB 級・常駐メモリも激減。
使っている OS の機能はどれも素の Win32 / WinRT で足りる。

### 置き換えの見当

| いま | C 化したら |
| --- | --- |
| winrt-Windows.Media.Control | `Windows.Media.Control` を C++/WinRT で直接叩く |
| Tk の Canvas 描画 | レイヤード ウィンドウ + Direct2D / GDI+ |
| Pillow (アート のデコード・縮小) | WIC (Windows Imaging Component) |
| pystray | `Shell_NotifyIcon` を直接 |
| ctypes の winapi.py | そのまま Win32 呼び出しになる (むしろ素直になる) |

### 注意

- **文字送り (曲名が流れるやつ) と角丸の座**の描画は作り込んであるので、
  移植で見た目が変わらないよう気を付ける。
- 背景色のサンプリング (`sample_rows` / `sample_color`) はもともと GDI 直叩き
  なので、ほぼそのまま移せる。
- 一気にやらず、まず**現状の挙動を README / DEVELOPMENT.md と突き合わせて
  仕様として固めてから**取りかかった方が安全。
- ビルドが PyInstaller から MSVC / CMake に変わるので `build.ps1` も総取り替え。
  配布 zip の形 (`MuuTask.exe` + `README.txt`) は維持する。
