# TODO

やると決めたが、まだ手を付けていないもの。**「A-1 をやって」**のように番号で
指示できるようにしてある。

---

## A-1. バーをクリックして、鳴らしているアプリを前に出す

**状態:** 方針決定済み・未着手 (2026-08-23 に検討、2026-08-25 に決定)

ボタン以外のところを左クリックしたら、いま鳴らしているアプリのウィンドウを
アクティブ化する。UI Automation でブラウザーのタブまで切り替える案は
**入れない** (日本語 UI 依存・Chrome の UI 改変で壊れる・クロス プロセス UIA が
数百 ms かかる、で割に合わない)。ブラウザーは**ウィンドウまで**で割り切る。

### 1. AUMID からウィンドウを引き当てる (src/win32util.cpp)

GSMTC がくれるのは AUMID (`SourceAppUserModelId`) だけで、HWND は付いてこない
([src/media.cpp](src/media.cpp) の `AppIdOf`)。自前で引く。

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

全部 Win32 で書けるので**依存は増やさない**。exe を 0.42 MB に収めた分を
壊さないこと。

### 2. 前に出す

バーは `WS_EX_NOACTIVATE` 付きのツール ウィンドウなので
([src/win32util.cpp](src/win32util.cpp) の `MakeToolWindow`)、クリックしても
MuuTask 自身はフォアグラウンドにならない。ただし `SetForegroundWindow` の制限は
「そのプロセスが最後の入力イベントを受け取ったか」でも通るので、クリック直後
なら呼べるはず。

- `IsIconic` なら `ShowWindow(SW_RESTORE)` → `SetForegroundWindow`
- 失敗したら `SwitchToThisWindow(hwnd, TRUE)` にフォールバック
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

- [src/bar.cpp](src/bar.cpp) の `WM_LBUTTONUP` で、`HitTest` が `Button::None`
  かつ押した位置と離した位置が同じ (ドラッグでない) ときにアクティブ化。
  押下時に `Button::None` だったことも記録が要る — 今は `pressed_` に
  `Button::None` が入るので、**ボタン外の押下**と**無効なボタンの押下**の
  区別が付かない。
- アートの小窓は最前面なので、アクティブ化の前に `Popup::Hide()`。
- 誤爆を嫌う人向けに、右クリック メニューに `bar_click_activates` (既定 on) を
  1 つ足す。ホイール音量 (`bar_wheel_volume`) と同じ扱い。

### 分かっている隣の穴 (A-1 の範囲外)

タブごとにセッションが立つのに AUMID が同じなので、右クリック メニューの
**「再生元」**でも複数タブが同じ名前で並んで見分けが付かない
([src/media.cpp](src/media.cpp) の `PickSession` が返す listing)。
A-1 で直す必要はないが、同じ根っこの問題。

---

## 済んだもの

- **A-2. Python をやめて C 化し、軽くする** — v22 で完了。起動の CPU が
  0.47 秒 → 0.11 秒、常駐メモリが 17 MB → 1.8 MB、配布物が 29 MB → 0.42 MB。
  経緯と実測値は [DEVELOPMENT.md](DEVELOPMENT.md) の
  「Python から C++ へ」にある。
