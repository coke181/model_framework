#include "mf/voice/livekit_token_provider.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <chrono>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>

// 文件职责：生成 LiveKit 可验证的 HS256 JWT。
// 安全边界：API Secret 只在后端内存中使用，函数不会记录或返回 Secret。

namespace mf::voice {
namespace {

std::string base64_url_encode(std::span<const unsigned char> bytes) {
    if (bytes.empty()) return {};
    std::string encoded(((bytes.size() + 2) / 3) * 4, '\0');
    const auto written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        bytes.data(),
        static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(written));
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    for (auto& ch : encoded) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    return encoded;
}

std::string base64_url_encode(std::string_view text) {
    return base64_url_encode(std::span{
        reinterpret_cast<const unsigned char*>(text.data()),
        text.size()});
}

std::string sign_hs256(std::string_view content, std::string_view secret) {
    unsigned int length = EVP_MAX_MD_SIZE;
    std::vector<unsigned char> digest(length);
    if (!HMAC(
            EVP_sha256(),
            secret.data(),
            static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(content.data()),
            content.size(),
            digest.data(),
            &length)) {
        throw std::runtime_error("LiveKit JWT HMAC-SHA256 签名失败");
    }
    digest.resize(length);
    return base64_url_encode(digest);
}

}  // namespace

LiveKitTokenProvider::LiveKitTokenProvider(VoiceOptions options)
    : options_(std::move(options)) {}

bool LiveKitTokenProvider::configured() const {
    return !options_.server_url.empty() && !options_.api_key.empty() && !options_.api_secret.empty();
}

const VoiceOptions& LiveKitTokenProvider::options() const {
    return options_;
}

VoiceCredentials LiveKitTokenProvider::issue(
    const std::string& room_id,
    const std::string& participant_id,
    const std::string& display_name,
    bool can_publish) const {
    if (!configured()) throw std::runtime_error("LiveKit 配置不完整");
    const auto now = std::chrono::system_clock::now();
    const auto expires_at = now + options_.token_ttl;
    const auto issued_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto expires_seconds = std::chrono::duration_cast<std::chrono::seconds>(expires_at.time_since_epoch()).count();

    const auto header = boost::json::serialize(boost::json::object{{"alg", "HS256"}, {"typ", "JWT"}});
    const auto payload = boost::json::serialize(boost::json::object{
        {"iss", options_.api_key},
        {"sub", participant_id},
        {"name", display_name},
        {"iat", issued_seconds},
        {"nbf", issued_seconds - 5},
        {"exp", expires_seconds},
        {"video", boost::json::object{
            {"roomJoin", true},
            {"room", room_id},
            {"canPublish", can_publish},
            {"canSubscribe", true},
            {"canPublishData", true},
        }},
    });
    const auto signing_input = base64_url_encode(header) + '.' + base64_url_encode(payload);
    return VoiceCredentials{
        .server_url = options_.server_url,
        .room_id = room_id,
        .participant_id = participant_id,
        .display_name = display_name,
        .access_token = signing_input + '.' + sign_hs256(signing_input, options_.api_secret),
        .can_publish = can_publish,
        .expires_at = expires_at,
    };
}

}  // namespace mf::voice
