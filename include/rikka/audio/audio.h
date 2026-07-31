#ifndef RIKKA_AUDIO_AUDIO_H
#define RIKKA_AUDIO_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * 音频管线：TTS/ASR 客户端 + WAV 封装。
 * TTS: OpenAI TTS API（返回 MP3）
 * ASR: OpenAI Whisper API（返回文本）
 * WAV: PCM → WAV 封装
 */

typedef struct {
    uint8_t *data;     /* 音频数据（malloc，调用方 free） */
    size_t len;
    char format[16];   /* "mp3"/"pcm"/"wav" */
} RkAudio;

void rk_audio_free(RkAudio *a);

/* TTS: OpenAI TTS API（text → MP3） */
int rk_tts_openai(const char *api_key, const char *text, const char *voice, RkAudio *out);

/* ASR: OpenAI Whisper API（audio → text） */
int rk_asr_openai(const char *api_key, const uint8_t *audio, size_t len,
                  const char *format, char **text);

/* WAV 封装：PCM → WAV（16-bit PCM） */
int rk_wav_encode(const uint8_t *pcm, size_t len, int sample_rate, int channels,
                  uint8_t **out, size_t *out_len);

#endif /* RIKKA_AUDIO_AUDIO_H */
