#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <boost/unordered/concurrent_flat_map.hpp>

#include <userver/engine/semaphore.hpp>
#include <userver/engine/shared_mutex.hpp>
#include <userver/engine/single_use_event.hpp>
#include <userver/engine/sleep.hpp>

namespace kv {
namespace us = userver;
namespace eng = us::engine;

class Store;
class Tx;
struct LockSt;
struct OpHandle;

template <typename> struct IsStdExpected : std::false_type {};
template <typename T, typename E> struct IsStdExpected<std::expected<T, E>> : std::true_type {};
template <typename F>
concept TxCallback = std::invocable<F, Tx &> &&
                     IsStdExpected<std::remove_cvref_t<std::invoke_result_t<F, Tx &>>>::value;

class Tx final {
public:
    explicit Tx(Store &) noexcept;
    Tx(const Tx &) = delete;
    Tx &operator=(const Tx &) = delete;
    enum class AbortCause {
        kContention,
        kCancelled,
    };
    enum class OpError {
        kAborted,
        kMissing,
        kSharedToExclusiveLockUpgrade,
    };
    template <typename T> using Result = std::expected<T, OpError>;
    Result<void> insertOrAssign(std::string key, std::string value) noexcept;
    Result<void> erase(std::string key) noexcept;
    Result<std::optional<std::string>> find(std::string key) noexcept;
    void commit() noexcept;

    ~Tx();

private:
    friend class Store;

    struct LockOp {
        LockSt *lock;
        bool isWrite;
        std::unique_ptr<OpHandle> op;
    };
    using LocksMap = std::unordered_map<std::string, LockOp>;
    LocksMap exclusiveLocks;
    LocksMap sharedLocks;
    std::unordered_map<std::string, std::string> writes;
    std::unordered_set<std::string> erases;
    Store &store;

    struct LockingResult {
        LockSt *ptr;
        bool inserted;
    };
    template <bool E> Result<LockingResult> acquireLock(std::string key) noexcept;
    LockingResult ensureLock(std::string key) noexcept;

    std::optional<AbortCause> aborted;
    uint64_t id;
    std::optional<LockOp> enqueuedFor;
    friend struct OpHandle;

    struct ReconcileResult {
        std::vector<OpHandle *> toWake;
    };

    std::vector<OpHandle *> cleanupEnqueuedFor() noexcept;
    ReconcileResult reconcileLock(LockSt &lock);
    template <typename T, bool E> static bool isValidOpAttempt(const OpHandle &op, const T &x);
};

struct OpHandle {
    enum class State {
        kWaiting,
        kGranted,
        kAborted,
    };

    Tx *tx;
    std::unique_ptr<eng::SingleUseEvent> wakeup{std::make_unique<eng::SingleUseEvent>()};
    State state{State::kWaiting};
    bool operator<(const OpHandle &rhs) const noexcept { return tx->id < rhs.tx->id; }
};

struct OpHandlePtrComparator {
    bool operator()(const OpHandle *lhs, const OpHandle *rhs) const noexcept { return *lhs < *rhs; }
};

using OpHandleSet = std::set<OpHandle *, OpHandlePtrComparator>;

struct ActiveReaders {
    OpHandleSet readers;
};

struct ActiveWriter {
    OpHandle *writer;
};

struct None {};

struct LockSt {
    eng::Mutex mutex;
    // may be promoted but don't hold the lock yet
    using Users = std::variant<ActiveReaders, ActiveWriter, None>;
    Users users{None{}};
    OpHandleSet waitingReaders;
    OpHandleSet waitingWriters;
};

class Store final {
public:
    explicit Store(size_t maxConcurrentTxs) noexcept;
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

private:
    eng::CancellableSemaphore txSemaphore;

public:
    struct Aborted {
        bool retriable{false};
    };
    struct InvalidArguments {};
    template <typename E> using RunInTxError = std::variant<E, Aborted, InvalidArguments>;
    template <typename T, typename E> using Result = std::expected<T, RunInTxError<E>>;
    template <
        TxCallback F, typename R = std::remove_cvref_t<std::invoke_result_t<F, Tx &>>,
        typename T = R::value_type, typename E = R::error_type>
    Result<T, E> runInTx(F &&func) noexcept
    {
        using Unex = std::unexpected<RunInTxError<E>>;
        SemaphoreLock semLock{txSemaphore};
        if (!semLock.lock())
            return Unex{{Aborted{}}};
        Tx tx(*this);
        auto &&result = func(tx);
        if (tx.aborted)
            return tx.aborted.value() == Tx::AbortCause::kContention
                       ? Unex{{Aborted{.retriable = true}}}
                       : Unex{{Aborted{}}};
        if (!result)
            return Unex{{result.error()}};
        tx.commit();
        return std::move(result).value();
    }
    template <
        TxCallback F, typename R = std::remove_cvref_t<std::invoke_result_t<F, Tx &>>,
        typename T = R::value_type, typename E = R::error_type>
    Store::Result<T, E>
    runInTxWithRetry(F &&func, size_t maxAttempts, std::chrono::milliseconds interval) noexcept
    {
        using Unex = std::unexpected<RunInTxError<E>>;
        if (maxAttempts == 0)
            return Unex{{InvalidArguments{}}};
        std::mt19937 mt;
        std::uniform_int_distribution<size_t> dMult(1, 3);
        for (size_t i = 0; i < maxAttempts; i++) {
            if (auto &&result = runInTx(std::forward<F>(func));
                result ||
                !(std::holds_alternative<Aborted>(result.error()) &&
                  std::get<Aborted>(result.error()).retriable) ||
                i + 1 == maxAttempts) {
                return result;
            }
            eng::SleepFor(interval * dMult(mt));
        }
        std::abort();
    }

private:
    class SemaphoreLock final {
    public:
        explicit SemaphoreLock(eng::CancellableSemaphore &sem) noexcept : sem(sem) {}
        SemaphoreLock(const SemaphoreLock &) = delete;
        SemaphoreLock &operator=(const SemaphoreLock &) = delete;

        [[nodiscard]] bool lock() noexcept
        {
            locked = sem.try_lock_shared_until(eng::Deadline{});
            return locked;
        }

        ~SemaphoreLock()
        {
            if (locked)
                sem.unlock_shared();
        }

        eng::CancellableSemaphore &sem;
        bool locked{false};
    };
    friend class Tx;
    boost::concurrent_flat_map<std::string, std::unique_ptr<LockSt>> locks;
    boost::concurrent_flat_map<std::string, std::string> map;
    std::atomic<uint64_t> nextTxId{0};
};

} // namespace kv
