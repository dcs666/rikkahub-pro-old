#include "test.h"
#include "rikka/audio/audio.h"
#include <string.h>
#include <stdlib.h>

TEST(wav_encode_basic) {
    /* 生成简单 PCM（正弦波模拟） */
    uint8_t pcm[1000];
    for (int i = 0; i < 1000; i++) pcm[i] = (uint8_t)(i % 256);
    uint8_t *wav = NULL;
    size_t wav_len = 0;
    int rc = rk_wav_encode(pcm, sizeof(pcm), 44100, 1, &wav, &wav_len);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(wav);
    ASSERT_EQ_SIZE(44 + sizeof(pcm), wav_len);
    /* 验证 WAV header */
    ASSERT(memcmp(wav, "RIFF", 4) == 0);
    ASSERT(memcmp(wav + 8, "WAVE", 4) == 0);
    ASSERT(memcmp(wav + 12, "fmt ", 4) == 0);
    ASSERT(memcmp(wav + 36, "data", 4) == 0);
    /* 验证 PCM 数据 */
    ASSERT(memcmp(wav + 44, pcm, sizeof(pcm)) == 0);
    free(wav);
}

TEST(wav_encode_stereo) {
    uint8_t pcm[2000];
    for (int i = 0; i < 2000; i++) pcm[i] = (uint8_t)(i % 256);
    uint8_t *wav = NULL;
    size_t wav_len = 0;
    int rc = rk_wav_encode(pcm, sizeof(pcm), 48000, 2, &wav, &wav_len);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_SIZE(44 + sizeof(pcm), wav_len);
    /* 验证 channels=2 */
    uint16_t channels;
    memcpy(&channels, wav + 22, 2);
    ASSERT_EQ_INT(2, channels);
    free(wav);
}

TEST(audio_free_safe) {
    RkAudio a = {NULL, 0, ""};
    rk_audio_free(&a); /* 不崩溃 */
    a.data = malloc(10);
    a.len = 10;
    rk_audio_free(&a);
    ASSERT_NULL(a.data);
    ASSERT_EQ_SIZE(0, a.len);
}

int run_audio_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(audio, wav_encode_basic),
        RIKKA_TEST_REGISTER(audio, wav_encode_stereo),
        RIKKA_TEST_REGISTER(audio, audio_free_safe),
    };
    return run_suite("audio", tests, sizeof(tests) / sizeof(tests[0]));
}
