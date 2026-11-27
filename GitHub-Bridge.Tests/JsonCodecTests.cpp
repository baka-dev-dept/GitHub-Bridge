#include "TestFramework.h"

#include "MCP/JsonCodec.h"
#include "Types/RpcMessage.h"

using namespace ghb;

TEST(JsonCodec, DecodesValidRequest) {
    const DecodeResult r = JsonCodec::Decode(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    CHECK(r.ok);
    CHECK_EQ(r.message.method, std::string("tools/list"));
    CHECK(r.message.has_id);
    CHECK_EQ(r.message.id, std::string("1"));
    CHECK(r.message.id_is_number);
}

TEST(JsonCodec, StringIdPreserved) {
    // 字符串 id 必须原样保存，回包时按同类型回写。
    const DecodeResult r = JsonCodec::Decode(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})");
    CHECK(r.ok);
    CHECK_EQ(r.message.id, std::string("abc"));
    CHECK(!r.message.id_is_number);
}

TEST(JsonCodec, NotificationHasNoId) {
    const DecodeResult r = JsonCodec::Decode(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK(r.ok);
    CHECK(!r.message.has_id);
    CHECK(JsonCodec::IsNotification(r.message));
}

TEST(JsonCodec, RejectsMalformedJson) {
    const DecodeResult r = JsonCodec::Decode("{not json");
    CHECK(!r.ok);
    CHECK_EQ(r.error.code, kJsonRpcParseError);
}

TEST(JsonCodec, RejectsMissingJsonrpc) {
    const DecodeResult r = JsonCodec::Decode(R"({"id":1,"method":"ping"})");
    CHECK(!r.ok);
    CHECK_EQ(r.error.code, kJsonRpcInvalidRequest);
}

TEST(JsonCodec, KeepsIdOnVersionMismatch) {
    // 版本不匹配时仍要保留 id，否则客户端无从对应请求与错误。
    const DecodeResult r = JsonCodec::Decode(R"({"jsonrpc":"1.0","id":42,"method":"ping"})");
    CHECK(!r.ok);
    CHECK_EQ(r.error.code, kJsonRpcInvalidRequest);
    CHECK(r.message.has_id);
    CHECK_EQ(r.message.id, std::string("42"));
}

TEST(JsonCodec, EncodeResponseIsSingleLine) {
    RpcMessage req;
    req.has_id = true;
    req.id = "7";
    req.id_is_number = true;
    const std::string out = JsonCodec::EncodeResponse(req, R"({"ok":true})");
    // stdio 分帧约定：正文紧凑单行，结尾恰好一个 '\n'。
    CHECK_EQ(out.back(), '\n');
    CHECK_EQ(out.find('\n'), out.size() - 1);
    CHECK(out.find("\"result\"") != std::string::npos);
}

TEST(JsonCodec, EncodeErrorNullId) {
    RpcError err;
    err.code = kJsonRpcParseError;
    err.message = "parse error";
    const std::string out = JsonCodec::EncodeErrorNullId(err);
    CHECK(out.find("\"id\":null") != std::string::npos);
    CHECK(out.find("-32700") != std::string::npos);
}
