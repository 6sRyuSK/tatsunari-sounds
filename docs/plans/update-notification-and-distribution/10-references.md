# 10. 参考

- [free-audio/clap-wrapper](https://github.com/free-audio/clap-wrapper) — 対応形式は
  VST3 / AUv2 / AUv3 / AAX / standalone。web ターゲットなし
- [free-audio/web-clap](https://github.com/free-audio/web-clap) — WCLAP のドラフト
- [Signalsmith-Audio/wasm-clap-browserhost](https://github.com/Signalsmith-Audio/wasm-clap-browserhost)
  — wasm32 CLAP のブラウザホスト実験
- [Web Audio Modules 2](https://www.webaudiomodules.com/docs/intro/)
- [Plugin Installation | ZL Audio](https://zl-audio.github.io/help/plugin_installation/)
  — 未公証配布の先行事例（4.4）
- [Installation | ZL Audio (ZL Equalizer)](https://zl-audio.github.io/plugins/zlequalizer/installation/)
- [ZL-Audio/ZLEqualizer Discussions #29](https://github.com/ZL-Audio/ZLEqualizer/discussions/29)
  — Gatekeeper 警告のユーザー報告

## 参照資料の運用

外部資料は設計根拠であり、取得可能性や現行仕様を保証しない。実装着手時に各リンクの確認日と
参照した commit/tag を PR に記録する。特に clap-wrapper の対応 target、web-clap/WCLAP の成熟度、
OS の署名要件、Cloudflare/GitHub の料金・制限は変化し得るため再検証する。

セキュリティまたは配信 API の判断はブログだけで確定せず、OS vendor、Cloudflare、GitHub の
一次資料を追加する。リンク切れは根拠を消さず、Internet Archive または同等の一次資料へ差し替える。
