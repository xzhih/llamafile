// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "chatbot.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "chat-auto-parser.h"
#include "common.h"
#include "image.h"
#include "llama.h"
#include "llamafile.h"
#include "sampling.h"
#include "string.h"

namespace lf {
namespace chatbot {

using json = nlohmann::ordered_json;

struct PreparedMessagesRequest {
    json messages = json::array();
    std::vector<std::string> image_data_uris;
    bool has_media = false;
};

struct RenderedMessagesRequest {
    std::string prompt;
    bool has_media = false;
};

static bool is_language_code_valid(std::string_view code) {
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

static std::string trim_whitespace(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool looks_like_remote_url(std::string_view value) {
    return startscasewith(value, "http://") || startscasewith(value, "https://");
}

static const common_chat_templates * ensure_translate_chat_templates() {
    if (!g_chat_templates) {
        if (g_params->chat_template.empty() && llama_model_chat_template(g_model, nullptr) == nullptr) {
            throw std::invalid_argument("model does not provide a chat template required by translate mode");
        }
        g_chat_templates = common_chat_templates_init(g_model, g_params->chat_template);
    }
    if (!g_chat_templates) {
        throw std::invalid_argument("failed to initialize chat template");
    }
    return g_chat_templates.get();
}

static std::string load_messages_json_source(std::string_view source) {
    std::string raw(source);
    raw = trim_whitespace(raw);
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

static std::string normalize_image_reference(std::string value) {
    value = trim_whitespace(std::move(value));
    if (value.empty()) {
        throw std::invalid_argument("image content item is missing a usable image reference");
    }
    if (startscasewith(value, "data:")) {
        return value;
    }
    if (looks_like_remote_url(value)) {
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
    return data_uri;
}

static std::string load_image_file_as_data_uri(const char *path) {
    std::string image;
    if (!slurp(&image, path)) {
        throw std::invalid_argument(std::string("failed to read image: ") + path);
    }
    if (!is_image(image)) {
        throw std::invalid_argument(std::string("unsupported image file: ") + path);
    }

    std::string data_uri;
    convert_image_to_uri(&data_uri, image);
    return data_uri;
}

static std::string require_string_field(const json &object, const char *field, const char *context) {
    if (!object.is_object() || !object.contains(field) || !object.at(field).is_string()) {
        throw std::invalid_argument(std::string(context) + " is missing required field '" + field + "'");
    }
    return object.at(field).get<std::string>();
}

static void validate_assistant_message(const json &message) {
    if (!message.contains("content") || !message.at("content").is_string()) {
        throw std::invalid_argument("assistant message content must be a string");
    }
}

static void validate_user_message(json &message, std::vector<std::string> *image_data_uris) {
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
    if (!is_language_code_valid(source_lang)) {
        throw std::invalid_argument("invalid source language code in messages json");
    }
    if (!is_language_code_valid(target_lang)) {
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

    std::string data_uri = normalize_image_reference(
        require_string_field(content, "url", "image content item"));
    content["url"] = data_uri;
    image_data_uris->push_back(std::move(data_uri));
}

static PreparedMessagesRequest parse_messages_json_payload() {
    json payload = json::parse(load_messages_json_source(g_translate_options.messages_json));
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

    PreparedMessagesRequest result;
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
            validate_user_message(message, &result.image_data_uris);
        } else {
            validate_assistant_message(message);
        }

        expect_user = !expect_user;
    }

    result.has_media = !result.image_data_uris.empty();
    return result;
}

static PreparedMessagesRequest build_text_request() {
    PreparedMessagesRequest request;
    request.messages = json::array({
        {
            {"role", "user"},
            {"content", json::array({
                {
                    {"type", "text"},
                    {"source_lang_code", g_translate_options.source_lang},
                    {"target_lang_code", g_translate_options.target_lang},
                    {"text", g_translate_options.text},
                },
            })},
        },
    });
    return request;
}

static PreparedMessagesRequest build_image_request(const char *path) {
    PreparedMessagesRequest request;
    request.has_media = true;
    request.image_data_uris.push_back(load_image_file_as_data_uri(path));
    request.messages = json::array({
        {
            {"role", "user"},
            {"content", json::array({
                {
                    {"type", "image"},
                    {"source_lang_code", g_translate_options.source_lang},
                    {"target_lang_code", g_translate_options.target_lang},
                    {"url", request.image_data_uris.back()},
                },
            })},
        },
    });
    return request;
}

static std::string replace_rendered_image_placeholders(
    std::string prompt, const std::vector<std::string> &image_data_uris) {
    size_t search_from = 0;
    for (const auto &image_data_uri : image_data_uris) {
        const size_t pos = prompt.find("<start_of_image>", search_from);
        if (pos == std::string::npos) {
            throw std::invalid_argument("rendered prompt is missing <start_of_image> placeholder for image content");
        }
        prompt.replace(pos, strlen("<start_of_image>"), image_data_uri);
        search_from = pos + image_data_uri.size();
    }
    if (prompt.find("<start_of_image>", search_from) != std::string::npos) {
        throw std::invalid_argument("rendered prompt contains unexpected extra <start_of_image> placeholders");
    }
    return prompt;
}

static std::string inject_translation_instruction_into_prompt(
    std::string prompt, std::string_view translation_instruction) {
    const std::string instruction = trim_whitespace(std::string(translation_instruction));
    if (instruction.empty()) {
        return prompt;
    }

    static const char *const kAnchors[] = {
        "\nProduce only the ",
        "\nPlease translate the ",
    };

    size_t insert_pos = std::string::npos;
    for (const char *anchor : kAnchors) {
        const size_t pos = prompt.find(anchor);
        if (pos != std::string::npos && (insert_pos == std::string::npos || pos < insert_pos)) {
            insert_pos = pos + 1;
        }
    }
    if (insert_pos == std::string::npos) {
        throw std::invalid_argument(
            "failed to locate translation instruction insertion point in rendered prompt");
    }

    prompt.insert(insert_pos, "Translation preference: " + instruction + "\n");
    return prompt;
}

static std::string render_translategemma_messages_prompt(
    const PreparedMessagesRequest &request, std::string_view translation_instruction) {
    const common_chat_templates *tmpls = ensure_translate_chat_templates();
    std::string template_source = common_chat_templates_source(tmpls);
    if (template_source.empty()) {
        throw std::invalid_argument("chat template source is empty");
    }

    const llama_vocab *vocab = llama_model_get_vocab(g_model);
    const llama_token bos_id = vocab ? llama_vocab_bos(vocab) : LLAMA_TOKEN_NULL;
    const llama_token eos_id = vocab ? llama_vocab_eos(vocab) : LLAMA_TOKEN_NULL;
    const std::string bos_token = bos_id == LLAMA_TOKEN_NULL ? std::string() : common_token_to_piece(vocab, bos_id, true);
    const std::string eos_token = eos_id == LLAMA_TOKEN_NULL ? std::string() : common_token_to_piece(vocab, eos_id, true);

    common_chat_template chat_template(template_source, bos_token, eos_token);

    autoparser::templates_params params;
    params.messages = request.messages;
    params.add_generation_prompt = true;
    params.add_bos = vocab && llama_vocab_get_add_bos(vocab);
    params.add_eos = vocab && llama_vocab_get_add_eos(vocab);

    std::string prompt = common_chat_template_direct_apply(
        chat_template, params);
    prompt = inject_translation_instruction_into_prompt(
        std::move(prompt), translation_instruction);
    if (!request.image_data_uris.empty()) {
        prompt = replace_rendered_image_placeholders(std::move(prompt), request.image_data_uris);
    }
    return prompt;
}

static RenderedMessagesRequest build_messages_json_request() {
    PreparedMessagesRequest request = parse_messages_json_payload();
    RenderedMessagesRequest rendered;
    rendered.has_media = request.has_media;
    rendered.prompt = render_translategemma_messages_prompt(request, {});
    return rendered;
}

static std::string generate_translation() {
    std::string output;
    const llama_vocab *vocab = llama_model_get_vocab(g_model);
    for (;;) {
        if (g_got_sigint) {
            g_got_sigint = false;
            break;
        }
        llama_token id = common_sampler_sample(g_sampler, g_ctx, -1);
        common_sampler_accept(g_sampler, id, true);
        if (!eval_token(id)) {
            break;
        }
        if (llama_vocab_is_eog(vocab, id)) {
            break;
        }
        output += token_to_piece(g_ctx, id, false);
        if (output.find("<|im_end|>") != std::string::npos ||
            output.find("<end_of_turn>") != std::string::npos ||
            output.find("</start_of_turn>") != std::string::npos ||
            output.find("</end_of_turn>") != std::string::npos) {
            break;
        }
    }
    llama_synchronize(g_ctx);
    return sanitize_translategemma_output(output);
}

int run_translate_mode() {
    FLAG_nologo = true;
    g_params->prompt.clear();

    std::string request;
    bool request_has_media = false;

    if (!g_translate_options.messages_json.empty()) {
        try {
            auto rendered = build_messages_json_request();
            if (rendered.has_media && !g_mtmd) {
                err("multimodal model not loaded (use --mmproj to specify vision model)");
                return 11;
            }
            request_has_media = rendered.has_media;
            request = std::move(rendered.prompt);
        } catch (const std::exception &e) {
            err("%s", e.what());
            return 12;
        }
    } else if (g_translate_options.image_mode) {
        if (!g_mtmd) {
            err("multimodal model not loaded (use --mmproj to specify vision model)");
            return 11;
        }
        try {
            PreparedMessagesRequest image_request = build_image_request(g_translate_options.image_path.c_str());
            request_has_media = true;
            request = render_translategemma_messages_prompt(
                image_request, g_translate_options.translation_instruction);
        } catch (const std::exception &e) {
            err("%s", e.what());
            return 12;
        }
    } else {
        try {
            PreparedMessagesRequest text_request = build_text_request();
            request = render_translategemma_messages_prompt(
                text_request, g_translate_options.translation_instruction);
        } catch (const std::exception &e) {
            err("%s", e.what());
            return 12;
        }
    }

    // Work around an aarch64 IQK kernel assert hit by some multimodal requests.
    if (request_has_media) {
        setenv("LLAMAFILE_DISABLE_SGEMM", "1", 1);
        setenv("LLAMAFILE_DISABLE_IQK_MIXMUL", "1", 1);
    }

    if (request.empty()) {
        err("translation prompt is empty");
        return 13;
    }

    if (!eval_string(request, ADD_SPECIAL, DONT_PARSE_SPECIAL)) {
        return 13;
    }

    std::string output = generate_translation();
    if (output.empty()) {
        err("translation output is empty");
        return 14;
    }

    std::fputs(output.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

} // namespace chatbot
} // namespace lf
