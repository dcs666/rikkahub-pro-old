/*
 * rikkahub CLI：命令行 AI 客户端。
 * 用法：
 *   rikkahub "hello"                          # 单次对话（环境变量 OPENAI_API_KEY）
 *   rikkahub --model claude-3-5 --provider claude "hi"
 *   rikkahub --interactive                     # 交互模式
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/provider.h"
#include "rikka/core/message.h"
#include "rikka/util/arena.h"
#include "rikka/trace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage: rikkahub [options] \"message\"\n");
    printf("Options:\n");
    printf("  --provider openai|claude|google   Provider (default: openai)\n");
    printf("  --model NAME                      Model name (default: gpt-4o)\n");
    printf("  --base-url URL                    API base URL\n");
    printf("  --api-key KEY                     API key (or env OPENAI_API_KEY/ANTHROPIC_API_KEY)\n");
    printf("  --max-tokens N                    Max tokens\n");
    printf("  --trace                           Enable trace output\n");
    printf("  --interactive                     Interactive mode\n");
    printf("  -h, --help                        Show help\n");
}

static RikkaProviderId parse_provider(const char *name) {
    if (strcmp(name, "claude") == 0) return RIKKA_PROVIDER_CLAUDE;
    if (strcmp(name, "google") == 0) return RIKKA_PROVIDER_GOOGLE;
    return RIKKA_PROVIDER_OPENAI;
}

static RikkaMessage *make_message(Arena *a, RikkaRole role, const char *text) {
    RikkaMessage *m = rmsg_new(a, role);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    p->data = text;
    p->len = strlen(text);
    return m;
}

int main(int argc, char **argv) {
    const char *prompt = NULL;
    const char *model = NULL;
    const char *api_key = NULL;
    const char *base_url = NULL;
    RikkaProviderId provider = RIKKA_PROVIDER_OPENAI;
    int max_tokens = 0;
    int trace_enabled = 0;
    int interactive = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider = parse_provider(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
            api_key = argv[++i];
        } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
            base_url = argv[++i];
        } else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            max_tokens = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace_enabled = 1;
        } else if (strcmp(argv[i], "--interactive") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            prompt = argv[i];
        }
    }

    /* 环境变量 API key */
    if (!api_key) {
        if (provider == RIKKA_PROVIDER_OPENAI) api_key = getenv("OPENAI_API_KEY");
        else if (provider == RIKKA_PROVIDER_CLAUDE) api_key = getenv("ANTHROPIC_API_KEY");
        else if (provider == RIKKA_PROVIDER_GOOGLE) api_key = getenv("GOOGLE_API_KEY");
    }
    if (!api_key) {
        fprintf(stderr, "Error: no API key (use --api-key or set env)\n");
        return 1;
    }
    if (!model) {
        model = provider == RIKKA_PROVIDER_OPENAI ? "gpt-4o"
              : provider == RIKKA_PROVIDER_CLAUDE ? "claude-3-5-sonnet"
              : "gemini-pro";
    }
    if (!base_url) {
        base_url = provider == RIKKA_PROVIDER_OPENAI ? "https://api.openai.com/v1"
                 : provider == RIKKA_PROVIDER_CLAUDE ? "https://api.anthropic.com"
                 : "https://generativelanguage.googleapis.com";
    }

    RikkaProviderCfg cfg = {provider, base_url, api_key, model, max_tokens, 0, {0}};

    if (interactive) {
        /* 交互模式 */
        printf("RikkaHub CLI (Ctrl-D to exit)\n");
        char line[4096];
        while (1) {
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            line[strcspn(line, "\n")] = '\0';
            if (strlen(line) == 0) continue;
            if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

            Arena *a = arena_create(0);
            const RikkaMessage *msgs[1];
            msgs[0] = make_message(a, RIKKA_ROLE_USER, line);
            RikkaStream out;
            rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);

            RkTracer tracer;
            rk_trace_init(&tracer);
            if (trace_enabled) {
                rk_trace_enable(&tracer, 1);
                rk_trace_set_global(&tracer);
            }

            int rc = rp_chat_stream(&cfg, msgs, 1, &out, 60000, NULL);
            if (rc == 0) {
                /* 输出流式结果 */
                for (size_t i = 0; i < out.msg->part_count; i++) {
                    RikkaPart *p = &out.msg->parts[i];
                    if (p->type == RIKKA_PART_TEXT) {
                        fwrite(p->data, 1, p->len, stdout);
                    }
                }
                printf("\n");
            } else {
                fprintf(stderr, "Error: request failed (rc=%d)\n", rc);
            }
            if (trace_enabled) {
                rk_trace_dump(&tracer, stderr);
                rk_trace_set_global(NULL);
            }
            rk_trace_destroy(&tracer);
            rstream_destroy(&out);
            arena_destroy(a);
        }
    } else {
        /* 单次模式 */
        if (!prompt) {
            print_usage();
            return 1;
        }
        Arena *a = arena_create(0);
        const RikkaMessage *msgs[1];
        msgs[0] = make_message(a, RIKKA_ROLE_USER, prompt);
        RikkaStream out;
        rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);

        RkTracer tracer;
        rk_trace_init(&tracer);
        if (trace_enabled) {
            rk_trace_enable(&tracer, 1);
            rk_trace_set_global(&tracer);
        }

        int rc = rp_chat_stream(&cfg, msgs, 1, &out, 60000, NULL);
        if (rc == 0) {
            for (size_t i = 0; i < out.msg->part_count; i++) {
                RikkaPart *p = &out.msg->parts[i];
                if (p->type == RIKKA_PART_TEXT) {
                    fwrite(p->data, 1, p->len, stdout);
                }
            }
            printf("\n");
        } else {
            fprintf(stderr, "Error: request failed (rc=%d)\n", rc);
            return 1;
        }
        if (trace_enabled) {
            rk_trace_dump(&tracer, stderr);
            rk_trace_set_global(NULL);
        }
        rk_trace_destroy(&tracer);
        rstream_destroy(&out);
        arena_destroy(a);
    }
    return 0;
}
