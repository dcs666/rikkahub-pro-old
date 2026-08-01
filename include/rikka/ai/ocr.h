#ifndef RIKKA_AI_OCR_H
#define RIKKA_AI_OCR_H

#include "rikka/ai/provider.h"

/*
 * 图像 OCR（对标 JVM 版 OcrTransformer.performOcr）：
 * 用视觉模型识别本地图片。构建 [system(ocr_prompt), user(IMAGE part)]
 * 调用 rp_chat_stream。
 *
 * image_path 语义与 provider 的 image_url 一致：data URI（data:image/...）
 * 或可访问的 http(s) URL（调用方负责把本地文件转成其中一种）。
 * 返回 0 成功（*text_out 为识别文本，malloc，调用方 free）；-1 失败。
 */
int rk_ocr_image(const RikkaProviderCfg *cfg, const char *ocr_prompt,
                 const char *image_path, int timeout_ms, char **text_out);

#endif /* RIKKA_AI_OCR_H */
