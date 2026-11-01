# rdp_timing v25 — DK64 のアトラクトデモのフレームそのものを再生

dk64_tasklog（ares）で分かったこと: DK64 のデモのフレームは **40 万画素 / 1.4 万スパン**（v15/v22 の dk64_797 = イントロのフレーム、21 万画素の 2 倍）で、
ares では RDP が起動から **2.5 VI** 掛かっています（モデル 1.9〜2.3 VI + 待ち）。3 VI のフレームに収まるかどうかの境界にいるので、
このフレームの実機コストを直接測ります。

- リスト: `dk64_d6052`（デモ開始）、`d6110` / `d6253` / `d6373`（重い）、`d6513`（終わり近く）= ares の Sync_Full 番号、`bar_2316`（継続性）。
- variant: full / no_tri / tri_only / no_load / no_rect / state_only / om_no_atomic / om_no_aa / z_near（Z クリア除去＋最近 = 全落ち）。
  full / no_tri / tri_only は VI オフとゲームの VI モード（DK64: 0x3016）の両方（VI が途中で止まる問題は VI プローブで分かります）。
- `X` 行の形式は v24 と同じ。`ares_reference.txt` = ares v7（`T_ZPASS=1`）、`ares_raster_wmodel.txt` = + エッジウォーカー + W 単価。
  `compare.py 実機ファイル v7z rastwz`（`v25runs/TAG.txt` に置く）。8 MB 必須、約 1 分。

作: Nekokabu
