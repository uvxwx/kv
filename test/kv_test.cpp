#include <kv/kv.hpp>

#include <optional>
#include <string>

#include <userver/engine/async.hpp>
#include <userver/engine/single_use_event.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/utest/utest.hpp>

namespace {

namespace us = userver;
namespace eng = us::engine;

UTEST_MT(Kv, YoungerWriterAbortsAgainstOlderReader, 2)
{
    kv::Store store(8);
    auto olderReader = std::make_unique<kv::Tx>(store);

    auto readerResult = olderReader->find("k");
    ASSERT_TRUE(readerResult);

    auto writer = eng::AsyncNoSpan([&]() {
        auto youngerWriter = kv::Tx(store);
        return youngerWriter.insertOrAssign("k", "writer");
    });

    auto writerResult = writer.Get();
    ASSERT_FALSE(writerResult);
    EXPECT_EQ(writerResult.error(), kv::Tx::OpError::kAborted);
}

UTEST_MT(Kv, QueuedWriterDiesAfterReaderPromotion, 1)
{
    kv::Store store(8);
    auto olderReader = std::make_unique<kv::Tx>(store);
    auto olderWriter = std::make_unique<kv::Tx>(store);
    auto youngerWriter = std::make_unique<kv::Tx>(store);

    ASSERT_TRUE(youngerWriter->insertOrAssign("k", "younger-writer"));

    auto olderReaderTask = eng::AsyncNoSpan([&]() { return olderReader->find("k"); });

    auto olderWriterTask = eng::AsyncNoSpan([&]() {
        return olderWriter->insertOrAssign("k", "older-writer");
    });

    eng::Yield();
    eng::Yield();
    youngerWriter.reset();

    auto olderWriterResult = olderWriterTask.Get();
    ASSERT_FALSE(olderWriterResult);
    EXPECT_EQ(olderWriterResult.error(), kv::Tx::OpError::kAborted);

    auto olderReaderResult = olderReaderTask.Get();
    ASSERT_TRUE(olderReaderResult);
    EXPECT_FALSE(olderReaderResult->has_value());
}

UTEST(Kv, RunInTxPersistsWritesAndErases)
{
    kv::Store store(8);

    auto writeResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
        EXPECT_TRUE(tx.insertOrAssign("kept", "v1"));

        auto written = tx.find("kept");
        EXPECT_TRUE(written);
        EXPECT_TRUE(written && written->has_value());
        if (written && written->has_value())
            EXPECT_EQ(**written, "v1");

        EXPECT_TRUE(tx.insertOrAssign("gone", "tmp"));
        EXPECT_TRUE(tx.erase("gone"));

        auto erased = tx.find("gone");
        EXPECT_TRUE(erased);
        EXPECT_TRUE(erased && !erased->has_value());

        EXPECT_TRUE(tx.insertOrAssign("kept", "v2"));
        return 1;
    });

    ASSERT_TRUE(writeResult);
    EXPECT_EQ(*writeResult, 1);

    auto readResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<std::optional<std::string>> {
        return tx.find("kept");
    });
    ASSERT_TRUE(readResult);
    ASSERT_TRUE(readResult->has_value());
    EXPECT_EQ(**readResult, "v2");

    auto missingResult = store.runInTx(
        [](kv::Tx &tx) -> kv::Tx::Result<std::optional<std::string>> { return tx.find("gone"); }
    );
    ASSERT_TRUE(missingResult);
    EXPECT_FALSE(missingResult->has_value());
}

UTEST(Kv, SharedToExclusiveUpgradeAndMissingEraseFail)
{
    using enum kv::Tx::OpError;

    kv::Store store(8);
    auto tx = kv::Tx(store);

    auto readResult = tx.find("shared");
    ASSERT_TRUE(readResult);
    EXPECT_FALSE(readResult->has_value());

    auto writeResult = tx.insertOrAssign("shared", "value");
    ASSERT_FALSE(writeResult);
    EXPECT_EQ(writeResult.error(), kSharedToExclusiveLockUpgrade);

    auto eraseUpgradeResult = tx.erase("shared");
    ASSERT_FALSE(eraseUpgradeResult);
    EXPECT_EQ(eraseUpgradeResult.error(), kSharedToExclusiveLockUpgrade);

    auto missingEraseTx = kv::Tx(store);
    auto missingEraseResult = missingEraseTx.erase("missing");
    ASSERT_FALSE(missingEraseResult);
    EXPECT_EQ(missingEraseResult.error(), kMissing);
}

UTEST(Kv, AbortedTxRejectsLaterOps)
{
    using enum kv::Tx::OpError;

    kv::Store store(8);
    auto olderReader = std::make_unique<kv::Tx>(store);
    auto youngerWriter = kv::Tx(store);

    auto readerResult = olderReader->find("k");
    ASSERT_TRUE(readerResult);

    auto firstWriteResult = youngerWriter.insertOrAssign("k", "writer");
    ASSERT_FALSE(firstWriteResult);
    EXPECT_EQ(firstWriteResult.error(), kAborted);

    auto secondWriteResult = youngerWriter.insertOrAssign("other", "writer");
    ASSERT_FALSE(secondWriteResult);
    EXPECT_EQ(secondWriteResult.error(), kAborted);

    auto readAfterAbort = youngerWriter.find("other");
    ASSERT_FALSE(readAfterAbort);
    EXPECT_EQ(readAfterAbort.error(), kAborted);

    auto eraseAfterAbort = youngerWriter.erase("other");
    ASSERT_FALSE(eraseAfterAbort);
    EXPECT_EQ(eraseAfterAbort.error(), kAborted);
}

} // namespace
