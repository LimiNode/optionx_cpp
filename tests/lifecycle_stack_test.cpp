#include <optionx_cpp/lifecycle.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using optionx::lifecycle::ILifecycleModule;
using optionx::lifecycle::LifecycleStack;

class RecordingModule final : public ILifecycleModule {
public:
    RecordingModule(
            std::string name,
            std::size_t shutdown_process_count,
            std::vector<std::string>& events)
            : m_name(std::move(name)),
              m_shutdown_process_count(shutdown_process_count),
              m_events(events) {}

    void process() override {
        m_events.push_back("process:" + m_name);
        if (!m_shutdown_requested || m_shutdown_process_count == 0) return;
        --m_shutdown_process_count;
        if (m_shutdown_process_count == 0) m_stopped = true;
    }

    void shutdown() noexcept override {
        m_events.push_back("shutdown:" + m_name);
        m_shutdown_requested = true;
        if (m_shutdown_process_count == 0) m_stopped = true;
    }

    [[nodiscard]] bool is_stopped() const noexcept override {
        return m_stopped;
    }

private:
    std::string m_name;
    std::size_t m_shutdown_process_count = 0;
    std::vector<std::string>& m_events;
    bool m_shutdown_requested = false;
    bool m_stopped = false;
};

TEST(LifecycleStack, ProcessesForwardAndShutsDownDependentsOneAtATime) {
    std::vector<std::string> events;
    RecordingModule platform("platform", 0, events);
    RecordingModule router("router", 1, events);
    RecordingModule bot("bot", 1, events);
    LifecycleStack stack;

    ASSERT_TRUE(stack.add_module(platform));
    ASSERT_TRUE(stack.add_module(router));
    ASSERT_TRUE(stack.add_module(bot));
    EXPECT_FALSE(stack.add_module(router));
    EXPECT_FALSE(stack.add_module(stack));

    stack.process();
    EXPECT_EQ(events, (std::vector<std::string>{
        "process:platform",
        "process:router",
        "process:bot"}));
    events.clear();

    stack.shutdown();
    EXPECT_TRUE(stack.is_shutdown_requested());
    EXPECT_FALSE(stack.is_stopped());
    EXPECT_EQ(events, (std::vector<std::string>{"shutdown:bot"}));
    events.clear();

    stack.process();
    EXPECT_FALSE(stack.is_stopped());
    EXPECT_EQ(events, (std::vector<std::string>{
        "process:platform",
        "process:router",
        "process:bot",
        "shutdown:router"}));
    events.clear();

    stack.process();
    EXPECT_TRUE(stack.is_stopped());
    EXPECT_EQ(events, (std::vector<std::string>{
        "process:platform",
        "process:router",
        "shutdown:platform"}));

    events.clear();
    stack.process();
    stack.shutdown();
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(stack.add_module(bot));
}

TEST(LifecycleStack, StopsSynchronousModulesInReverseOrder) {
    std::vector<std::string> events;
    RecordingModule first("first", 0, events);
    RecordingModule second("second", 0, events);
    LifecycleStack stack;

    ASSERT_TRUE(stack.add_module(first));
    ASSERT_TRUE(stack.add_module(second));
    stack.shutdown();

    EXPECT_TRUE(stack.is_stopped());
    EXPECT_EQ(events, (std::vector<std::string>{
        "shutdown:second",
        "shutdown:first"}));
}

TEST(LifecycleStack, EmptyStackStopsOnRequest) {
    LifecycleStack stack;

    EXPECT_TRUE(stack.empty());
    EXPECT_EQ(stack.size(), 0u);
    EXPECT_FALSE(stack.is_stopped());

    stack.shutdown();

    EXPECT_TRUE(stack.is_shutdown_requested());
    EXPECT_TRUE(stack.is_stopped());
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
