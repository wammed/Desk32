# 第三者ライセンス台帳 (Third-Party Licenses Ledger)

- **文書バージョン:** 1.1.0
- **最終更新日:** 2026-11-18
- **プロジェクト名:** Desk32
- **Desk32 自作部分ライセンス:** MIT License（第一候補・既定）
- **配布形態:** ソースコード公開のみ（Desk32側でのバイナリ再配布なし）

Desk32は第三者のソースコードをリポジトリ内にベンダリング（複製同梱）せず、すべて各Linuxディストリビューションおよびユーザー環境の共有ライブラリ（`.so` による動的リンク）として利用します。以下にDesk32が利用・検証する主要な外部ライブラリおよびコンポーネントのライセンス情報を記録します。

---

## 1. 外部依存ライブラリ一覧

| パッケージ名称 | 採用バージョン (基準環境) | 用途 | ライセンス識別子 (SPDX) | 著作権者 / アップストリームURL | 変更有無 | リンク / 依存形態 |
|---|---|---|---|---|---|---|
| **Qt 6 (Core, Quick, Qml, QuickControls2)** | 6.12.0 | GUIフレームワーク、Scene Graph、多言語化 | `LGPL-3.0-only` OR `GPL-3.0-only` | The Qt Company & Qt Project contributors<br>https://www.qt.io/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **libpipewire-0.3** | 1.6.10 | 標準音声入出力バックエンド、グラフ接続 | `MIT` / `LGPL-2.1-or-later` | Wim Taymans, Red Hat, PipeWire contributors<br>https://pipewire.org/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **JACK (libjack)** | 1.9.22 | プロオーディオ接続、外部ホスト連携 | `LGPL-2.1-or-later` | Stephane Letz, Paul Davis, JACK contributors<br>https://jackaudio.org/ | なし (Unmodified) | 動的リンク (`.so` / PipeWire-JACK) |
| **alsa-lib (libasound)** | 1.2.16.1 | ハードウェア能力直接取得、ALSA direct | `LGPL-2.1-or-later` | Jaroslav Kysela, ALSA Project contributors<br>https://www.alsa-project.org/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **SQLite** | 3.54.0 | 操作履歴（Log Tab）、プリセットDB、レンダー連番管理 | `blessing` (Public Domain) | D. Richard Hipp, SQLite contributors<br>https://www.sqlite.org/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **libFLAC** | 1.5.0 | ハイレゾPCMデコード（FLAC） | `BSD-3-Clause` | Xiph.Org Foundation<br>https://xiph.org/flac/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **libvorbisfile** | 1.3.7 | Ogg Vorbis音声デコード | `BSD-3-Clause` | Xiph.Org Foundation<br>https://xiph.org/vorbis/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **libopus** | 1.6.1 | Opus高効率低遅延音声デコード | `BSD-3-Clause` | Xiph.Org Foundation, Skype Ltd., CSIRO<br>https://opus-codec.org/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **FFmpeg (libavcodec, libavformat)** | 63.1.102 | 包括的マルチコーデックデコード候補 | `LGPL-2.1-or-later` | FFmpeg team<br>https://ffmpeg.org/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **libsoxr** | 0.1.3 | 高精度マルチレートPCMリサンプリング | `LGPL-2.1-or-later` | Rob Sykes, The SoX Resampler contributors<br>https://sourceforge.net/projects/soxr/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |
| **TagLib** | 2.3.2 | 音声ファイルのタグメタデータ抽出・書き出しおよび来歴タグ (`DESK32*` / PropertyMap) | `LGPL-2.1-or-later` OR `MPL-1.1` | Scott Wheeler, TagLib contributors<br>https://taglib.org/ | なし (Unmodified) | 動的リンク (`.so` / システムパッケージ) |

---

## 2. 配布形態との整合性確認 (Distribution Compliance)

1. **ソースコード配布原則:**
   - Desk32プロジェクト自身はバイナリ配布・パッケージングを行わず、ソースコードのみを公開します。
   - ユーザーは各ディストリビューションの公式パッケージマネージャを介して依存ライブラリを導入し、各自のローカル環境でビルドします。
2. **動的リンクによるLGPL遵守:**
   - LGPLライセンス（Qt 6、PipeWire、ALSA、FFmpeg等）のライブラリはすべて共有ライブラリ（`.so`）として動的にリンクされ、Desk32自身のコード（MIT）と明確に分離されます。
   - ユーザーは任意のバージョンの共有ライブラリに差し替えてDesk32を実行することが可能です。
3. **第三者コードの同梱禁止 (No Vendoring):**
   - リポジトリ内にはDesk32独自のオリジナルコードおよび自作アセットのみを配置し、第三者著作物の複製・再配布を行いません。

