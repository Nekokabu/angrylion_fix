# dk64_tasklog v2 — ドンキーコング64 (J) の実機フレームタイムライン記録

bar_tasklog と同じ常駐ロガーを DK64 (J) に入れたパッチです。ゲームの libultra の 4 関数（`osSpTaskLoad` / `osSpTaskStartGo` /
例外ハンドラの `send_mesg` / `osSpTaskYield`）を `j`/`jal` で差し替え、COP0 Count 付きで「グラフィックタスクの起動・RSP 停止・DP 割り込み・
VI 割り込み」を 4 バイト/件で記録します（形式は bar_tasklog v3 と同じ。ヘッダのマジックだけ `DKTL`）。

- 記録は **タイトル画面の静止フレーム（60 VI 以上グラフィックタスクが無い）の直後に来る最初のグラフィックタスク = アトラクトデモの開始**で
  自動的に始まり（3000 VI 以降）、32 KB（約 8000 件、デモ全体 ≈ 1600 VI を含む約 2500 VI）溜まるとカートリッジの **SRAM（0x08000000）に書いて
  停止**します（画面が止まったら完了。デモ開始から約 40 秒）。トリガが掛からなくても 20000 VI（約 5.5 分）で書いて止まります。
- **v2**: PI DMA の開始（`osEPiRawStartDma` / `__osPiRawStartDma` をフック、kind 6、payload = バイト数/8）と PI 割り込み（event 8）も記録します
  （ヘッダ version 5、idle 版 6）。デモ中は 1 フレームに 5〜14 回、10〜28 KB の PI DMA があり（ares で 0.13〜0.35 VI/フレーム）、
  実機の DMA 時間（開始→割り込み）が frames 表の `pi_time` に出ます。記録は 1 フレーム約 20 件になるので約 430 フレーム（デモの大半）で一杯になります。
- ロガーは RAM 0x80160000（コード）/ 0x80170000（32 KB バッファ）に常駐します。ares ではタイトルからデモまでずっと 0 のままの領域
  （0x8015368C〜0x80200000）ですが、実機で別の使われ方をしていた場合は画面が乱れるか止まります。拡張パック必須（ゲーム自体が必要）。

## 必要なもの
- ドンキーコング64 (J) の ROM（32 MB、.z64/.n64/.v64 いずれも可）— **ROM 自体は同梱していません**。
- EverDrive 64 X シリーズ。セーブタイプを **SRAM 256K** に手動設定してください（本来は EEPROM 16K。EEPROM 無しでもデモまでは動きます。
  ares で確認済み）。

## 手順
1. `python3 make_patch.py <DK64_J.z64> hook.bin dk64_tasklog.z64`（CIC-6105 のチェックサムを再計算します。ROM サイズは 32 MB のまま、
   ロガーは末尾パディング 0x1FF0000 に入ります）。
2. SD に入れて起動（セーブタイプ: SRAM 256K）。そのまま放置。タイトル → アトラクトデモ、デモ開始から約 40 秒で画面が止まります。
3. リセットして EverDrive のメニューに戻り、セーブデータ（`dk64_tasklog.srm`、32 KB）を送ってください。
4. 解析: `python3 decode_tasklog.py dk64_tasklog.srm`（`--events` で全イベント、`--csv out.csv`）。

## idle 版（参考、まずは通常版だけで十分です）
`hook_idle.bin` で作った ROM（`python3 make_patch.py <ROM> hook_idle.bin dk64_tasklog_idle.z64`、**ファイル名を変えてください**）は、記録中、
グラフィックタスクを起動した CPU が RSP 停止まで SP_STATUS を読むだけのループで待ちます（bar_tasklog v4c と同じ）。ares では十数フレーム後に
ゲームの進行が崩れて（デモが 58 フレームで止まる）バッファが埋まらず、20000 VI の保険で書き出されます。最初の十数フレームの
RSP 0.65 VI / DP 2.4 VI は通常版（0.69 / 2.55）とほぼ同じで、ares では DK64 の CPU 競合は小さいことが分かります。

## ares 側の参照
`ares_v7.srm` / `ares_v7_frames.txt`（通常版、series v7 のハーネス）、`ares_v7_idle.srm` / `ares_v7_idle_frames.txt`（idle 版）。
ares ではデモ開始が VI 12644、1 フレーム = 3 VI（RSP 0.69 VI、**DP 割り込みが起動から 2.5 VI 後**、次の起動まで 2.2 VI）。
v1 の実機ログ（`hw/dk64_tasklog_hw_001`）はデモ開始 VI 12658 で ares と 14 VI しか違わず、フレームごとに直接比べられました。

作: Nekokabu
