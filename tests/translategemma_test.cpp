// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "chatbot.h"
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
    char payload[] = "[{\"role\":\"user\",\"content\":\"hello\"}]";
    char *argv[] = {prog, flag1, payload, nullptr};
    TranslateGemmaOptions opts;
    std::string error;
    ASSERT_TRUE(parse_translategemma_options(3, argv, &opts, &error), "messages json args should parse");
    ASSERT_TRUE(opts.enabled, "messages json should enable translate mode");
    ASSERT_FALSE(opts.image_mode, "messages json should not force legacy image mode");
    ASSERT_STR_EQ(std::string(payload), opts.messages_json, "messages json payload should be captured");
}

TEST(build_text_prompt) {
    auto prompt = build_translategemma_text_prompt("en", "zh-CN", "Hello world");
    ASSERT_STR_EQ(
        std::string("You are a professional English (en) to Chinese (zh-CN) translator. Your goal is to accurately convey the meaning and nuances of the original English text while adhering to Chinese grammar, vocabulary, and cultural sensitivities.\n"
                    "Output plain translated text only. Do not output HTML, XML, Markdown, or any tags like <...>.\n"
                    "Produce only the Chinese translation, without any additional explanations or commentary. Please translate the following English text into Chinese:\n\n\nHello world"),
        prompt,
        "text prompt should match expected template");
}

TEST(build_image_prompt) {
    auto prompt = build_translategemma_image_prompt("en", "zh-CN");
    ASSERT_STR_EQ(
        std::string("You are a professional English (en) to Chinese (zh-CN) translator. Your goal is to accurately convey the meaning and nuances of the original English text while adhering to Chinese grammar, vocabulary, and cultural sensitivities.\n"
                    "Output plain translated text only. Do not output HTML, XML, Markdown, or any tags like <...>.\n"
                    "Please translate the English text in the provided image into Chinese. Produce only the Chinese translation, without any additional explanations, alternatives or commentary. Focus only on the text, do not output where the text is located, surrounding objects or any other explanation about the picture. Ignore symbols, pictogram, and arrows!\n\n\n"),
        prompt,
        "image prompt should match expected template");
}

TEST(sanitize_output) {
    auto output = sanitize_translategemma_output("<start_of_turn>model\nHello\n<|file_separator|>\n<end_of_turn>\n");
    ASSERT_STR_EQ(std::string("Hello"), output, "output sanitizer should remove multimodal marker");
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
