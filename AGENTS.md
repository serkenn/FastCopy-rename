# Repository Guidelines

## 文書の役割と言語ルール
- `README.md` は使用者向けの説明書（使い方、実行方法、FAQ）として記述する。
- `AGENTS.md` は開発者向けの説明書（開発手順、実装方針、レビュー基準）として記述する。
- このリポジトリで今後追加・更新する指示文やガイド文書は、日本語で記述する。

## プロジェクト構成
- `src/` は FastCopy 本体のソース（`fastcopy*.cpp`、`mainwin*.cpp`、各種ダイアログ/設定/ユーティリティ）。
- `src/TLib/` は共通内部ライブラリ。
- `src/shellext/` は Explorer シェル拡張。
- `src/install/` はインストーラ関連。
- `external/xxhash` と `external/zlib` は同梱サードパーティ依存。
- `doc/` と `help/` はエンドユーザー向け資料。
- 主要ビルド入口は `FastCopy.sln` と `FastCopy.vcxproj`。

## ビルド・開発コマンド
- `msbuild FastCopy.sln /p:Configuration=Debug /p:Platform=x64`  
  x64 Debug の全プロジェクトをビルド。
- `msbuild FastCopy.sln /p:Configuration=Release /p:Platform=Win32`  
  Win32 Release をビルド。
- `msbuild FastCopy.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64`  
  本体アプリのみ再ビルド。
- 前提: Visual Studio 2017 以上（`v141` ツールセット）。

## コーディング規約
- `src/` 既存スタイルに合わせる（タブインデント、K&R ブレース、Windows API 型/マクロ）。
- 命名は既存パターンを踏襲する。
- クラス/型: `PascalCase`（例: `FastCopy`）。
- ファイル名: 役割サフィックス付き小文字ベース（例: `mainwinlog.cpp`、`setupdlg.h`）。
- 変更は小さく分割し、レガシー領域の大規模リファクタは避ける。

## テスト方針
- 専用の自動ユニットテスト基盤は未整備。
- 挙動変更時は `Debug` / `Release` と `Win32` / `x64` の影響範囲で手動確認する。
- PR には再現手順（入力、操作モード、期待結果）を必ず記載する。

## コミット・PRルール
- コミット件名は短く明確にする（例: `v3.63`, `shellextのパス解析修正`）。
- 1コミット1目的を基本とする。
- PR には目的、ユーザー影響、関連Issue、確認したビルド構成を記載する。
- UI変更時のみスクリーンショットを添付する。

## リリース運用（エージェント向け）
- GitHub Actions のリリースワークフロー（`v*` タグ起点）を使う場合、エージェントは必要作業にタグ作成とタグ push を含めて実施する。
- 例: `git tag v3.64` → `git push origin v3.64`
- 誤タグ防止のため、タグ名（バージョン）と対象コミットを確認してから実行する。

## NAS互換文字対応（Synology等）
- Synology などの NAS へコピーする際に、ファイル名文字が原因でエラーや文字化けが発生する場合は、互換性優先で文字を置換または省略して処理する。
- 基本方針:
- コピー失敗よりも到達性を優先し、危険文字・非互換文字は安全な代替文字（例: `_`）へ置換する。
- 置換しても不正となる場合は該当文字を省略する。
- 変更後の名前重複が発生する場合は連番等で衝突回避する。
- ログには「元の文字列」と「変換後の文字列」を残し、追跡可能にする。
