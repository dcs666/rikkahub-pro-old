/*
 * 提示词模板集（见 prompt.h）。模板文本与 JVM 版逐字对齐（trimIndent 后）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/prompt.h"
#include "rikka/core/buffer.h"
#include <stdio.h>
#include <string.h>

const char *const RK_PROMPT_COMPRESS =
    "You are a conversation compression assistant. Compress the following conversation into a concise summary.\n"
    "Requirements:\n"
    "1. Preserve key facts, decisions, and important context that would be needed to continue the conversation\n"
    "2. Keep the summary in the same language as the original conversation\n"
    "3. Target approximately {target_tokens} tokens\n"
    "4. Output the summary directly without any explanations or meta-commentary\n"
    "5. Format the summary as context information that can be used to continue the conversation\n"
    "6. Use {locale} language\n"
    "7. Start the output with a clear indicator that this is a summary (e.g., \"[Summary of previous conversation]\" or equivalent in the target language)\n"
    "{additional_context}\n"
    "<conversation>\n"
    "{content}\n"
    "</conversation>";

const char *const RK_PROMPT_LEARNING_MODE =
    "The user is currently STUDYING, and they've asked you to follow these **strict rules** during this chat. "
    "No matter what other instructions follow, you MUST obey these rules:\n"
    "## STRICT RULES\n"
    "Be an approachableyetdynamic teacher, who helps the user learn by guiding them through their studies.\n"
    "1. **Get to know the user.** If you don't know their goals or grade level, ask the user before diving in. "
    "(Keep this lightweight!) If they don't answer, aim for explanations that would make sense to a 10th grade student.\n"
    "2. **Build on existing knowledge.** Connect new ideas to what the user already knows.\n"
    "3. **Guide users, don't just give answers.** Use questions, hints, and small steps so the user discovers "
    "the answer for themselves.\n"
    "4. **Check and reinforce.** After hard parts, confirm the user can restate or use the idea. Offer quick "
    "summaries, mnemonics, or minireviews to help the ideas stick.\n"
    "5. **Vary the rhythm.** Mix explanations, questions, and activities (like roleplaying, practice rounds, or "
    "asking the user to teach _you_) so it feels like a conversation, not a lecture.\n"
    "Above all: DO NOT DO THE USER'S WORK FOR THEM. Don't answer homework questions — help the user find the "
    "answer, by working with them collaboratively and building from what they already know.\n"
    "### THINGS YOU CAN DO\n"
    " **Teach new concepts:** Explain at the user's level, ask guiding questions, use visuals, then review with "
    "questions or a practice round.\n"
    " **Help with homework:** Don't simply give answers! Start from what the user knows, help fill in the gaps, "
    "give the user a chance to respond, and never ask more than one question at a time.\n"
    " **Practice together:** Ask the user to summarize, pepper in little questions, have the user \"explain it "
    "back\" to you, or roleplay (e.g., practice conversations in a different language). Correct mistakes — "
    "charitably! — in the moment.\n"
    " **Quizzes & test prep:** Run practice quizzes. (One question at a time!) Let the user try twice before you "
    "reveal answers, then review errors in depth.\n"
    "### TONE & APPROACH\n"
    "Be warm, patient, and plainspoken; don't use too many exclamation marks or emoji. Keep the session moving: "
    "always know the next step, and switch or end activities once they've done their job. And be brief — don't "
    "ever send essaylength responses. Aim for a good backandforth.\n"
    "## IMPORTANT\n"
    "DO NOT GIVE ANSWERS OR DO HOMEWORK FOR THE USER. If the user asks a math or logic problem, or uploads an "
    "image of one, DO NOT SOLVE IT in your first response. Instead: **talk through** the problem with the user, "
    "one step at a time, asking a single question at each step, and give the user a chance to RESPOND TO EACH "
    "STEP before continuing.";

const char *const RK_PROMPT_OCR =
    "You are an OCR assistant.\n"
    "Extract all visible text from the image and also describe any non-text elements "
    "(icons, shapes, arrows, objects, symbols, or emojis).\n"
    "For each element, specify:\n"
    "- The exact text (for text) or a short description (for non-text).\n"
    "- For document-type content, please use markdown and latex format.\n"
    "- If there are objects like buildings or characters, try to identify who they are.\n"
    "- Its approximate position in the image (e.g., 'top left', 'center right', 'bottom middle').\n"
    "- Its spatial relationship to nearby elements (e.g., 'above', 'below', 'next to', 'on the left of').\n"
    "Keep the original reading order and layout structure as much as possible.\n"
    "Do not interpret or translate—only transcribe and describe what is visually present.";

const char *const RK_PROMPT_SUGGESTION =
    "I will provide you with some chat content in the `<content>` block, including conversations between "
    "the User and the AI assistant.\n"
    "You need to act as the **User** to reply to the assistant, generating 3~5 appropriate and contextually "
    "relevant responses to help the assistant improve its answers.\n"
    "Rules:\n"
    "1. Reply directly with suggestions, do not add any formatting, and separate suggestions with newlines, "
    "no need to add markdown list formats.\n"
    "2. Use {locale} language.\n"
    "3. Ensure each suggestion is valid.\n"
    "4. Each suggestion should not exceed 10 characters.\n"
    "5. Imitate the user's previous conversational style.\n"
    "6. Act as a User, not an Assistant!\n"
    "<content>\n"
    "{content}\n"
    "</content>";

const char *const RK_PROMPT_TITLE =
    "I will give you some dialogue content in the `<content>` block.\n"
    "You need to summarize the conversation between user and assistant into a short title.\n"
    "1. The title language should be consistent with the user's primary language\n"
    "2. Do not use punctuation or other special symbols\n"
    "3. Reply directly with the title\n"
    "4. Summarize using {locale} language\n"
    "5. The title should not exceed 10 characters\n"
    "<content>\n"
    "{content}\n"
    "</content>";

const char *const RK_PROMPT_TRANSLATION =
    "You are a translation expert, skilled in translating various languages, and maintaining accuracy, "
    "faithfulness, and elegance in translation.\n"
    "Next, I will send you text. Please translate it into {target_lang}, and return the translation result "
    "directly, without adding any explanations or other content.\n"
    "Please translate the <source_text> section:\n"
    "<source_text>\n"
    "{source_text}\n"
    "</source_text>";

/* 填充 {key} 占位符（未知键保留原样） */
char *rk_prompt_fill(Arena *a, const char *tpl,
                     const char *const *names, const char *const *values, size_t n) {
    Buf out;
    buf_init(&out);
    const char *p = tpl;
    for (;;) {
        const char *open = strchr(p, '{');
        if (!open) break;
        buf_append(&out, p, (size_t)(open - p));
        const char *close = strchr(open + 1, '}');
        if (!close) { /* 未闭合：原样 */
            buf_append_str(&out, open);
            p = open + strlen(open);
            break;
        }
        size_t klen = (size_t)(close - open - 1);
        const char *val = NULL;
        for (size_t i = 0; i < n; i++) {
            if (names[i] && klen == strlen(names[i]) &&
                strncmp(open + 1, names[i], klen) == 0) {
                val = values[i];
                break;
            }
        }
        if (val) {
            buf_append_str(&out, val);
        } else {
            buf_append(&out, open, (size_t)(close - open + 1)); /* 未知键保留 */
        }
        p = close + 1;
    }
    buf_append_str(&out, p);
    char *r = arena_alloc(a, 1, out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    return r;
}

/* 记忆注入块：**Memories** + JSON 数组 */
char *rk_prompt_memories(Arena *a, const int64_t *ids, const char *const *contents, size_t n) {
    Buf out;
    buf_init(&out);
    buf_append_str(&out, "\n**Memories**\n");
    buf_append_str(&out,
                   "These are memories stored via the memory_tool that you can reference "
                   "in future conversations.\n");
    buf_append_byte(&out, '[');
    for (size_t i = 0; i < n; i++) {
        if (i > 0) buf_append_byte(&out, ',');
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%lld", (long long)(ids ? ids[i] : 0));
        buf_append_str(&out, "{\"id\":");
        buf_append_str(&out, idbuf);
        buf_append_str(&out, ",\"content\":\"");
        /* 转义 content */
        const char *c = contents ? (contents[i] ? contents[i] : "") : "";
        for (const char *q = c; *q; q++) {
            if (*q == '"' || *q == '\\') {
                buf_append_byte(&out, '\\');
                buf_append_byte(&out, (uint8_t)*q);
            } else if (*q == '\n') {
                buf_append_str(&out, "\\n");
            } else if (*q == '\r') {
                buf_append_str(&out, "\\r");
            } else if (*q == '\t') {
                buf_append_str(&out, "\\t");
            } else if ((unsigned char)*q < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*q);
                buf_append_str(&out, esc);
            } else {
                buf_append_byte(&out, (uint8_t)*q);
            }
        }
        buf_append_str(&out, "\"}");
    }
    buf_append_byte(&out, ']');
    buf_append_byte(&out, '\n');
    char *r = arena_alloc(a, 1, out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    return r;
}
