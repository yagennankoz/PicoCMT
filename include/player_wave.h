// ============================================================================
// player_wave.h
// 波形をそのまま再生するプレーヤー(RAW・WAV)
// ============================================================================
#pragma once

#include "cmt_player.h"
#include "cmt_sd_buffer.h"
#include "cmt_wav_reader.h"

// 半周期(レベルと終わりの時刻)を順に受け取って再生キューへ入れる共通部分。
// 時刻の合計から長さを出すので、丸めの誤差が積み上がらない
class PlayerHalves : public CmtPlayer {
   protected:
    File* cmt_file;
    SdBufferedRead* buffered_reader;
    bool end_reached;
    bool exhausted;
    bool source_eof;

    // 読み込み中の半周期(同じレベルが続けばまとめる)
    bool has_current;
    bool current_level;
    uint64_t current_end_us;  // 読み込み中の半周期の終わりの時刻
    uint64_t emitted_us;      // キューへ渡し終えた時間の累計

    // キューに入りきらなかった半周期の残り
    bool pend_level;
    uint32_t pend_us;

    // 次にキューへ入れた値が出力されるレベル(PIOは値ごとにH/Lを反転する)
    bool out_high_next;

    char playback_log[24];

    void reset_halves();
    void set_log(const char* text);

    // 次の半周期を読む。読めたらtrueで、*levelにレベル、*end_usに終わりの時刻を入れる。
    // ファイルの終わりならfalse
    virtual bool read_half(bool* level, uint64_t* end_us) = 0;
    // 先頭に戻す
    virtual void rewind() = 0;

   private:
    bool enqueue_raw(queue_t* q, uint16_t us);
    void close_current();
    bool drain_pending(queue_t* q);

   public:
    PlayerHalves();
    virtual ~PlayerHalves();

    void process(queue_t* play_queue) override;
    bool seek(int direction, queue_t* play_queue) override;
    void stop() override;
    bool is_end_reached() override;
    bool is_exhausted() override;
    void get_status(PlaybackStatus* out) override;
};

// 録音したRAWファイル(16bitワード: bit15=レベル、bit14-0=録音PIOの計測値。
// 0xFFFF+ミリ秒は10ms以上の無音)
class PlayerRaw : public PlayerHalves {
   private:
    uint64_t time_us;
    bool last_level;

   protected:
    bool read_half(bool* level, uint64_t* end_us) override;
    void rewind() override;

   public:
    bool init(File* f, const char* filename) override;
};

// WAVファイル(リニアPCM、8/16/24/32bit、チャンネルは先頭だけ使う)
class PlayerWav : public PlayerHalves {
   private:
    WavFormat fmt;
    WavEdgeDetector detector;
    uint32_t data_read;  // 読んだ音声データのバイト数
    bool tail_done;      // ファイルの終わりで最後の半周期を出したか

   protected:
    bool read_half(bool* level, uint64_t* end_us) override;
    void rewind() override;

   public:
    bool init(File* f, const char* filename) override;
};
