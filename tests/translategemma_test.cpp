// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "chatbot.h"
#include "embedded_resource.h"
#include "server_mode.h"
#include "translategemma_request.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace lf::chatbot;

static int test_count = 0;
static int fail_count = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct TestRegister_##name { \
        TestRegister_##name() { test_##name(); } \
    } test_register_##name; \
    static void test_##name()

#define ASSERT_TRUE(actual, msg) \
    do { \
        test_count++; \
        if (!(actual)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            fail_count++; \
        } \
    } while (0)

#define ASSERT_FALSE(actual, msg) \
    do { \
        test_count++; \
        if ((actual)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            fail_count++; \
        } \
    } while (0)

#define ASSERT_STR_EQ(expected, actual, msg) \
    do { \
        test_count++; \
        if ((expected) != (actual)) { \
            fprintf(stderr, "FAIL: %s\n  expected: \"%s\"\n  actual: \"%s\"\n", \
                    msg, (expected).c_str(), (actual).c_str()); \
            fail_count++; \
        } \
    } while (0)

TEST(parse_translate_text_args) {
    char prog[] = "llamafile";
    char flag1[] = "--translate-text";
    char text[] = "Hello world";
    char flag2[] = "--source-lang";
    char source[] = "en";
    char flag3[] = "--target-lang";
    char target[] = "zh-CN";
    char *argv[] = {prog, flag1, text, flag2, source, flag3, target, nullptr};
    TranslateGemmaOptions opts;
    std::string error;
    ASSERT_TRUE(parse_translategemma_options(7, argv, &opts, &error), "text args should parse");
    ASSERT_TRUE(opts.enabled, "translate mode should be enabled");
    ASSERT_FALSE(opts.image_mode, "text mode should not be image mode");
    ASSERT_STR_EQ(std::string("Hello world"), opts.text, "text payload should be captured");
}

TEST(parse_translation_instruction_args) {
    char prog[] = "llamafile";
    char flag1[] = "--translate-text";
    char text[] = "Hello world";
    char flag2[] = "--source-lang";
    char source[] = "en";
    char flag3[] = "--target-lang";
    char target[] = "zh-CN";
    char flag4[] = "--translation-instruction";
    char instruction[] = "Keep brand names in English.";
    char *argv[] = {prog, flag1, text, flag2, source, flag3, target, flag4, instruction, nullptr};
    TranslateGemmaOptions opts;
    std::string error;
    ASSERT_TRUE(parse_translategemma_options(9, argv, &opts, &error), "translation instruction args should parse");
    ASSERT_STR_EQ(std::string("Keep brand names in English."), opts.translation_instruction,
                  "translation instruction should be captured");
}

TEST(parse_rejects_missing_target_lang) {
    char prog[] = "llamafile";
    char flag1[] = "--translate-text";
    char text[] = "Hello world";
    char flag2[] = "--source-lang";
    char source[] = "en";
    char *argv[] = {prog, flag1, text, flag2, source, nullptr};
    TranslateGemmaOptions opts;
    std::string error;
    ASSERT_FALSE(parse_translategemma_options(5, argv, &opts, &error), "missing target lang should fail");
    ASSERT_STR_EQ(std::string("missing required flag --target-lang"), error, "error message should be stable");
}

TEST(parse_translate_messages_json_args) {
    char prog[] = "llamafile";
    char flag1[] = "--translate-messages-json";
    char payload[] = "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"source_lang_code\":\"en\",\"target_lang_code\":\"zh-CN\",\"text\":\"hello\"}]}]";
    char *argv[] = {prog, flag1, payload, nullptr};
    TranslateGemmaOptions opts;
    std::string error;
    ASSERT_TRUE(parse_translategemma_options(3, argv, &opts, &error), "messages json args should parse");
    ASSERT_TRUE(opts.enabled, "messages json should enable translate mode");
    ASSERT_FALSE(opts.image_mode, "messages json should not force legacy image mode");
    ASSERT_STR_EQ(std::string(payload), opts.messages_json, "messages json payload should be captured");
}

TEST(parse_rejects_instruction_with_messages_json) {
    char prog[] = "llamafile";
    char flag1[] = "--translate-messages-json";
    char payload[] = "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"source_lang_code\":\"en\",\"target_lang_code\":\"zh-CN\",\"text\":\"hello\"}]}]";
    char flag2[] = "--translation-instruction";
    char instruction[] = "Keep product names in English.";
    char *argv[] = {prog, flag1, payload, flag2, instruction, nullptr};
    TranslateGemmaOptions opts;
    std::string error;
    ASSERT_FALSE(parse_translategemma_options(5, argv, &opts, &error),
                 "translation instruction should be rejected with messages json");
    ASSERT_STR_EQ(std::string("--translation-instruction is not supported with --translate-messages-json"), error,
                  "messages json rejection should be stable");
}

TEST(sanitize_output) {
    auto output = sanitize_translategemma_output("<start_of_turn>model\nHello\n<|file_separator|>\n<end_of_turn>\n");
    ASSERT_STR_EQ(std::string("Hello"), output, "output sanitizer should remove multimodal marker");
}

TEST(sanitize_output_slash_turn_marker) {
    auto output = sanitize_translategemma_output("Hello\n</start_of_turn>\n");
    ASSERT_STR_EQ(std::string("Hello"), output, "output sanitizer should remove slash turn marker");
}

TEST(resolve_embedded_path_prefers_zip_bundle_for_missing_bare_name) {
    auto resolved = resolve_embedded_path("model.gguf", [](std::string_view path) {
        return path == "/zip/model.gguf";
    });
    ASSERT_STR_EQ(std::string("/zip/model.gguf"), resolved.path, "bare filename should resolve to bundled /zip path");
    ASSERT_TRUE(resolved.is_zip, "resolved bundled model should be marked as zip-backed");
    ASSERT_TRUE(resolved.resolved_from_bundle, "resolution should report bundled fallback");
}

TEST(resolve_embedded_path_keeps_existing_host_path) {
    auto resolved = resolve_embedded_path("/tmp/model.gguf", [](std::string_view path) {
        return path == "/tmp/model.gguf";
    });
    ASSERT_STR_EQ(std::string("/tmp/model.gguf"), resolved.path, "explicit host path should remain unchanged");
    ASSERT_FALSE(resolved.is_zip, "host path should not be marked as zip-backed");
    ASSERT_FALSE(resolved.resolved_from_bundle, "host path should not report bundled fallback");
}

TEST(resolve_embedded_path_recognizes_explicit_zip_path) {
    auto resolved = resolve_embedded_path("/zip/model.gguf", [](std::string_view) {
        return false;
    });
    ASSERT_STR_EQ(std::string("/zip/model.gguf"), resolved.path, "explicit /zip path should be preserved");
    ASSERT_TRUE(resolved.is_zip, "explicit /zip path should be marked as zip-backed");
    ASSERT_FALSE(resolved.resolved_from_bundle, "explicit /zip path should not report implicit fallback");
}

TEST(server_mode_detects_translate_flags) {
    char prog[] = "llamafile";
    char flag1[] = "--server";
    char flag2[] = "--source-lang";
    char source[] = "en";
    char *argv[] = {prog, flag1, flag2, source, nullptr};
    ASSERT_TRUE(has_translategemma_flags(4, argv), "server mode should reject any TranslateGemma-specific flags");
}

TEST(server_mode_detects_explicit_chat_template_override) {
    char prog[] = "llamafile";
    char flag1[] = "--server";
    char flag2[] = "--chat-template";
    char tmpl[] = "chatml";
    char *argv[] = {prog, flag1, flag2, tmpl, nullptr};
    ASSERT_TRUE(has_explicit_chat_template_override(4, argv),
                "explicit --chat-template should disable automatic fallback");
}

TEST(server_mode_fallback_matches_translategemma_template_shape) {
    ASSERT_TRUE(
        should_use_server_safe_gemma_template(
            "gemma3",
            "{{ message.content[0].source_lang_code }} -> {{ message.content[0].target_lang_code }}"),
        "TranslateGemma-style Gemma3 template should trigger server-safe fallback");
    ASSERT_FALSE(
        should_use_server_safe_gemma_template(
            "gemma3",
            "{{ message.content }}"),
        "regular Gemma3 chat template should not trigger fallback");
    ASSERT_FALSE(
        should_use_server_safe_gemma_template(
            "llama",
            "{{ message.content[0].source_lang_code }}"),
        "non-Gemma3 models should not trigger fallback");
}

TEST(http_translate_text_defaults) {
    TranslateGemmaJson body = {
        {"text", "Hello world"},
    };
    TranslateGemmaHttpRequest request;
    std::string error;
    ASSERT_TRUE(parse_translategemma_http_translate_request(body, &request, &error),
                "text translate request should parse");
    ASSERT_FALSE(request.request.has_media, "text translate request should not mark media");
    ASSERT_TRUE(request.generation.contains("max_tokens"), "default max_tokens should be populated");
    ASSERT_STR_EQ(std::string("512"), std::to_string(request.max_tokens),
                  "text translate request should default max_tokens to 512");
    ASSERT_TRUE(request.generation.contains("stop"), "translate route should append stop tokens");
}

TEST(http_translate_rejects_both_text_and_image) {
    TranslateGemmaJson body = {
        {"text", "Hello"},
        {"image_data_url", "data:image/png;base64,AAAA"},
    };
    TranslateGemmaHttpRequest request;
    std::string error;
    ASSERT_FALSE(parse_translategemma_http_translate_request(body, &request, &error),
                 "translate route should reject both text and image");
    ASSERT_STR_EQ(std::string("exactly one of 'text' or 'image_data_url' is required"), error,
                  "translate route mutual exclusion error should be stable");
}

TEST(http_translate_rejects_non_data_uri_image) {
    TranslateGemmaJson body = {
        {"image_data_url", "/tmp/test.png"},
    };
    TranslateGemmaHttpRequest request;
    std::string error;
    ASSERT_FALSE(parse_translategemma_http_translate_request(body, &request, &error),
                 "translate route should reject non-data-uri images");
    ASSERT_STR_EQ(std::string("image references must be data URIs for HTTP translate routes"), error,
                  "translate route should require data uri images");
}

TEST(http_translate_accepts_valid_data_uri_image) {
    const std::string gif_data_uri =
        "data:image/gif;base64,"
        "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==";
    TranslateGemmaJson body = {
        {"image_data_url", gif_data_uri},
    };
    TranslateGemmaHttpRequest request;
    std::string error;
    ASSERT_TRUE(parse_translategemma_http_translate_request(body, &request, &error),
                "translate route should accept valid image data URIs");
    ASSERT_TRUE(request.request.has_media, "valid image data uri should mark media");
    ASSERT_TRUE(!request.request.images.empty(), "valid image data uri should decode image bytes");
}

TEST(http_translate_messages_supports_translation_instruction) {
    TranslateGemmaJson body = {
        {"messages", TranslateGemmaJson::array({
            {
                {"role", "user"},
                {"content", TranslateGemmaJson::array({
                    {
                        {"type", "text"},
                        {"source_lang_code", "en"},
                        {"target_lang_code", "zh-CN"},
                        {"text", "Hello"},
                    },
                })},
            },
        })},
        {"translation_instruction", "Keep product names in English."},
    };
    TranslateGemmaHttpRequest request;
    std::string error;
    ASSERT_TRUE(parse_translategemma_http_messages_request(body, &request, &error),
                "messages translate route should accept translation instruction");
    ASSERT_STR_EQ(std::string("Keep product names in English."), request.translation_instruction,
                  "messages route should capture translation instruction");
}

TEST(http_translate_messages_rejects_streaming_images) {
    const std::string png_data_uri =
        "data:image/gif;base64,"
        "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==";
    TranslateGemmaJson body = {
        {"messages", TranslateGemmaJson::array({
            {
                {"role", "user"},
                {"content", TranslateGemmaJson::array({
                    {
                        {"type", "image"},
                        {"source_lang_code", "auto"},
                        {"target_lang_code", "zh-CN"},
                        {"url", png_data_uri},
                    },
                })},
            },
        })},
        {"stream", true},
    };
    TranslateGemmaHttpRequest request;
    std::string error;
    ASSERT_FALSE(parse_translategemma_http_messages_request(body, &request, &error),
                 "messages route should reject streaming image translation");
    ASSERT_STR_EQ(std::string("streaming image translation is not supported"), error,
                  "messages route should reject streaming image translation after validation");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    fprintf(stderr, "Running translategemma tests...\n");

    if (fail_count > 0) {
        fprintf(stderr, "\n%d/%d tests FAILED\n", fail_count, test_count);
        return 1;
    }

    fprintf(stderr, "All %d tests PASSED\n", test_count);
    return 0;
}
