#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define EXPECT_STR(got, want) do { \
    if (strcmp((got),(want)) != 0) { \
        printf("FAIL %s:%d\n  got:  [%s]\n  want: [%s]\n", \
               __FILE__, __LINE__, (got), (want)); \
        failures++; \
    } else { printf("ok  line %d\n", __LINE__); } \
} while(0)

#define EXPECT_INT(got, want) do { \
    if ((int)(got) != (int)(want)) { \
        printf("FAIL %s:%d  got=%d want=%d\n", __FILE__, __LINE__, (int)(got), (int)(want)); \
        failures++; \
    } else { printf("ok  line %d\n", __LINE__); } \
} while(0)

#define EXPECT_BOOL(got, want) EXPECT_INT((int)(got), (int)(want))


/* ---- JsonBuilder ---- */

static void test_builder_simple() {
    char buf[512];
    JsonBuilder b;
    b.reset(buf, sizeof(buf));
    b.beginObj();
    b.str("model", "llama3");
    b.boolean("stream", true);
    b.endObj();
    b.finish();
    EXPECT_STR(buf, "{\"model\":\"llama3\",\"stream\":true}");
}

static void test_builder_nested_array() {
    char buf[512];
    JsonBuilder b;
    b.reset(buf, sizeof(buf));
    b.beginObj();
    b.str("model", "llama3");
    b.beginArr("messages");
      b.beginObj();
      b.str("role", "user");
      b.str("content", "hello world");
      b.endObj();
    b.endArr();
    b.boolean("stream", true);
    b.endObj();
    b.finish();
    EXPECT_STR(buf,
        "{\"model\":\"llama3\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello world\"}],"
        "\"stream\":true}");
}

static void test_builder_escaping() {
    char buf[256];
    JsonBuilder b;
    b.reset(buf, sizeof(buf));
    b.beginObj();
    b.str("msg", "say \"hi\"\nbye");
    b.endObj();
    b.finish();
    EXPECT_STR(buf, "{\"msg\":\"say \\\"hi\\\"\\nbye\"}");
}


/* ---- JsonParser ---- */

static void test_parser_get_string() {
    JsonParser p;
    char out[64];
    const char *j = "{\"model\":\"llama3\",\"done\":false}";
    EXPECT_INT(p.getString(j, "model", out, sizeof(out)), 0);
    EXPECT_STR(out, "llama3");
}

static void test_parser_get_bool() {
    JsonParser p;
    bool v = true;
    const char *j = "{\"done\":false,\"model\":\"llama3\"}";
    EXPECT_INT(p.getBool(j, "done", v), 0);
    EXPECT_BOOL(v, false);
}

static void test_parser_missing_key() {
    JsonParser p;
    char out[64];
    const char *j = "{\"model\":\"llama3\"}";
    EXPECT_INT(p.getString(j, "nothere", out, sizeof(out)), -1);
}

static void test_parser_nested() {
    JsonParser p;
    char out[256];
    /* Typical Ollama streaming line */
    const char *j =
        "{\"model\":\"llama3\",\"created_at\":\"2024-01-01\","
        "\"message\":{\"role\":\"assistant\",\"content\":\"Hello\"},"
        "\"done\":false}";
    EXPECT_INT(p.getNested(j, "message", "content", out, sizeof(out)), 0);
    EXPECT_STR(out, "Hello");
}

static void test_parser_nested_escape() {
    JsonParser p;
    char out[256];
    const char *j =
        "{\"message\":{\"role\":\"assistant\","
        "\"content\":\"line1\\nline2\"}}";
    EXPECT_INT(p.getNested(j, "message", "content", out, sizeof(out)), 0);
    EXPECT_STR(out, "line1\nline2");
}

/* OpenAI SSE streaming format tests */

static void test_openai_delta_content() {
    JsonParser p;
    char out[256];
    const char *j =
        "{\"id\":\"x\",\"choices\":[{\"delta\":{\"content\":\"hello\"},"
        "\"finish_reason\":null}]}";
    EXPECT_INT(p.getDeltaContent(j, out, sizeof(out)), 0);
    EXPECT_STR(out, "hello");
}

static void test_openai_delta_content_empty() {
    JsonParser p;
    char out[256];
    /* First chunk often has role but no content */
    const char *j =
        "{\"choices\":[{\"delta\":{\"role\":\"assistant\"},"
        "\"finish_reason\":null}]}";
    EXPECT_INT(p.getDeltaContent(j, out, sizeof(out)), -1);
}

static void test_openai_finish_reason() {
    JsonParser p;
    char out[64];
    const char *j =
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}";
    EXPECT_INT(p.getFinishReason(j, out, sizeof(out)), 0);
    EXPECT_STR(out, "stop");
}

static void test_openai_finish_reason_null() {
    JsonParser p;
    char out[64];
    const char *j =
        "{\"choices\":[{\"delta\":{\"content\":\"tok\"},\"finish_reason\":null}]}";
    EXPECT_INT(p.getFinishReason(j, out, sizeof(out)), -1);
}

static void test_openai_tool_call() {
    JsonParser p;
    char name[64], args[256];
    /* OpenAI tool call delta */
    const char *j =
        "{\"choices\":[{\"delta\":{"
          "\"tool_calls\":[{\"index\":0,\"function\":{"
            "\"name\":\"read_file\","
            "\"arguments\":\"{\\\"filename\\\":\\\"foo.txt\\\"}\""
          "}}]"
        "},\"finish_reason\":null}]}";
    EXPECT_INT(p.getDeltaToolCall(j, 0, name, sizeof(name), args, sizeof(args)), 0);
    EXPECT_STR(name, "read_file");
    EXPECT_STR(args, "{\"filename\":\"foo.txt\"}");

    EXPECT_INT(p.getDeltaToolCall(j, 1, name, sizeof(name), args, sizeof(args)), 1);
}

static void test_openai_finish_tool_calls() {
    JsonParser p;
    char out[64];
    const char *j =
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}";
    EXPECT_INT(p.getFinishReason(j, out, sizeof(out)), 0);
    EXPECT_STR(out, "tool_calls");
}

/* Full round-trip: build an Ollama request, verify key fields parseable */
static void test_roundtrip() {
    char reqBuf[1024];
    JsonBuilder b;
    b.reset(reqBuf, sizeof(reqBuf));
    b.beginObj();
    b.str("model", "llama3");
    b.beginArr("messages");
      b.beginObj();
      b.str("role", "user");
      b.str("content", "What is 2+2?");
      b.endObj();
    b.endArr();
    b.boolean("stream", true);
    b.endObj();
    b.finish();

    JsonParser p;
    char out[64];
    bool bv = false;
    EXPECT_INT(p.getString(reqBuf, "model", out, sizeof(out)), 0);
    EXPECT_STR(out, "llama3");
    EXPECT_INT(p.getBool(reqBuf, "stream", bv), 0);
    EXPECT_BOOL(bv, true);
}

int main() {
    test_builder_simple();
    test_builder_nested_array();
    test_builder_escaping();
    test_parser_get_string();
    test_parser_get_bool();
    test_parser_missing_key();
    test_parser_nested();
    test_parser_nested_escape();
    test_openai_delta_content();
    test_openai_delta_content_empty();
    test_openai_finish_reason();
    test_openai_finish_reason_null();
    test_openai_tool_call();
    test_openai_finish_tool_calls();
    test_roundtrip();

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        printf("\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
