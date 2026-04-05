#include <kv/kv.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <userver/engine/async.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/utest/utest.hpp>

namespace {

namespace us = userver;
namespace eng = us::engine;

constexpr std::chrono::seconds kTaskWaitTimeout{1};

template <typename T>
[[nodiscard]] std::optional<T> getTaskResultNoThrow(eng::TaskWithResult<T> &task) noexcept
{
    try {
        return task.Get();
    } catch (const eng::SemaphoreLockCancelledError &) {
        return {};
    } catch (const eng::TaskCancelledException &) {
        return {};
    } catch (const eng::WaitInterruptedException &) {
        return {};
    }
}

template <typename T> [[nodiscard]] bool waitUntilReady(eng::TaskWithResult<T> &task) noexcept
{
    return task.WaitNothrowUntil(eng::Deadline::FromDuration(kTaskWaitTimeout)) ==
           eng::FutureStatus::kReady;
}

[[nodiscard]] bool waitUntilReady(eng::SingleUseEvent &event) noexcept
{
    return event.WaitUntil(eng::Deadline::FromDuration(kTaskWaitTimeout)) ==
           eng::FutureStatus::kReady;
}

class ConcurrentStart final {
public:
    explicit ConcurrentStart(size_t workerCount) noexcept : remaining(workerCount) {}

    void arriveAndWait() noexcept
    {
        if (remaining.fetch_sub(1) == 1)
            ready.Send();
        while (!released.load())
            eng::Yield();
    }

    [[nodiscard]] bool waitUntilReady() noexcept { return ::waitUntilReady(ready); }

    void release() noexcept { released.store(true); }

private:
    std::atomic<size_t> remaining;
    std::atomic<bool> released{false};
    eng::SingleUseEvent ready;
};

UTEST_MT(Kv, YoungerWriterAbortsAgainstOlderReader, 2)
{
    auto store = kv::Store(8);
    auto olderReader = std::make_unique<kv::Tx>(store);

    ASSERT_TRUE(olderReader->find("k"));

    auto writer = eng::AsyncNoSpan([&]() {
        auto youngerWriter = kv::Tx(store);
        return youngerWriter.insertOrAssign("k", "writer");
    });

    auto writerResult = writer.Get();
    ASSERT_FALSE(writerResult);
    ASSERT_EQ(writerResult.error(), kv::Tx::OpError::kAborted);
}

UTEST_MT(Kv, QueuedWriterDiesAfterReaderPromotion, 1)
{
    auto store = kv::Store(8);
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
    auto olderReaderResult = olderReaderTask.Get();

    ASSERT_FALSE(olderWriterResult);
    ASSERT_EQ(olderWriterResult.error(), kv::Tx::OpError::kAborted);
    ASSERT_TRUE(olderReaderResult);
    ASSERT_FALSE(olderReaderResult->has_value());
}

UTEST_MT(Kv, RunInTxWithRetryCommitsAtomically, 4)
{
    constexpr size_t kWorkerCount = 4;
    constexpr size_t kIterationsPerWorker = 8;
    constexpr size_t kMaxAttempts = 32;
    constexpr std::chrono::milliseconds kRetryInterval{1};
    constexpr std::chrono::milliseconds kOverlapSleep{1};

    auto store = kv::Store(8);
    ConcurrentStart start{kWorkerCount};

    auto seedResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
        auto leftResult = tx.insertOrAssign("pair-left", "seed");
        if (!leftResult)
            return std::unexpected{leftResult.error()};

        auto rightResult = tx.insertOrAssign("pair-right", "seed");
        if (!rightResult)
            return std::unexpected{rightResult.error()};

        return 1;
    });
    ASSERT_TRUE(seedResult);

    using WorkerResult = kv::Store::Result<int, kv::Tx::OpError>;
    std::vector<eng::TaskWithResult<WorkerResult>> workers;
    workers.reserve(kWorkerCount);

    for (size_t workerId = 0; workerId < kWorkerCount; workerId++) {
        workers.push_back(eng::AsyncNoSpan([&, workerId]() -> WorkerResult {
            start.arriveAndWait();

            for (size_t iteration = 0; iteration < kIterationsPerWorker; iteration++) {
                std::string token = std::format("worker-{}-{}", workerId, iteration);

                auto writeResult = store.runInTxWithRetry(
                    [&](kv::Tx &tx) -> kv::Tx::Result<int> {
                        auto leftResult = tx.insertOrAssign("pair-left", token);
                        if (!leftResult)
                            return std::unexpected{leftResult.error()};

                        eng::SleepFor(kOverlapSleep);

                        auto rightResult = tx.insertOrAssign("pair-right", token);
                        if (!rightResult)
                            return std::unexpected{rightResult.error()};

                        return 1;
                    },
                    kMaxAttempts, kRetryInterval
                );
                if (!writeResult)
                    return std::unexpected{writeResult.error()};

                auto verifyResult = store.runInTxWithRetry(
                    [](kv::Tx &tx) -> kv::Tx::Result<int> {
                        auto leftResult = tx.find("pair-left");
                        if (!leftResult || !leftResult->has_value())
                            return std::unexpected{kv::Tx::OpError::kAborted};

                        auto rightResult = tx.find("pair-right");
                        if (!rightResult || !rightResult->has_value())
                            return std::unexpected{kv::Tx::OpError::kAborted};

                        if (**leftResult != **rightResult)
                            return std::unexpected{kv::Tx::OpError::kAborted};

                        return 1;
                    },
                    kMaxAttempts, kRetryInterval
                );
                if (!verifyResult)
                    return std::unexpected{verifyResult.error()};
            }

            return 1;
        }));
    }

    if (!start.waitUntilReady()) {
        start.release();
        FAIL();
    }
    start.release();

    for (auto &worker : workers) {
        ASSERT_TRUE(waitUntilReady(worker));
        auto workerResult = getTaskResultNoThrow(worker);
        ASSERT_TRUE(workerResult);
        ASSERT_TRUE(*workerResult);
        ASSERT_EQ(**workerResult, 1);
    }

    auto finalResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<std::string> {
        auto leftResult = tx.find("pair-left");
        if (!leftResult || !leftResult->has_value())
            return std::unexpected{kv::Tx::OpError::kAborted};

        auto rightResult = tx.find("pair-right");
        if (!rightResult || !rightResult->has_value())
            return std::unexpected{kv::Tx::OpError::kAborted};

        if (**leftResult != **rightResult)
            return std::unexpected{kv::Tx::OpError::kAborted};

        return **leftResult;
    });
    ASSERT_TRUE(finalResult);
    ASSERT_FALSE(finalResult->empty());
}

UTEST_MT(Kv, RunInTxAbortsContendingWriters, 4)
{
    constexpr size_t kWriterCount = 3;

    auto store = kv::Store(8);

    auto seedResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
        auto writeResult = tx.insertOrAssign("hot", "seed");
        if (!writeResult)
            return std::unexpected{writeResult.error()};
        return 1;
    });
    ASSERT_TRUE(seedResult);

    eng::SingleUseEvent readerReady;
    eng::SingleUseEvent releaseReader;
    ConcurrentStart writersStart{kWriterCount};

    auto reader = eng::AsyncNoSpan([&]() -> kv::Store::Result<int, kv::Tx::OpError> {
        return store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
            auto readResult = tx.find("hot");
            if (!readResult || !readResult->has_value())
                return std::unexpected{kv::Tx::OpError::kAborted};

            readerReady.Send();
            releaseReader.WaitNonCancellable();
            return 1;
        });
    });
    ASSERT_TRUE(waitUntilReady(readerReady));

    using WriterResult = kv::Store::Result<int, kv::Tx::OpError>;
    std::vector<eng::TaskWithResult<WriterResult>> writers;
    writers.reserve(kWriterCount);

    for (size_t writerId = 0; writerId < kWriterCount; writerId++) {
        writers.push_back(eng::AsyncNoSpan([&, writerId]() -> WriterResult {
            writersStart.arriveAndWait();

            return store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
                auto writeResult = tx.insertOrAssign("hot", std::format("writer-{}", writerId));
                if (!writeResult)
                    return std::unexpected{writeResult.error()};
                return 1;
            });
        }));
    }

    if (!writersStart.waitUntilReady()) {
        writersStart.release();
        releaseReader.Send();
        FAIL();
    }
    writersStart.release();

    for (auto &writer : writers) {
        ASSERT_TRUE(waitUntilReady(writer));
        auto writerResult = getTaskResultNoThrow(writer);
        ASSERT_TRUE(writerResult);
        ASSERT_FALSE(*writerResult);
        ASSERT_TRUE(std::holds_alternative<kv::Store::Aborted>(writerResult->error()));
        ASSERT_TRUE(std::get<kv::Store::Aborted>(writerResult->error()).retriable);
    }

    releaseReader.Send();
    ASSERT_TRUE(waitUntilReady(reader));
    auto readerResult = getTaskResultNoThrow(reader);
    ASSERT_TRUE(readerResult);
    ASSERT_TRUE(*readerResult);
    ASSERT_EQ(**readerResult, 1);

    auto finalResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<std::optional<std::string>> {
        return tx.find("hot");
    });
    ASSERT_TRUE(finalResult);
    ASSERT_TRUE(finalResult->has_value());
    ASSERT_EQ(**finalResult, "seed");
}

UTEST(Kv, RunInTxPersistsWritesAndErases)
{
    auto store = kv::Store(8);

    auto writeResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
        if (!tx.insertOrAssign("kept", "v1"))
            return std::unexpected{kv::Tx::OpError::kAborted};

        auto written = tx.find("kept");
        if (!written || !written->has_value() || **written != "v1")
            return std::unexpected{kv::Tx::OpError::kAborted};

        if (!tx.insertOrAssign("gone", "tmp") || !tx.erase("gone"))
            return std::unexpected{kv::Tx::OpError::kAborted};

        auto erased = tx.find("gone");
        if (!erased || erased->has_value() || !tx.insertOrAssign("kept", "v2"))
            return std::unexpected{kv::Tx::OpError::kAborted};

        return 1;
    });
    ASSERT_TRUE(writeResult);
    ASSERT_EQ(*writeResult, 1);

    auto readResult = store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<std::optional<std::string>> {
        return tx.find("kept");
    });
    ASSERT_TRUE(readResult);
    ASSERT_TRUE(readResult->has_value());
    ASSERT_EQ(**readResult, "v2");

    auto missingResult = store.runInTx(
        [](kv::Tx &tx) -> kv::Tx::Result<std::optional<std::string>> { return tx.find("gone"); }
    );
    ASSERT_TRUE(missingResult);
    ASSERT_FALSE(missingResult->has_value());
}

UTEST(Kv, SharedToExclusiveUpgradeAndMissingEraseFail)
{
    using enum kv::Tx::OpError;

    auto store = kv::Store(8);
    auto tx = kv::Tx(store);

    auto readResult = tx.find("shared");
    ASSERT_TRUE(readResult);
    ASSERT_FALSE(readResult->has_value());

    auto writeResult = tx.insertOrAssign("shared", "value");
    ASSERT_FALSE(writeResult);
    ASSERT_EQ(writeResult.error(), kSharedToExclusiveLockUpgrade);

    auto eraseUpgradeResult = tx.erase("shared");
    ASSERT_FALSE(eraseUpgradeResult);
    ASSERT_EQ(eraseUpgradeResult.error(), kSharedToExclusiveLockUpgrade);

    auto missingEraseTx = kv::Tx(store);
    auto missingEraseResult = missingEraseTx.erase("missing");
    ASSERT_FALSE(missingEraseResult);
    ASSERT_EQ(missingEraseResult.error(), kMissing);
}

UTEST(Kv, AbortedTxRejectsLaterOps)
{
    using enum kv::Tx::OpError;

    auto store = kv::Store(8);
    auto olderReader = std::make_unique<kv::Tx>(store);
    auto youngerWriter = kv::Tx(store);

    ASSERT_TRUE(olderReader->find("k"));

    auto firstWriteResult = youngerWriter.insertOrAssign("k", "writer");
    ASSERT_FALSE(firstWriteResult);
    ASSERT_EQ(firstWriteResult.error(), kAborted);

    auto secondWriteResult = youngerWriter.insertOrAssign("other", "writer");
    ASSERT_FALSE(secondWriteResult);
    ASSERT_EQ(secondWriteResult.error(), kAborted);

    auto readAfterAbort = youngerWriter.find("other");
    ASSERT_FALSE(readAfterAbort);
    ASSERT_EQ(readAfterAbort.error(), kAborted);

    auto eraseAfterAbort = youngerWriter.erase("other");
    ASSERT_FALSE(eraseAfterAbort);
    ASSERT_EQ(eraseAfterAbort.error(), kAborted);
}

UTEST(Kv, RunInTxCancellationReturnsAbort)
{
    auto store = kv::Store(1);
    eng::SingleUseEvent entered;
    eng::SingleUseEvent releaseHolder;

    auto holder = eng::AsyncNoSpan([&]() -> kv::Store::Result<int, kv::Tx::OpError> {
        return store.runInTx([&](kv::Tx &) -> kv::Tx::Result<int> {
            entered.Send();
            releaseHolder.WaitNonCancellable();
            return 1;
        });
    });
    ASSERT_TRUE(waitUntilReady(entered));

    auto waiter = eng::AsyncNoSpan([&]() -> kv::Store::Result<int, kv::Tx::OpError> {
        return store.runInTx([](kv::Tx &) -> kv::Tx::Result<int> { return 2; });
    });

    eng::Yield();
    eng::Yield();
    waiter.RequestCancel();

    if (!waitUntilReady(waiter)) {
        releaseHolder.Send();
        FAIL();
    }

    auto waiterResult = getTaskResultNoThrow(waiter);

    releaseHolder.Send();
    ASSERT_TRUE(waitUntilReady(holder));
    auto holderResult = getTaskResultNoThrow(holder);

    ASSERT_TRUE(waiterResult);
    ASSERT_FALSE(*waiterResult);
    ASSERT_TRUE(std::holds_alternative<kv::Store::Aborted>(waiterResult->error()));
    ASSERT_FALSE(std::get<kv::Store::Aborted>(waiterResult->error()).retriable);
    ASSERT_TRUE(holderResult);
    ASSERT_TRUE(*holderResult);
    ASSERT_EQ(**holderResult, 1);
}

UTEST(Kv, CancelledTxCleansUpAndRejectsLaterOps)
{
    using enum kv::Tx::OpError;

    auto store = kv::Store(8);
    auto olderReader = std::make_unique<kv::Tx>(store);
    auto cancelledWriter = std::make_unique<kv::Tx>(store);

    ASSERT_TRUE(olderReader->find("k"));

    auto writerTask = eng::AsyncNoSpan([&]() {
        return cancelledWriter->insertOrAssign("k", "writer");
    });

    eng::Yield();
    eng::Yield();
    writerTask.RequestCancel();

    ASSERT_TRUE(waitUntilReady(writerTask));
    auto writeResult = getTaskResultNoThrow(writerTask);
    ASSERT_TRUE(writeResult);
    ASSERT_FALSE(*writeResult);
    ASSERT_EQ(writeResult->error(), kAborted);

    auto secondWriteResult = cancelledWriter->insertOrAssign("other", "writer");
    ASSERT_FALSE(secondWriteResult);
    ASSERT_EQ(secondWriteResult.error(), kAborted);

    auto readAfterAbort = cancelledWriter->find("other");
    ASSERT_FALSE(readAfterAbort);
    ASSERT_EQ(readAfterAbort.error(), kAborted);

    auto eraseAfterAbort = cancelledWriter->erase("other");
    ASSERT_FALSE(eraseAfterAbort);
    ASSERT_EQ(eraseAfterAbort.error(), kAborted);

    olderReader.reset();

    auto freshWriterTask = eng::AsyncNoSpan([&]() {
        auto freshWriter = kv::Tx(store);
        return freshWriter.insertOrAssign("k", "fresh");
    });
    ASSERT_TRUE(waitUntilReady(freshWriterTask));
    auto freshWriteResult = getTaskResultNoThrow(freshWriterTask);

    ASSERT_TRUE(freshWriteResult);
    ASSERT_TRUE(*freshWriteResult);
}

} // namespace
