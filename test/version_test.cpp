#include <kv/version.hpp>

#include <userver/utest/utest.hpp>

UTEST(KvVersion, MatchesConstant)
{
    EXPECT_EQ(kv::version(), kv::kVersion);
    EXPECT_FALSE(kv::version().empty());
}
