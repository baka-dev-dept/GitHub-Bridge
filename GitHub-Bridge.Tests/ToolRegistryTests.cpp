#include "TestFramework.h"

#include "Core/ToolRegistry.h"

using namespace ghb;

// ToolRegistry 是单例且没有 Clear()。测试不能假定空状态，
// 所以用唯一名字注册，靠相对断言而不是绝对计数。

namespace {

// 注册表拒绝没有处理函数的工具，所以测试桩必须给一个真实的处理函数。
std::string StubHandler(const std::string&) {
    return "{}";
}

ToolRegistration MakeTool(const std::string& name) {
    ToolRegistration reg;
    reg.tool.name = name;
    reg.tool.title = name;
    reg.handler = &StubHandler;
    return reg;
}

ToolRegistration MakeHandlerlessTool(const std::string& name) {
    ToolRegistration reg;
    reg.tool.name = name;
    reg.tool.title = name;
    reg.handler = nullptr;
    return reg;
}

} // namespace

TEST(ToolRegistry, RegisterAndFind) {
    CHECK(ToolRegistry::Get()->Register(MakeTool("test_alpha")));
    const ToolRegistration* found = ToolRegistry::Get()->Find("test_alpha");
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK_EQ(found->tool.name, std::string("test_alpha"));
    }
}

TEST(ToolRegistry, FindMissingReturnsNull) {
    CHECK(ToolRegistry::Get()->Find("test_definitely_not_registered") == nullptr);
}

TEST(ToolRegistry, DuplicateNameRejected) {
    CHECK(ToolRegistry::Get()->Register(MakeTool("test_dup")));
    CHECK(!ToolRegistry::Get()->Register(MakeTool("test_dup")));
}

TEST(ToolRegistry, SizeGrowsByOnePerRegister) {
    const size_t before = ToolRegistry::Get()->Size();
    CHECK(ToolRegistry::Get()->Register(MakeTool("test_size_a")));
    CHECK_EQ(ToolRegistry::Get()->Size(), before + 1);
    CHECK(ToolRegistry::Get()->Register(MakeTool("test_size_b")));
    CHECK_EQ(ToolRegistry::Get()->Size(), before + 2);
}

TEST(ToolRegistry, ListMatchesSize) {
    CHECK_EQ(ToolRegistry::Get()->List().size(), ToolRegistry::Get()->Size());
}

TEST(ToolRegistry, EmptyNameRejected) {
    CHECK(!ToolRegistry::Get()->Register(MakeTool("")));
}

TEST(ToolRegistry, HandlerlessToolRejected) {
    // 没有处理函数的工具注册进来会在调用时崩溃，注册表在入口就挡住。
    CHECK(!ToolRegistry::Get()->Register(MakeHandlerlessTool("test_no_handler")));
    CHECK(ToolRegistry::Get()->Find("test_no_handler") == nullptr);
}
