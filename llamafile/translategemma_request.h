// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

struct llama_vocab;

namespace lf {
namespace chatbot {

using TranslateGemmaJson = nlohmann::ordered_json;

enum class TranslateGemmaImageReferencePolicy {
    kAllowLocalPathOrDataUri,
    kDataUriOnly,
};

struct TranslateGemmaImagePayload {
    std::string data_uri;
    std::string bytes;
};

struct TranslateGemmaPreparedMessagesRequest {
    TranslateGemmaJson messages = TranslateGemmaJson::array();
    std::vector<TranslateGemmaImagePayload> images;
    bool has_media = false;
};

struct TranslateGemmaRenderedRequest {
    std::string prompt;
    std::vector<std::string> image_bytes;
    bool has_media = false;
};

struct TranslateGemmaHttpRequest {
    TranslateGemmaPreparedMessagesRequest request;
    TranslateGemmaJson generation = TranslateGemmaJson::object();
    std::string translation_instruction;
    bool stream = false;
    int max_tokens = 0;
};

bool is_translategemma_language_code_valid(std::string_view code);
std::string trim_translategemma_whitespace(std::string value);
bool is_translategemma_chat_template_source(std::string_view chat_template_source);

TranslateGemmaPreparedMessagesRequest build_translategemma_text_request(
    std::string_view source_lang,
    std::string_view target_lang,
    std::string_view text);

TranslateGemmaPreparedMessagesRequest build_translategemma_image_request_from_path(
    std::string_view source_lang,
    std::string_view target_lang,
    std::string_view image_path);

TranslateGemmaPreparedMessagesRequest build_translategemma_image_request_from_data_uri(
    std::string_view source_lang,
    std::string_view target_lang,
    std::string_view data_uri);

TranslateGemmaPreparedMessagesRequest parse_translategemma_messages_json_source(
    std::string_view source,
    TranslateGemmaImageReferencePolicy image_policy);

TranslateGemmaPreparedMessagesRequest parse_translategemma_messages_payload(
    const TranslateGemmaJson &payload,
    TranslateGemmaImageReferencePolicy image_policy);

TranslateGemmaRenderedRequest render_translategemma_request(
    std::string_view template_source,
    const llama_vocab *vocab,
    const TranslateGemmaPreparedMessagesRequest &request,
    std::string_view translation_instruction,
    std::string_view media_placeholder = {});

bool parse_translategemma_http_translate_request(
    const TranslateGemmaJson &body,
    TranslateGemmaHttpRequest *out,
    std::string *error);

bool parse_translategemma_http_messages_request(
    const TranslateGemmaJson &body,
    TranslateGemmaHttpRequest *out,
    std::string *error);

void apply_translategemma_stop_tokens(TranslateGemmaJson *generation);

} // namespace chatbot
} // namespace lf
