ISDB向けFFmpeg
=============

ISDB-S/Tから受信・保存したMPEG-TSを再生するため、
ffmpeg 4.3をベースに以下の拡張を加えた。

* MPEG-TS demuxerの拡張 (mpegts)
* ISDB字幕デコーダの追加 (isdbsub)
* DVBデバイスからの入力の拡張 (dvbproto)

ISDB向けmpvで使用されている。

----
## MPEG-TS demuxer (mpegts)

## 機能

* BCAS復号(デスクランブリング)
* ISDB字幕の取り出し・分離
* その他
  - 局名などのメタ情報の文字コード変換
  - ユーザによる映像/音声/字幕の各トラックや番組の切り替え対応
  - TSストリーム内での番組(プログラム,PMT)切り替わり対応
  - 音声/字幕トラックの言語タグ・デフォルトマーク認識
  - デュアルモノやマルチ言語の字幕PESへの対応

## 関連モジュール・ライブラリ

* libdemulti2 + {libpcsclite | libsobacas | libyakisoba}: BCAS復号のため(optional)
    (入手はtor/Freenetより)
* [gconv-module-aribb24](https://github.com/0p1pp1/gconv-module-aribb24):
 メタ情報の文字コード変換のため (optional)


----
## ISDB字幕デコーダ (isdbsub)

### 機能

* ISDB-S/Tの字幕データのPESパケットを(拡張した)ASS文字列に変換する。

### 使用上の注意

* ISDB-S/T独自の字幕データを無理やりASS形式に変換しているので、多少の位置ずれがある。
* ARIB STD-B24仕様での「字幕データ」のみに対応し、「字幕スーパー」には非対応なので、
番組内でのセリフや音の字幕のみとなり、お知らせや速報などの表示には対応していない。
* 「ロールアップモード」(巻き上がり的な表示。仕様書参照)にも非対応
* テストが不十分なので、表示崩れなども予想される。

### 関連モジュール・ライブラリ

* 上記拡張版のmpegts demuxer: MPEG-TSからの字幕データの取り出しのため
* [ISDB向けlibass](https://github.com/0p1pp1/libass):
 出力結果の描画用。ISDB-S/Tの字幕機能実現のため一部独自拡張を含むため。


----
## DVBデバイスからの入力 (dvbproto)

### 機能
* libavformatに`dvb://[cardno@]channel_name[?option=val...]`の形式での入力機能を追加。
* チャンネル設定ファイルのデフォルトパスは `${XDG_DATA_HOME}/ffmpeg/channels.conf`、
  ISDB向けmplayerのものと同じ1行1チャンネルの形式。

なお、mpvやmplayerは自前で同等機能を実装しているので、
本機能はffplayプログラムでの使用等を想定したおまけ的・テスト的な機能である。


以下、オリジナルのREADME.md

--------------
FFmpeg README
=============

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides a mean to alter decoded Audio and Video through chain of filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
