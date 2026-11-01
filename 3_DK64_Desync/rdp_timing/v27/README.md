# rdp_timing v27 — 幅の広いスパン × 本物のテクスチャの進み

v26 の結果: 幅 64 px 以上のスパンは、実機が ares のテーブル（矩形で測った 32 px ごとの外挿）より **25〜40% 安い**
（2cyc_tex_blend_z で 1 画素あたり 2.15 clk / テーブル 2.94、1cyc では 1.79 / 3.25）。ところが BAR の軽いフレームは実機の方が高い。
v22〜v26 の合成三角形はテクスチャ座標が 1 画素あたり 1/32 texel しか進まず（スパン全体で 10 texel）、ゲームの路面や空の
スパンは 1 画素 1 texel 以上進むので、その違いを測ります。

- Y: H=8、w = 8 / 32 / 128 / 318、ランダム配置、カラー 0x300000 / Z 0x4DA800。
  `wide_tbz_1tx`（1 texel/px）、`_4tx`（4 texel/px、縮小）、`_qtx`（1/4 texel/px）、`_persp`（TEX_PERSP + W の傾き）、`wide_bar_1tx` / `wide_bar_persp`（BAR のモード語）、
  `wide_tb_1tx` / `wide_tz_1tx` / `wide_1cyc_1tx`、`wide_notex_sz`（テクスチャ無し 0D）、`wide_tbz_i8_1tx` / `wide_tbz_rgba32`（I8 / RGBA32 テクスチャ）、
  `base_1tx` / `base_1_32`（1 texel/px と 1/32 texel/px の対照）。
- X: bar_i2274 の full / tri_only / no_tri。
- `ares_reference.txt` = ares v7（`T_ZPASS=1`、テクスチャの進みはモデルに無いので全ケース同値）。約 1 分。

作: Nekokabu
