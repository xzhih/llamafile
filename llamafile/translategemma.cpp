// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "chatbot.h"

#include <cctype>
#include <cstring>

namespace lf {
namespace chatbot {

TranslateGemmaOptions g_translate_options;

static bool is_language_code_valid(std::string_view code) {
    if (code == "auto")
        return true;
    if (code.size() != 2 && code.size() != 5)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(code[0])) ||
        !std::isalpha(static_cast<unsigned char>(code[1])))
        return false;
    if (code.size() == 2)
        return true;
    if (code[2] != '-' && code[2] != '_')
        return false;
    return std::isalpha(static_cast<unsigned char>(code[3])) &&
           std::isalpha(static_cast<unsigned char>(code[4]));
}

static const char *arg_value(int argc, char **argv, int *index, std::string *error) {
    if (*index + 1 >= argc) {
        *error = std::string("missing value for ") + argv[*index];
        return nullptr;
    }
    ++*index;
    return argv[*index];
}

bool parse_translategemma_options(int argc, char **argv, TranslateGemmaOptions *out, std::string *error) {
    *out = TranslateGemmaOptions{};
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--translate-text") {
            if (out->enabled) {
                *error = "only one of --translate-text or --translate-image may be specified";
                return false;
            }
            const char *value = arg_value(argc, argv, &i, error);
            if (!value)
                return false;
            out->enabled = true;
            out->image_mode = false;
            out->text = value;
        } else if (arg == "--translate-image") {
            if (out->enabled) {
                *error = "only one of --translate-text or --translate-image may be specified";
                return false;
            }
            const char *value = arg_value(argc, argv, &i, error);
            if (!value)
                return false;
            out->enabled = true;
            out->image_mode = true;
            out->image_path = value;
        } else if (arg == "--source-lang") {
            const char *value = arg_value(argc, argv, &i, error);
            if (!value)
                return false;
            out->source_lang = value;
        } else if (arg == "--target-lang") {
            const char *value = arg_value(argc, argv, &i, error);
            if (!value)
                return false;
            out->target_lang = value;
        }
    }

    if (!out->enabled)
        return true;
    if (out->source_lang.empty()) {
        *error = "missing required flag --source-lang";
        return false;
    }
    if (out->target_lang.empty()) {
        *error = "missing required flag --target-lang";
        return false;
    }
    if (!is_language_code_valid(out->source_lang)) {
        *error = "invalid source language code";
        return false;
    }
    if (!is_language_code_valid(out->target_lang)) {
        *error = "invalid target language code";
        return false;
    }
    if (out->image_mode && out->image_path.empty()) {
        *error = "missing image path";
        return false;
    }
    if (!out->image_mode && out->text.empty()) {
        *error = "missing translation text";
        return false;
    }
    return true;
}

std::string build_translategemma_text_prompt(std::string_view source_lang,
                                             std::string_view target_lang,
                                             std::string_view text) {
    std::string prompt = "Translate from ";
    prompt += source_lang;
    prompt += " to ";
    prompt += target_lang;
    prompt += ". Preserve line breaks when possible.\n\n";
    prompt.append(text.data(), text.size());
    return prompt;
}

std::string build_translategemma_image_prompt(std::string_view source_lang,
                                              std::string_view target_lang) {
    std::string prompt = "Extract and translate all text in this image from ";
    prompt += source_lang;
    prompt += " to ";
    prompt += target_lang;
    prompt += ". Return only the translated text. Do not repeat the source text. Ignore icons, symbols, arrows, and non-text visual elements.";
    return prompt;
}

std::string sanitize_translategemma_output(std::string_view raw) {
    std::string clean(raw);
    while (true) {
        size_t pos = clean.find("<start_of_image>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<start_of_image>"));
    }
    while (true) {
        size_t pos = clean.find("<end_of_image>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<end_of_image>"));
    }
    while (true) {
        size_t pos = clean.find("<__media__>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<__media__>"));
    }
    while (true) {
        size_t pos = clean.find("<|im_end|>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<|im_end|>"));
    }
    while (true) {
        size_t pos = clean.find("<|im_start|>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<|im_start|>"));
    }
    while (true) {
        size_t pos = clean.find("<start_of_turn>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<start_of_turn>"));
    }
    while (true) {
        size_t pos = clean.find("<end_of_turn>");
        if (pos == std::string::npos)
            break;
        clean.erase(pos, strlen("<end_of_turn>"));
    }
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r'))
        clean.pop_back();
    return clean;
}

} // namespace chatbot
} // namespace lf
