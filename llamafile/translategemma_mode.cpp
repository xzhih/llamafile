// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "chatbot.h"

#include <cstdio>

#include "common.h"
#include "image.h"
#include "llama.h"
#include "llamafile.h"
#include "sampling.h"
#include "string.h"

namespace lf {
namespace chatbot {

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
    if (g_translate_options.image_mode) {
        if (!g_mtmd) {
            err("multimodal model not loaded (use --mmproj to specify vision model)");
            return 11;
        }
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
