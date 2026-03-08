// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "translategemma_request.h"

#include <cctype>
#include <stdexcept>

#include "datauri.h"
#include "image.h"
#include "string.h"

namespace lf {
namespace chatbot {

namespace {

TranslateGemmaJson copy_generation_body(const TranslateGemmaJson &body) {
    if (!body.is_object()) {
        throw std::invalid_argument("request body must be a JSON object");
    }
    return body;
}

bool read_optional_string_field(const TranslateGemmaJson &body,
                                const char *key,
                                std::string *out,
                                std::string *error) {
    if (!body.contains(key)) {
        return true;
    }
    if (!body.at(key).is_string()) {
        *error = std::string("'") + key + "' must be a string";
        return false;
    }
    *out = trim_translategemma_whitespace(body.at(key).get<std::string>());
    return true;
}

bool read_optional_bool_field(const TranslateGemmaJson &body,
                              const char *key,
                              bool *out,
                              std::string *error) {
    if (!body.contains(key)) {
        return true;
    }
    if (!body.at(key).is_boolean()) {
        *error = std::string("'") + key + "' must be a boolean";
        return false;
    }
    *out = body.at(key).get<bool>();
    return true;
}

bool resolve_max_tokens(const TranslateGemmaJson &body,
                        int default_value,
                        int *out,
                        std::string *error) {
    if (body.contains("n_predict")) {
        if (!body.at("n_predict").is_number_integer()) {
            *error = "'n_predict' must be an integer";
            return false;
        }
        *out = body.at("n_predict").get<int>();
        return true;
    }
    if (body.contains("max_completion_tokens")) {
        if (!body.at("max_completion_tokens").is_number_integer()) {
            *error = "'max_completion_tokens' must be an integer";
            return false;
        }
        *out = body.at("max_completion_tokens").get<int>();
        return true;
    }
    if (body.contains("max_tokens")) {
        if (!body.at("max_tokens").is_number_integer()) {
            *error = "'max_tokens' must be an integer";
            return false;
        }
        *out = body.at("max_tokens").get<int>();
        return true;
    }
    *out = default_value;
    return true;
}

bool payload_contains_image_message(const TranslateGemmaJson &payload_in) {
    TranslateGemmaJson payload = payload_in;
    if (payload.is_object() && payload.contains("messages")) {
        payload = payload.at("messages");
    }
    if (!payload.is_array()) {
        return false;
    }
    for (const auto &message : payload) {
        if (!message.is_object() || !message.contains("content") || !message.at("content").is_array()) {
            continue;
        }
        for (const auto &item : message.at("content")) {
            if (item.is_object() && item.contains("type") && item.at("type").is_string() &&
                item.at("type").get<std::string>() == "image") {
                return true;
            }
        }
    }
    return false;
}

TranslateGemmaImagePayload normalize_image_reference(std::string value,
                                                     TranslateGemmaImageReferencePolicy image_policy) {
    value = trim_translategemma_whitespace(std::move(value));
    if (value.empty()) {
        throw std::invalid_argument("image content item is missing a usable image reference");
    }

    if (startscasewith(value, "data:")) {
        if (!startscasewith(value, "data:image/")) {
            throw std::invalid_argument("image data URI must have an image/* mime type");
        }
        DataUri uri;
        const std::string_view uri_payload = std::string_view(value).substr(strlen("data:"));
        if (uri.parse(uri_payload) != uri_payload.size()) {
            throw std::invalid_argument("invalid image data URI");
        }
        std::string bytes = uri.decode();
        if (!is_image(bytes)) {
            throw std::invalid_argument("invalid image data URI payload");
        }
        TranslateGemmaImagePayload image;
        image.data_uri = std::move(value);
        image.bytes = std::move(bytes);
        return image;
    }

    if (image_policy == TranslateGemmaImageReferencePolicy::kDataUriOnly) {
        throw std::invalid_argument("image references must be data URIs for HTTP translate routes");
    }

    if (startscasewith(value, "http://") || startscasewith(value, "https://")) {
        throw std::invalid_argument(
            "remote image urls are not supported in translate mode; use a data URI or local file path");
    }
    if (startscasewith(value, "file://")) {
        value.erase(0, strlen("file://"));
    }

    std::string image;
    if (!slurp(&image, value.c_str())) {
        throw std::invalid_argument("failed to read image file: " + value);
    }
    if (!is_image(image)) {
        throw std::invalid_argument("unsupported image file: " + value);
    }

    std::string data_uri;
    convert_image_to_uri(&data_uri, image);
    TranslateGemmaImagePayload result;
    result.data_uri = std::move(data_uri);
    result.bytes = std::move(image);
    return result;
}

std::string load_messages_json_source(std::string_view source) {
    std::string raw = trim_translategemma_whitespace(std::string(source));
    if (raw.empty()) {
        throw std::invalid_argument("empty --translate-messages-json payload");
    }
    if (raw.front() == '[' || raw.front() == '{') {
        return raw;
    }

    std::string file_content;
    if (!slurp(&file_content, raw.c_str())) {
        throw std::invalid_argument("failed to read messages json: " + raw);
    }
    return file_content;
}

std::string require_string_field(const TranslateGemmaJson &object,
                                 const char *field,
                                 const char *context) {
    if (!object.is_object() || !object.contains(field) || !object.at(field).is_string()) {
        throw std::invalid_argument(std::string(context) + " is missing required field '" + field + "'");
    }
    return object.at(field).get<std::string>();
}

void validate_assistant_message(const TranslateGemmaJson &message) {
    if (!message.contains("content") || !message.at("content").is_string()) {
        throw std::invalid_argument("assistant message content must be a string");
    }
}

void validate_user_message(TranslateGemmaJson &message,
                           TranslateGemmaImageReferencePolicy image_policy,
                           std::vector<TranslateGemmaImagePayload> *images) {
    if (!message.contains("content") || !message.at("content").is_array()) {
        throw std::invalid_argument("user message content must be an array");
    }

    auto &content_items = message["content"];
    if (content_items.size() != 1) {
        throw std::invalid_argument("user message content must contain exactly one item");
    }
    if (!content_items[0].is_object()) {
        throw std::invalid_argument("user message content item must be an object");
    }

    auto &content = content_items[0];
    const std::string source_lang = require_string_field(content, "source_lang_code", "user message content item");
    const std::string target_lang = require_string_field(content, "target_lang_code", "user message content item");
    if (!is_translategemma_language_code_valid(source_lang)) {
        throw std::invalid_argument("invalid source language code in messages json");
    }
    if (!is_translategemma_language_code_valid(target_lang)) {
        throw std::invalid_argument("invalid target language code in messages json");
    }

    const std::string type = require_string_field(content, "type", "user message content item");
    if (type == "text") {
        require_string_field(content, "text", "text content item");
        return;
    }
    if (type != "image") {
        throw std::invalid_argument("user message content item type must be 'text' or 'image'");
    }

    TranslateGemmaImagePayload image = normalize_image_reference(
        require_string_field(content, "url", "image content item"),
        image_policy);
    content["url"] = image.data_uri;
    images->push_back(std::move(image));
}

void erase_translate_control_fields(TranslateGemmaJson *generation) {
    generation->erase("text");
    generation->erase("image_data_url");
    generation->erase("messages");
    generation->erase("source_lang");
    generation->erase("target_lang");
    generation->erase("translation_instruction");
}

} // namespace

bool is_translategemma_language_code_valid(std::string_view code) {
    if (code == "auto") {
        return true;
    }
    if (code.size() != 2 && code.size() != 5) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(code[0])) ||
        !std::isalpha(static_cast<unsigned char>(code[1]))) {
        return false;
    }
    if (code.size() == 2) {
        return true;
    }
    if (code[2] != '-' && code[2] != '_') {
        return false;
    }
    return std::isalpha(static_cast<unsigned char>(code[3])) &&
           std::isalpha(static_cast<unsigned char>(code[4]));
}

std::string trim_translategemma_whitespace(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool is_translategemma_chat_template_source(std::string_view chat_template_source) {
    return chat_template_source.find("source_lang_code") != std::string_view::npos &&
           chat_template_source.find("target_lang_code") != std::string_view::npos;
}

TranslateGemmaPreparedMessagesRequest build_translategemma_text_request(
    std::string_view source_lang,
    std::string_view target_lang,
    std::string_view text) {
    TranslateGemmaPreparedMessagesRequest request;
    request.messages = TranslateGemmaJson::array({
        {
            {"role", "user"},
            {"content", TranslateGemmaJson::array({
                {
                    {"type", "text"},
                    {"source_lang_code", std::string(source_lang)},
                    {"target_lang_code", std::string(target_lang)},
                    {"text", std::string(text)},
                },
            })},
        },
    });
    return request;
}

TranslateGemmaPreparedMessagesRequest build_translategemma_image_request_from_path(
    std::string_view source_lang,
    std::string_view target_lang,
    std::string_view image_path) {
    TranslateGemmaPreparedMessagesRequest request;
    request.has_media = true;
    request.images.push_back(normalize_image_reference(
        std::string(image_path), TranslateGemmaImageReferencePolicy::kAllowLocalPathOrDataUri));
    request.messages = TranslateGemmaJson::array({
        {
            {"role", "user"},
            {"content", TranslateGemmaJson::array({
                {
                    {"type", "image"},
                    {"source_lang_code", std::string(source_lang)},
                    {"target_lang_code", std::string(target_lang)},
                    {"url", request.images.back().data_uri},
                },
            })},
        },
    });
    return request;
}

TranslateGemmaPreparedMessagesRequest build_translategemma_image_request_from_data_uri(
    std::string_view source_lang,
    std::string_view target_lang,
    std::string_view data_uri) {
    TranslateGemmaPreparedMessagesRequest request;
    request.has_media = true;
    request.images.push_back(normalize_image_reference(
        std::string(data_uri), TranslateGemmaImageReferencePolicy::kDataUriOnly));
    request.messages = TranslateGemmaJson::array({
        {
            {"role", "user"},
            {"content", TranslateGemmaJson::array({
                {
                    {"type", "image"},
                    {"source_lang_code", std::string(source_lang)},
                    {"target_lang_code", std::string(target_lang)},
                    {"url", request.images.back().data_uri},
                },
            })},
        },
    });
    return request;
}

TranslateGemmaPreparedMessagesRequest parse_translategemma_messages_json_source(
    std::string_view source,
    TranslateGemmaImageReferencePolicy image_policy) {
    return parse_translategemma_messages_payload(
        TranslateGemmaJson::parse(load_messages_json_source(source)),
        image_policy);
}

TranslateGemmaPreparedMessagesRequest parse_translategemma_messages_payload(
    const TranslateGemmaJson &payload_in,
    TranslateGemmaImageReferencePolicy image_policy) {
    TranslateGemmaJson payload = payload_in;
    if (payload.is_object()) {
        if (payload.size() != 1 || !payload.contains("messages")) {
            throw std::invalid_argument("messages json object must contain only a top-level 'messages' array");
        }
        payload = payload.at("messages");
    }
    if (!payload.is_array()) {
        throw std::invalid_argument("messages json must be an array or an object containing only 'messages'");
    }
    if (payload.empty()) {
        throw std::invalid_argument("messages payload must contain at least one message");
    }

    TranslateGemmaPreparedMessagesRequest result;
    result.messages = std::move(payload);

    bool expect_user = true;
    for (auto &message : result.messages) {
        if (!message.is_object()) {
            throw std::invalid_argument("message entries must be objects");
        }

        const std::string role = require_string_field(message, "role", "message");
        if (role != "user" && role != "assistant") {
            throw std::invalid_argument("message role must be 'user' or 'assistant'");
        }
        if (expect_user && role != "user") {
            throw std::invalid_argument("messages payload must start with role 'user'");
        }
        if (!expect_user && role != "assistant") {
            throw std::invalid_argument("messages payload roles must alternate user/assistant");
        }

        if (role == "user") {
            validate_user_message(message, image_policy, &result.images);
        } else {
            validate_assistant_message(message);
        }

        expect_user = !expect_user;
    }

    result.has_media = !result.images.empty();
    return result;
}

bool parse_translategemma_http_translate_request(
    const TranslateGemmaJson &body,
    TranslateGemmaHttpRequest *out,
    std::string *error) {
    try {
        TranslateGemmaJson generation = copy_generation_body(body);

        std::string text;
        std::string image_data_url;
        std::string source_lang;
        std::string target_lang;
        std::string translation_instruction;
        bool stream = false;

        if (!read_optional_string_field(body, "text", &text, error) ||
            !read_optional_string_field(body, "image_data_url", &image_data_url, error) ||
            !read_optional_string_field(body, "source_lang", &source_lang, error) ||
            !read_optional_string_field(body, "target_lang", &target_lang, error) ||
            !read_optional_string_field(body, "translation_instruction", &translation_instruction, error) ||
            !read_optional_bool_field(body, "stream", &stream, error)) {
            return false;
        }

        const bool has_text = !text.empty();
        const bool has_image = !image_data_url.empty();
        if (has_text == has_image) {
            *error = "exactly one of 'text' or 'image_data_url' is required";
            return false;
        }

        if (source_lang.empty()) {
            source_lang = has_image ? "auto" : "en";
        }
        if (target_lang.empty()) {
            target_lang = "zh-CN";
        }
        if (!is_translategemma_language_code_valid(source_lang)) {
            *error = "invalid source language code";
            return false;
        }
        if (!is_translategemma_language_code_valid(target_lang)) {
            *error = "invalid target language code";
            return false;
        }
        if (has_image && stream) {
            *error = "streaming image translation is not supported";
            return false;
        }

        int max_tokens = 0;
        if (!resolve_max_tokens(body, has_image ? 768 : 512, &max_tokens, error)) {
            return false;
        }
        erase_translate_control_fields(&generation);
        generation["stream"] = stream;
        if (!generation.contains("max_tokens") &&
            !generation.contains("n_predict") &&
            !generation.contains("max_completion_tokens")) {
            generation["max_tokens"] = max_tokens;
        }
        apply_translategemma_stop_tokens(&generation);

        out->request = has_image
            ? build_translategemma_image_request_from_data_uri(source_lang, target_lang, image_data_url)
            : build_translategemma_text_request(source_lang, target_lang, text);
        out->generation = std::move(generation);
        out->translation_instruction = std::move(translation_instruction);
        out->stream = stream;
        out->max_tokens = max_tokens;
        return true;
    } catch (const std::exception &e) {
        *error = e.what();
        return false;
    }
}

bool parse_translategemma_http_messages_request(
    const TranslateGemmaJson &body,
    TranslateGemmaHttpRequest *out,
    std::string *error) {
    try {
        TranslateGemmaJson generation = copy_generation_body(body);
        if (!body.contains("messages")) {
            *error = "'messages' is required";
            return false;
        }

        std::string translation_instruction;
        bool stream = false;
        if (!read_optional_string_field(body, "translation_instruction", &translation_instruction, error) ||
            !read_optional_bool_field(body, "stream", &stream, error)) {
            return false;
        }
        if (stream && payload_contains_image_message(body.at("messages"))) {
            *error = "streaming image translation is not supported";
            return false;
        }

        TranslateGemmaPreparedMessagesRequest request = parse_translategemma_messages_payload(
            body.at("messages"), TranslateGemmaImageReferencePolicy::kDataUriOnly);

        int max_tokens = 0;
        if (!resolve_max_tokens(body, request.has_media ? 768 : 512, &max_tokens, error)) {
            return false;
        }
        erase_translate_control_fields(&generation);
        generation["stream"] = stream;
        if (!generation.contains("max_tokens") &&
            !generation.contains("n_predict") &&
            !generation.contains("max_completion_tokens")) {
            generation["max_tokens"] = max_tokens;
        }
        apply_translategemma_stop_tokens(&generation);

        out->request = std::move(request);
        out->generation = std::move(generation);
        out->translation_instruction = std::move(translation_instruction);
        out->stream = stream;
        out->max_tokens = max_tokens;
        return true;
    } catch (const std::exception &e) {
        *error = e.what();
        return false;
    }
}

void apply_translategemma_stop_tokens(TranslateGemmaJson *generation) {
    static const std::vector<std::string> kStopTokens = {
        "<|im_end|>",
        "<end_of_turn>",
        "</start_of_turn>",
        "</end_of_turn>",
    };

    TranslateGemmaJson merged = TranslateGemmaJson::array();
    if (generation->contains("stop")) {
        const auto &stop = generation->at("stop");
        if (stop.is_string()) {
            merged.push_back(stop.get<std::string>());
        } else if (stop.is_array()) {
            for (const auto &entry : stop) {
                merged.push_back(entry);
            }
        }
    }

    for (const auto &token : kStopTokens) {
        bool found = false;
        for (const auto &entry : merged) {
            if (entry.is_string() && entry.get<std::string>() == token) {
                found = true;
                break;
            }
        }
        if (!found) {
            merged.push_back(token);
        }
    }
    (*generation)["stop"] = std::move(merged);
}

} // namespace chatbot
} // namespace lf
