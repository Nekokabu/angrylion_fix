# rdp_timing v26 — 幅の広いスパン（64〜320 px）

BAR の軽いフレーム（bar_i2274）は画素の 43% が **幅 128〜320 px のスパン**（空と路面の大きな三角形）にあり（重いフレームは 15%）、
実機では三角形が ares より 5〜6% 高い（v22）。ares のテーブルは幅 64 px までを測って、それ以上は 32 px ごとの定数（矩形で v3〜v5 に測定）で
外挿しているので、三角形の広いスパンを直接測ります。

- Y: H=8、w = 32 / 64 / 96 / 128 / 192 / 256 / 318、4096 スパン、ランダム配置、カラー 0x300000 / Z 0x4DA800（BAR の再生と同じ別バンク配置）。
  `wide_2cyc_tbz`（BAR のモード 22）、`wide_2cyc_tb`（モード 9: ブレンドのみ）、`wide_2cyc_tz`（モード 12: Z のみ）、`wide_bar_om`（BAR のモード語そのもの）、
  `wide_apex_tbz`（本物の三角形、w ≤ 128）、`wide_strip_tbz`、`wide_1cyc_tbz`、`wide_zfail_tbz`（Z 全落ち）。
- X: bar_i2274 の full / tri_only / no_tri（VI オフ、継続性）。
- `ares_reference.txt` = ares v7（`T_ZPASS=1`）。約 1 分。

作: Nekokabu
