#include "TestFramework.h"

#include "GitHub/GraphQLGuard.h"

using namespace ghb;

namespace {

// 把 query 包成工具参数，跟真实调用形态一致。
std::string Args(const std::string& query) {
    return R"({"query":")" + query + R"("})";
}

std::string Resolve(const std::string& query) {
    return ResolveGraphQLPermissionKey(Args(query));
}

} // namespace

TEST(GraphQLGuard, PlainMutationIsWrite) {
    CHECK_EQ(Resolve("mutation { createRepository(input:{name:\\\"x\\\"}) { repository { id } } }"), std::string("write"));
}

TEST(GraphQLGuard, NamedMutationIsWrite) {
    CHECK_EQ(Resolve("mutation CreateRepo { createRepository(input:{name:\\\"x\\\"}) { repository { id } } }"), std::string("write"));
}

TEST(GraphQLGuard, ExplicitQueryIsRead) {
    CHECK_EQ(Resolve("query { viewer { login } }"), std::string("read"));
}

TEST(GraphQLGuard, ShorthandQueryIsRead) {
    CHECK_EQ(Resolve("{ viewer { login } }"), std::string("read"));
}

TEST(GraphQLGuard, MutationInDoubleQuotedStringIsRead) {
    CHECK_EQ(Resolve("query { search(query: \\\"mutation\\\", type: ISSUE, first: 1) { issueCount } }"), std::string("read"));
}

TEST(GraphQLGuard, MutationInCommentIsRead) {
    CHECK_EQ(Resolve("query { # mutation in a comment\\n viewer { login } }"), std::string("read"));
}

TEST(GraphQLGuard, LambdaPrefixedIdentifierIsRead) {
    CHECK_EQ(Resolve("query { myMutation { id } }"), std::string("read"));
    CHECK_EQ(Resolve("query { mutationCount }"), std::string("read"));
}

TEST(GraphQLGuard, MissingQueryReturnsEmpty) {
    // 缺 query 交给工具实现报参数错误，这里不另造错误。
    CHECK_EQ(ResolveGraphQLPermissionKey(R"({})"), std::string(""));
    CHECK_EQ(ResolveGraphQLPermissionKey(R"({"query":123})"), std::string(""));
    CHECK_EQ(ResolveGraphQLPermissionKey("not json"), std::string(""));
}

TEST(GraphQLGuard, StripNoiseRemovesStrings) {
    const std::string stripped = StripGraphQLNoise("query { a(s: \"mutation\") }");
    CHECK(stripped.find("mutation") == std::string::npos);
    CHECK(stripped.find("query") != std::string::npos);
}

TEST(GraphQLGuard, StripNoiseRemovesBlockStrings) {
    const std::string stripped = StripGraphQLNoise("query { a(s: \"\"\"mutation\"\"\") }");
    CHECK(stripped.find("mutation") == std::string::npos);
}

TEST(GraphQLGuard, StripNoiseRemovesComments) {
    const std::string stripped = StripGraphQLNoise("query { # mutation\n a }");
    CHECK(stripped.find("mutation") == std::string::npos);
    CHECK(stripped.find("a") != std::string::npos);
}

TEST(GraphQLGuard, StripNoiseHandlesEscapedQuotes) {
    const std::string stripped = StripGraphQLNoise("query { a(s: \"say \\\"mutation\\\" now\") }");
    CHECK(stripped.find("mutation") == std::string::npos);
}

TEST(GraphQLGuard, StripNoiseKeepsStructure) {
    const std::string stripped = StripGraphQLNoise("mutation { create }");
    CHECK(stripped.find("mutation") != std::string::npos);
}
