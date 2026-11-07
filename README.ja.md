# Desk32

[English](README.md) | [日本語]

**Desk32** は、最高品質のモニタリング環境をローカルに再現する、オーディオマニア向けスタジオコンソール型ミュージックプレーヤー（Linux専用）です。

> **注意:** Desk32 は純粋な音質評価・リファレンス試聴のためのスタジオコンソールであり、DJ用同時ミキシング機能、ビート同期、クロスフェーダー等は明確に除外しています。

---

## 主要な特徴

- **Reference Deck System:** メイン画面に 4Deck（Deck A〜D）を常時表示。独立した Transport・Seek 位置を保持し、ワンタッチ比較試聴を実現。「通常切り替え（位置保持）」と「比較切り替え（切替元Pause＋切替先へサンプル/ms単位シーク＋Deck別オフセット調整）」の2モード制。
- **フォルダ連続試聴（Folder Session）:** フォルダ直下の音源を自然順スナップショットで先頭から再生。肥大化したPlaylist機能に頼らず、アルバムやフォルダごとの順次試聴を補助。
- **3系統のOutput Path（段階的実装）:**
  1. *Listening Path*: リスニング専用補正（ヘッドホン/スピーカー/部屋音響補正）。
  2. *Render Monitor Path*: リスニング補正混入なしでマスタリング結果そのものを試聴。
  3. *Render / File Output Path*: オフラインファイルレンダリング経路（Phase 4）。
- **内部処理 64-bit float 基準:** 内部ミキサー・サミング・内蔵DSPの基本演算は IEEE 754 64-bit 倍精度浮動小数点（double precision）を採用。
- **スタジオコンソール型UI:** WQHD (2560 × 1440) フルスクリーンを標準ターゲットとし、トラック波形表示必須、32-band GEQおよび左右各12バンドPEQの非破壊全面切り替えを搭載。
- **完全ローカル完結:** ネットワーク通信、テレメトリ、クラウド同期、オンラインAI機能を一切排除。ジャケット画像（Artwork）の抽出・キャッシュも行わない完全プライバシー設計。
- **非同期操作ログ（Log Tab）:** SQLiteへの非同期構造化記録およびJSONLエクスポート。Audio Threadを一切ブロックしない設計。

---

## 技術スタック

- **開発言語:** C++20
- **GUIフレームワーク:** Qt 6 (Qt Quick / QML / Scene Graph)
- **音声バックエンド:** PipeWire (標準・最優先) / JACK (早期対応) / ALSA direct (後続対応)
- **ビルドシステム:** CMake (≥ 3.22) & Ninja
- **対象プラットフォーム:** Linux デスクトップ (X11 / Wayland) — 基準環境: CachyOS + COSMIC Desktop Environment

---

## ソースコードからのビルド

Desk32 はソースコード公開のみで配布されます。依存ライブラリは各自のディストリビューションのパッケージマネージャから導入してください。

### 前提パッケージの導入 (Arch Linux / CachyOS)

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-tools \
    pipewire jack2 alsa-lib sqlite flac libvorbis opus ffmpeg
```

### ビルドとテストの実行

```bash
# CMake Preset を使用した構成 (GCC & Ninja デバッグ)
cmake --preset default

# ビルド実行
cmake --build --preset default

# 単体テストの自動実行
ctest --preset default
```

Clang コンパイラでビルドする場合:

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

---

## ライセンス

- Desk32 の自作コードは [MIT License](LICENSE) のもとで公開されています。
- 第三者ライブラリのライセンスおよび配布条件の詳細は [THIRD-PARTY-LICENSES.ja.md](THIRD-PARTY-LICENSES.ja.md) を参照してください。

