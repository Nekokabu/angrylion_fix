# rdp_timing v28 — ゲームのリストを三角形の幅で分ける

v26/v27: 幅 64 px 以上のスパンは実機がテーブルより 25〜40% 安く、テクスチャの進み・パースペクティブ・形式は関係なし（1 texel/px で +2%）。
それでも BAR の軽いフレーム（画素の 43% が幅 128 px 超）は実機の方が高い。矛盾を解くために、ゲームのリストの三角形を
**最大スパン幅で 3 つに分けて**それぞれ再生します（tri_only ベース、残りは NOP）:

- `tri_wide_only`（最大幅 64 px 以上）、`tri_mid_only`（16〜63）、`tri_narrow_only`（16 未満）、および full / tri_only / no_tri。
- リスト: bar_i2274（軽）、bar_i2314、bar_2316、dk64_d6373（DK64 デモの重いフレーム）。VI オフ。約 1 分。
- `ares_reference.txt` = ares v7（`T_ZPASS=1`）。各変種には Set 系（state_only 相当）が含まれるので、差を取るときは注意。

作: Nekokabu
