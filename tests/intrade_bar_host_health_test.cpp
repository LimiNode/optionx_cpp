#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include <optionx_cpp/platforms.hpp>

using namespace optionx::platforms::intrade_bar;

TEST(IntradeBarHostHealth, DisconnectsOnlyAfterTwoConsecutiveFailures) {
    std::uint32_t failures = 0;

    EXPECT_FALSE(detail::update_host_check_failure_streak(false, failures));
    EXPECT_EQ(failures, 1U);
    EXPECT_TRUE(detail::update_host_check_failure_streak(false, failures));
    EXPECT_EQ(failures, 2U);
}

TEST(IntradeBarHostHealth, SuccessfulCheckResetsFailureStreak) {
    std::uint32_t failures = 1;

    EXPECT_FALSE(detail::update_host_check_failure_streak(true, failures));
    EXPECT_EQ(failures, 0U);
    EXPECT_FALSE(detail::update_host_check_failure_streak(false, failures));
    EXPECT_EQ(failures, 1U);
}

TEST(IntradeBarHostHealth, FailureStreakSaturates) {
    std::uint32_t failures = std::numeric_limits<std::uint32_t>::max();

    EXPECT_TRUE(detail::update_host_check_failure_streak(false, failures));
    EXPECT_EQ(failures, std::numeric_limits<std::uint32_t>::max());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
