/*
 * rikkahub CLI：命令行 AI 客户端。
 * 用法：
 *   rikkahub "hello"                          # 单次对话（环境变量 OPENAI_API_KEY）
 *   rikkahub --model claude-3-5 --provider claude "hi"
 *   rikkahub --interactive                     # 交互模式
 *   rikkahub --mcp URL --mcp-list              # MCP SSE 工具调试
 *   rikkahub --mcp URL --mcp-call echo --mcp-args '{"text":"hi"}'
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/provider.h"
#include "rikka/core/message.h"
#include "rikka/mcp/mcp.h"
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
    printf("  --mcp URL                         MCP server (SSE transport), tool debug mode\n");
    printf("  --mcp-list                        List MCP tools (with --mcp)\n");
    printf("  --mcp-call NAME                   Call MCP tool (with --mcp)\n");
    printf("  --mcp-args JSON                   Tool arguments for --mcp-call (default {})\n");
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

/* MCP 工具调试模式（SSE 传输端到端） */
static int mcp_main(const char *url, int list, const char *call, const char *args_json) {
    if (!list && !call) {
        fprintf(stderr, "Error: --mcp needs --mcp-list or --mcp-call\n");
        return 1;
    }
    RkMcpClient c;
    if (rk_mcp_connect_sse(&c, url) != 0) {
        fprintf(stderr, "Error: MCP SSE connect failed: %s\n", url);
        return 1;
    }
    int rc = 0;
    if (list) {
        RkMcpTool *tools = NULL;
        size_t count = 0;
        if (rk_mcp_list_tools(&c, &tools, &count) != 0) {
            fprintf(stderr, "Error: tools/list failed\n");
            rc = 1;
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("%s", tools[i].name);
                if (tools[i].description && tools[i].description[0])
                    printf(" — %s", tools[i].description);
                printf("\n");
                if (tools[i].input_schema && tools[i].input_schema[0])
                    printf("  schema: %s\n", tools[i].input_schema);
            }
        }
        rk_mcp_tools_free(tools, count);
    }
    if (call) {
        char *result = NULL;
        if (rk_mcp_call_tool(&c, call, args_json ? args_json : "{}", &result) != 0) {
            fprintf(stderr, "Error: tools/call %s failed\n", call);
            rc = 1;
        } else if (result) {
            printf("%s\n", result);
            free(result);
        }
    }
    rk_mcp_disconnect(&c);
    return rc;
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
    const char *mcp_url = NULL;
    int mcp_list = 0;
    const char *mcp_call = NULL;
    const char *mcp_args = NULL;

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
        } else if (strcmp(argv[i], "--mcp") == 0 && i + 1 < argc) {
            mcp_url = argv[++i];
        } else if (strcmp(argv[i], "--mcp-list") == 0) {
            mcp_list = 1;
        } else if (strcmp(argv[i], "--mcp-call") == 0 && i + 1 < argc) {
            mcp_call = argv[++i];
        } else if (strcmp(argv[i], "--mcp-args") == 0 && i + 1 < argc) {
            mcp_args = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            prompt = argv[i];
        }
    }

    /* MCP 模式（不需要 API key） */
    if (mcp_url) return mcp_main(mcp_url, mcp_list, mcp_call, mcp_args);

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

    RikkaProviderCfg cfg = {provider, base_url, api_key, model, max_tokens, 0,
                             NULL, {0}, NULL, 0, -1, -1, NULL};

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
