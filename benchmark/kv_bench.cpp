#include <kv/kv.hpp>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include <userver/engine/async.hpp>
#include <userver/engine/run_standalone.hpp>
#include <userver/engine/sleep.hpp>

namespace {

namespace us = userver;
namespace eng = us::engine;

constexpr size_t kMaxConcurrentTxs = 64;
constexpr std::chrono::seconds kBenchWaitTimeout{1};

template <typename T, typename E> [[nodiscard]] T requireValue(std::expected<T, E> result) noexcept
{
    if (!result)
        std::abort();
    return std::move(*result);
}

template <typename E> void requireSuccess(std::expected<void, E> result) noexcept
{
    if (!result)
        std::abort();
}

[[nodiscard]] bool waitUntilReady(eng::SingleUseEvent &event) noexcept
{
    return event.WaitUntil(eng::Deadline::FromDuration(kBenchWaitTimeout)) ==
           eng::FutureStatus::kReady;
}

template <typename F>
void runInBenchmarkEngine(benchmark::State &state, size_t workerThreads, F &&func)
{
    us::engine::RunStandalone(workerThreads, [&]() { func(state); });
}

// NOLINTNEXTLINE(readability-identifier-naming)
static void KvBench_RunInTxReadOnly(benchmark::State &state)
{
    runInBenchmarkEngine(state, 1, [&](benchmark::State &benchState) {
        auto store = kv::Store(kMaxConcurrentTxs);
        benchmark::DoNotOptimize(requireValue(store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
            requireSuccess(tx.insertOrAssign("hot", "value"));
            return 1;
        })));

        for (auto _ : benchState) {
            benchmark::DoNotOptimize(_);
            auto value = requireValue(
                store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<std::optional<std::string>> {
                    return tx.find("hot");
                })
            );
            benchmark::DoNotOptimize(value);
        }
    });
}

BENCHMARK(KvBench_RunInTxReadOnly);

// NOLINTNEXTLINE(readability-identifier-naming)
static void KvBench_RunInTxWriteHeavy(benchmark::State &state)
{
    runInBenchmarkEngine(state, 1, [&](benchmark::State &benchState) {
        auto store = kv::Store(kMaxConcurrentTxs);
        auto toggle = false;

        for (auto _ : benchState) {
            benchmark::DoNotOptimize(_);
            std::string_view value{toggle ? "value-a" : "value-b"};
            auto result = requireValue(store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
                requireSuccess(tx.insertOrAssign("hot", std::string{value}));
                return 1;
            }));
            benchmark::DoNotOptimize(result);
            toggle = !toggle;
        }
    });
}

BENCHMARK(KvBench_RunInTxWriteHeavy);

// NOLINTNEXTLINE(readability-identifier-naming)
static void KvBench_RunInTxMixedReadWrite(benchmark::State &state)
{
    runInBenchmarkEngine(state, 1, [&](benchmark::State &benchState) {
        auto store = kv::Store(kMaxConcurrentTxs);
        benchmark::DoNotOptimize(requireValue(store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
            requireSuccess(tx.insertOrAssign("hot", "value-a"));
            requireSuccess(tx.insertOrAssign("warm", "value-b"));
            return 1;
        })));

        auto toggle = false;

        for (auto _ : benchState) {
            benchmark::DoNotOptimize(_);
            std::string_view nextHot{toggle ? "value-a" : "value-b"};
            std::string_view nextWarm{toggle ? "value-c" : "value-d"};
            auto result = requireValue(store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
                auto hot = tx.find("hot");
                if (!hot || !hot->has_value())
                    return std::unexpected{kv::Tx::OpError::kAborted};
                requireSuccess(tx.insertOrAssign("next-hot", std::string{nextHot}));
                requireSuccess(tx.insertOrAssign("next-warm", std::string{nextWarm}));
                return 1;
            }));
            benchmark::DoNotOptimize(result);
            toggle = !toggle;
        }
    });
}

BENCHMARK(KvBench_RunInTxMixedReadWrite);

// NOLINTNEXTLINE(readability-identifier-naming)
static void KvBench_RunInTxEraseUpdate(benchmark::State &state)
{
    runInBenchmarkEngine(state, 1, [&](benchmark::State &benchState) {
        auto store = kv::Store(kMaxConcurrentTxs);
        benchmark::DoNotOptimize(requireValue(store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
            requireSuccess(tx.insertOrAssign("stable", "value-a"));
            return 1;
        })));

        auto toggle = false;

        for (auto _ : benchState) {
            benchmark::DoNotOptimize(_);
            std::string_view nextValue{toggle ? "value-a" : "value-b"};
            auto result = requireValue(store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
                requireSuccess(tx.insertOrAssign("scratch", "tmp"));
                requireSuccess(tx.erase("scratch"));
                requireSuccess(tx.insertOrAssign("stable", std::string{nextValue}));
                return 1;
            }));
            benchmark::DoNotOptimize(result);
            toggle = !toggle;
        }
    });
}

BENCHMARK(KvBench_RunInTxEraseUpdate);

// NOLINTNEXTLINE(readability-identifier-naming)
static void KvBench_RunInTxWithRetryAfterContention(benchmark::State &state)
{
    runInBenchmarkEngine(state, 4, [&](benchmark::State &benchState) {
        auto store = kv::Store(kMaxConcurrentTxs);
        benchmark::DoNotOptimize(requireValue(store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
            requireSuccess(tx.insertOrAssign("hot", "seed"));
            return 1;
        })));

        for (auto _ : benchState) {
            benchmark::DoNotOptimize(_);
            eng::SingleUseEvent holderEntered;
            eng::SingleUseEvent releaseHolder;

            auto holder = eng::AsyncNoSpan([&]() -> kv::Store::Result<int, kv::Tx::OpError> {
                return store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
                    auto found = tx.find("hot");
                    if (!found || !found->has_value())
                        return std::unexpected{kv::Tx::OpError::kAborted};
                    holderEntered.Send();
                    releaseHolder.WaitNonCancellable();
                    return 1;
                });
            });

            if (!waitUntilReady(holderEntered))
                std::abort();

            auto releaser = eng::AsyncNoSpan([&]() {
                eng::SleepFor(std::chrono::milliseconds{1});
                releaseHolder.Send();
                return 0;
            });

            auto result = requireValue(store.runInTxWithRetry(
                [](kv::Tx &tx) -> kv::Tx::Result<int> {
                    requireSuccess(tx.insertOrAssign("hot", "retry"));
                    return 1;
                },
                4, std::chrono::milliseconds{1}
            ));
            benchmark::DoNotOptimize(result);
            benchmark::DoNotOptimize(releaser.Get());
            benchmark::DoNotOptimize(requireValue(holder.Get()));
        }
    });
}

BENCHMARK(KvBench_RunInTxWithRetryAfterContention);

// NOLINTNEXTLINE(readability-identifier-naming)
static void KvBench_RunInTxConcurrentContention(benchmark::State &state)
{
    runInBenchmarkEngine(state, 4, [&](benchmark::State &benchState) {
        auto store = kv::Store(kMaxConcurrentTxs);
        benchmark::DoNotOptimize(requireValue(store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
            requireSuccess(tx.insertOrAssign("hot", "seed"));
            return 1;
        })));

        for (auto _ : benchState) {
            benchmark::DoNotOptimize(_);
            eng::SingleUseEvent readerReady;
            eng::SingleUseEvent releaseReader;

            auto reader = eng::AsyncNoSpan([&]() -> kv::Store::Result<int, kv::Tx::OpError> {
                return store.runInTx([&](kv::Tx &tx) -> kv::Tx::Result<int> {
                    auto found = tx.find("hot");
                    if (!found || !found->has_value())
                        return std::unexpected{kv::Tx::OpError::kAborted};
                    readerReady.Send();
                    releaseReader.WaitNonCancellable();
                    return 1;
                });
            });

            if (!waitUntilReady(readerReady))
                std::abort();

            auto writer = eng::AsyncNoSpan([&]() -> kv::Store::Result<int, kv::Tx::OpError> {
                return store.runInTx([](kv::Tx &tx) -> kv::Tx::Result<int> {
                    requireSuccess(tx.insertOrAssign("hot", "writer"));
                    return 1;
                });
            });

            eng::SleepFor(std::chrono::milliseconds{1});
            releaseReader.Send();

            auto writerResult = writer.Get();
            auto readerResult = reader.Get();

            benchmark::DoNotOptimize(writerResult.has_value());
            benchmark::DoNotOptimize(readerResult.has_value());
        }
    });
}

BENCHMARK(KvBench_RunInTxConcurrentContention);

} // namespace
