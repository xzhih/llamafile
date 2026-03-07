// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "chatbot.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "common.h"
#include "image.h"
#include "llama.h"
#include "llamafile.h"
#include "sampling.h"
#include "string.h"

namespace lf {
namespace chatbot {

using json = nlohmann::ordered_json;

struct RenderedMessagesRequest {
    std::string prompt;
    bool has_media = false;
};

static std::string trim_whitespace(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool looks_like_remote_url(std::string_view value) {
    return startscasewith(value, "http://") || startscasewith(value, "https://");
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
        throw std::invalid_argument("image_url part is missing a usable url");
    }
    if (startscasewith(value, "data:")) {
        return value;
    }
    if (looks_like_remote_url(value)) {
        throw std::invalid_argument(
            "remote image urls are not supported in --translate-messages-json; use a data URI or local file path");
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

static void normalize_message_content_parts(json &messages) {
    for (auto &message : messages) {
        if (!message.is_object()) {
            throw std::invalid_argument("message entries must be objects");
        }
        if (!message.contains("role") || !message.at("role").is_string()) {
            throw std::invalid_argument("message is missing required field 'role'");
        }
        if (!message.contains("content") || !message.at("content").is_array()) {
            continue;
        }

        auto &content = message["content"];
        for (auto &part : content) {
            if (!part.is_object() || !part.contains("type") || !part.at("type").is_string()) {
                throw std::invalid_argument("message content parts must be objects with a 'type' field");
            }

            const std::string type = part.at("type").get<std::string>();
            if (type == "text" || type == "media_marker") {
                continue;
            }
            if (type == "input_text") {
                if (!part.contains("text")) {
                    throw std::invalid_argument("input_text part is missing required field 'text'");
                }
                part = json{{"type", "text"}, {"text", part.at("text")}};
                continue;
            }
            if (type == "image_url" || type == "input_image") {
                std::string image_ref;
                if (part.contains("image_url")) {
                    const auto &image_url = part.at("image_url");
                    if (image_url.is_string()) {
                        image_ref = image_url.get<std::string>();
                    } else if (image_url.is_object() && image_url.contains("url")) {
                        image_ref = image_url.at("url").get<std::string>();
                    }
                } else if (part.contains("url") && part.at("url").is_string()) {
                    image_ref = part.at("url").get<std::string>();
                }
                part = json{{"type", "media_marker"}, {"text", normalize_image_reference(std::move(image_ref))}};
                continue;
            }

            throw std::invalid_argument("unsupported content part type: " + type);
        }
    }
}

static json parse_messages_json_payload() {
    json payload = json::parse(load_messages_json_source(g_translate_options.messages_json));
    if (payload.is_object()) {
        if (!payload.contains("messages")) {
            throw std::invalid_argument("messages json object must contain a top-level 'messages' array");
        }
        payload = payload.at("messages");
    }
    if (!payload.is_array()) {
        throw std::invalid_argument("messages json must be an array or an object containing 'messages'");
    }

    normalize_message_content_parts(payload);
    return payload;
}

static std::string flatten_message_content(const json &message, bool *has_media) {
    std::string content;

    if (!message.contains("content")) {
        return content;
    }

    const auto &body = message.at("content");
    if (body.is_string()) {
        content = body.get<std::string>();
        return content;
    }

    if (!body.is_array()) {
        throw std::invalid_argument("message content must be either string or array");
    }

    bool last_was_media_marker = false;
    for (const auto &part : body) {
        if (!part.is_object() || !part.contains("type") || !part.at("type").is_string()) {
            throw std::invalid_argument("message content parts must be objects with a 'type' field");
        }

        const std::string type = part.at("type").get<std::string>();
        std::string text = part.contains("text") && part.at("text").is_string()
                               ? part.at("text").get<std::string>()
                               : "";

        if (type == "text") {
            if (!content.empty() && !last_was_media_marker) {
                content += '\n';
            }
            content += text;
            last_was_media_marker = false;
            continue;
        }

        if (type == "media_marker") {
            if (has_media) {
                *has_media = true;
            }
            content += text;
            last_was_media_marker = true;
            continue;
        }

        throw std::invalid_argument("unsupported content part type after normalization: " + type);
    }

    return content;
}

static RenderedMessagesRequest build_messages_json_request() {
    json messages = parse_messages_json_payload();

    RenderedMessagesRequest result;
    for (const auto &message : messages) {
        if (!message.is_object()) {
            throw std::invalid_argument("message entries must be objects");
        }
        if (!message.contains("role") || !message.at("role").is_string()) {
            throw std::invalid_argument("message is missing required field 'role'");
        }

        const std::string role = message.at("role").get<std::string>();
        const std::string content = flatten_message_content(message, &result.has_media);

        result.prompt += "<|im_start|>";
        result.prompt += role;
        result.prompt += "\n";
        result.prompt += content;
        result.prompt += "\n<|im_end|>\n";
    }

    result.prompt += "<|im_start|>assistant\n";
    return result;
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
        if (!eval_token(id))
            break;
        if (llama_vocab_is_eog(vocab, id))
            break;
        output += token_to_piece(g_ctx, id, false);
        if (output.find("<|im_end|>") != std::string::npos)
            break;
    }
    llama_synchronize(g_ctx);
    return sanitize_translategemma_output(output);
}

static std::string wrap_chatml_request(std::string_view user_content) {
    std::string prompt;
    prompt += "<|im_start|>system\n";
    prompt += "You are a translation engine. Return only translated text.\n";
    prompt += "<|im_end|>\n";
    prompt += "<|im_start|>user\n";
    prompt.append(user_content.data(), user_content.size());
    prompt += "\n<|im_end|>\n";
    prompt += "<|im_start|>assistant\n";
    return prompt;
}

static std::string build_image_request(const char *path) {
    std::string image;
    if (!slurp(&image, path)) {
        err("%s: failed to read image", path);
        return "";
    }
    if (!is_image(image)) {
        err("%s: unsupported image file", path);
        return "";
    }
    std::string user = build_translategemma_image_prompt(
        g_translate_options.source_lang,
        g_translate_options.target_lang);
    user += "\n\n";
    convert_image_to_uri(&user, image);
    return wrap_chatml_request(user);
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
        request_has_media = true;
        request = build_image_request(g_translate_options.image_path.c_str());
        if (request.empty())
            return 12;
    } else {
        std::string user = build_translategemma_text_prompt(
            g_translate_options.source_lang,
            g_translate_options.target_lang,
            g_translate_options.text);
        request = wrap_chatml_request(user);
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

    if (!eval_string(request, ADD_SPECIAL, DONT_PARSE_SPECIAL))
        return 13;

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
