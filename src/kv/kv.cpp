#include <kv/kv.hpp>

namespace kv {
namespace us = userver;
namespace eng = us::engine;

template <typename T> inline T grabValueOf(std::optional<T> &opt)
{
    T x = std::move(*opt);
    opt.reset();
    return x;
}

template <typename T> inline T grabValueOf(std::optional<T> &&opt) { return grabValueOf(opt); }

template <typename T, typename... Us>
concept oneOf = (std::same_as<T, Us> || ...);

Store::Store(size_t maxConcurrentTxs) noexcept : txSemaphore(maxConcurrentTxs) {}

Tx::Tx(Store &store) noexcept : store(store), id(store.nextTxId.fetch_add(1)) {}

Tx::Result<void> Tx::insertOrAssign(std::string key, std::string value) noexcept
{
    using enum OpError;
    if (aborted)
        return std::unexpected{kAborted};
    if (sharedLocks.contains(key))
        return std::unexpected{kSharedToExclusiveLockUpgrade};
    if (!exclusiveLocks.contains(key)) {
        auto ret = acquireLock<true>(key);
        if (!ret)
            return std::unexpected{ret.error()};
    } else {
        erases.erase(key);
    }
    writes.insert_or_assign(std::move(key), std::move(value));
    return {};
}

void Tx::commit() noexcept
{
    auto &map = store.map;
    for (auto &&[k, v] : writes) {
        map.insert_or_assign(std::move(k), std::move(v));
    }
    for (auto &&k : erases)
        map.erase(k);
}

Tx::Result<std::optional<std::string>> Tx::find(std::string key) noexcept
{
    using enum OpError;
    if (aborted)
        return std::unexpected{kAborted};
    if (exclusiveLocks.contains(key)) {
        auto it = writes.find(key);
        if (it == end(writes))
            return {};
        return it->second;
    }
    if (!sharedLocks.contains(key)) {
        auto ret = acquireLock<false>(key);
        if (!ret)
            return std::unexpected{ret.error()};
        if (ret->inserted)
            return {};
    }
    auto it = writes.find(key);
    if (it != end(writes))
        return it->second;
    std::string value;
    if (!store.map.cvisit(key, [&value](const auto &x) { value = x.second; }))
        return {};
    return value;
}

Tx::Result<void> Tx::erase(std::string key) noexcept
{
    using enum OpError;
    if (aborted)
        return std::unexpected{kAborted};
    if (sharedLocks.contains(key))
        return std::unexpected{kSharedToExclusiveLockUpgrade};
    if (!exclusiveLocks.contains(key)) {
        auto ret = acquireLock<true>(key);
        if (!ret)
            return std::unexpected{ret.error()};
        if (ret->inserted)
            return std::unexpected{kMissing};
    } else {
        writes.erase(key);
    }
    erases.insert(key);
    return {};
}

Tx::LockingResult Tx::ensureLock(std::string key) noexcept
{
    auto &&owner = std::make_unique<LockSt>();
    auto *ptr = owner.get();
    bool inserted = store.locks.emplace_or_cvisit(key, std::move(owner), [&ptr](const auto &x) {
        ptr = x.second.get();
    });
    return {.ptr = ptr, .inserted = inserted};
}

template <typename T, bool E> bool Tx::isValidOpAttempt(const OpHandle &op, const T &x)
{
    if constexpr (std::is_same_v<T, None>) {
        return true;
    } else if constexpr (std::is_same_v<T, ActiveReaders>) {
        if constexpr (E) {
            return op.tx->id <= (*begin(x.readers))->tx->id;
        } else {
            return true;
        }
    } else {
        return op.tx->id <= x.writer->tx->id;
    }
}

namespace {

void sendWakeups(const std::vector<OpHandle *> &toWake)
{
    for (auto *op : toWake)
        op->wakeup->Send();
}

} // namespace

Tx::ReconcileResult Tx::reconcileLock(LockSt &lock)
{
    using enum OpHandle::State;
    ReconcileResult ret;
    auto &waitingReaders = lock.waitingReaders;
    auto &waitingWriters = lock.waitingWriters;
    auto invalidateQueue = [&]<bool E>(OpHandleSet &queue) -> bool {
        bool changed = false;
        for (auto it = begin(queue); it != end(queue);) {
            auto *op = *it;
            if (std::visit(
                    [&]<typename T>(const T &x) { return isValidOpAttempt<T, E>(*op, x); },
                    lock.users
                )) {
                it++;
            } else {
                op->state = kAborted;
                ret.toWake.push_back(op);
                it = queue.erase(it);
                changed = true;
            }
        }
        return changed;
    };
    auto grantReaders = [&](auto first, auto last) -> bool {
        if (first == last)
            return false;
        if (std::holds_alternative<None>(lock.users))
            lock.users = ActiveReaders{};
        auto &users = std::get<ActiveReaders>(lock.users);
        for (auto it = first; it != last;) {
            auto *op = *it;
            op->state = kGranted;
            users.readers.insert(op);
            ret.toWake.push_back(op);
            it = waitingReaders.erase(it);
        }
        return true;
    };
    auto grantWriter = [&](auto it) -> bool {
        auto *op = *it;
        op->state = kGranted;
        ret.toWake.push_back(op);
        waitingWriters.erase(it);
        lock.users = ActiveWriter{.writer = op};
        return true;
    };
    auto grantMoreReaders = [&]() -> bool {
        if (waitingReaders.empty())
            return false;
        auto reader = begin(waitingReaders);
        if (waitingWriters.empty())
            return grantReaders(reader, end(waitingReaders));
        auto writer = begin(waitingWriters);
        if (*reader < *writer)
            return grantReaders(reader, waitingReaders.lower_bound(*writer));
        return false;
    };

    for (size_t i = 0; i < 3; i++) {
        bool changed = invalidateQueue.template operator()<false>(waitingReaders);
        if (invalidateQueue.template operator()<true>(waitingWriters))
            changed = true;
        bool hasReaders = !waitingReaders.empty();
        bool hasWriters = !waitingWriters.empty();
        auto reader = hasReaders ? begin(waitingReaders) : end(waitingReaders);
        auto writer = hasWriters ? begin(waitingWriters) : end(waitingWriters);
        if (std::visit(
                [&]<typename T>(T &) -> bool {
                    if constexpr (std::is_same_v<T, None>) {
                        if (hasWriters && hasReaders) {
                            if (*reader < *writer)
                                return grantReaders(reader, waitingReaders.lower_bound(*writer));
                            return grantWriter(writer);
                        }
                        if (hasWriters)
                            return grantWriter(writer);
                        if (hasReaders)
                            return grantReaders(reader, end(waitingReaders));
                        return false;
                    } else if constexpr (std::is_same_v<T, ActiveReaders>) {
                        return grantMoreReaders();
                    } else {
                        return false;
                    }
                },
                lock.users
            )) {
            changed = true;
        }
        if (!changed)
            return ret;
    }
    return ret;
}

std::vector<OpHandle *> Tx::cleanupEnqueuedFor() noexcept
{
    using enum OpHandle::State;
    std::vector<OpHandle *> toWake;
    if (!enqueuedFor)
        return toWake;

    auto &x = *enqueuedFor;
    bool shouldAwaitSend = false;
    {
        std::unique_lock<eng::Mutex> guard{x.lock->mutex};
        if (x.op->state == kWaiting) {
            bool erased = false;
            if (x.isWrite) {
                erased = x.lock->waitingWriters.erase(x.op.get()) != 0;
            } else {
                erased = x.lock->waitingReaders.erase(x.op.get()) != 0;
            }
            if (erased)
                toWake = reconcileLock(*x.lock).toWake;
        } else {
            shouldAwaitSend = true;
        }
    }

    if (shouldAwaitSend) {
        x.op->wakeup->WaitNonCancellable();
        std::unique_lock<eng::Mutex> guard{x.lock->mutex};
        if (x.op->state == kGranted) {
            if (x.isWrite) {
                x.lock->users = None{};
            } else {
                auto &activeReaders = std::get<ActiveReaders>(x.lock->users);
                auto &readers = activeReaders.readers;
                readers.erase(x.op.get());
                if (readers.empty())
                    x.lock->users = None{};
            }
            toWake = reconcileLock(*x.lock).toWake;
        }
    }

    enqueuedFor = {};
    return toWake;
}

template <bool E> Tx::Result<Tx::LockingResult> Tx::acquireLock(std::string key) noexcept
{
    using enum AbortCause;
    using enum OpError;
    auto ret = ensureLock(key);
    auto *ptr = ret.ptr;
    enqueuedFor = LockOp{.lock = ptr, .isWrite = E, .op = std::make_unique<OpHandle>(this)};
    std::vector<OpHandle *> toWake;
    {
        std::unique_lock<eng::Mutex> lock{ptr->mutex};
        OpHandleSet *q;
        if constexpr (E) {
            q = &ptr->waitingWriters;
        } else {
            q = &ptr->waitingReaders;
        }
        if (std::visit(
                [&]<typename T>(const T &x) { return isValidOpAttempt<T, E>(*enqueuedFor->op, x); },
                ptr->users
            )) {
            q->insert(enqueuedFor->op.get());
            toWake = reconcileLock(*ptr).toWake;
        } else {
            enqueuedFor = {};
            aborted = kContention;
            return std::unexpected{kAborted};
        }
    }
    sendWakeups(toWake);
    auto waitStatus = enqueuedFor->op->wakeup->WaitUntil(eng::Deadline{});
    if (waitStatus == eng::FutureStatus::kCancelled) {
        aborted = kCancelled;
        sendWakeups(cleanupEnqueuedFor());
        return std::unexpected{kAborted};
    }
    if (enqueuedFor->op->state == OpHandle::State::kAborted) {
        enqueuedFor = {};
        aborted = kContention;
        return std::unexpected{kAborted};
    }
    LocksMap *locks;
    if constexpr (E) {
        locks = &exclusiveLocks;
    } else {
        locks = &sharedLocks;
    }
    locks->emplace(key, grabValueOf(enqueuedFor));
    return {{.ptr = ptr, .inserted = ret.inserted}};
}

Tx::~Tx()
{
    using enum OpHandle::State;
    for (auto &&[k, op] : exclusiveLocks) {
        std::vector<OpHandle *> toWake;
        {
            std::unique_lock<eng::Mutex> guard{op.lock->mutex};
            op.lock->users = None{};
            toWake = reconcileLock(*op.lock).toWake;
        }
        sendWakeups(toWake);
    }
    for (auto &&[k, lockOp] : sharedLocks) {
        std::vector<OpHandle *> toWake;
        {
            std::unique_lock<eng::Mutex> guard{lockOp.lock->mutex};
            auto &activeReaders = std::get<ActiveReaders>(lockOp.lock->users);
            auto &readers = activeReaders.readers;
            readers.erase(lockOp.op.get());
            if (readers.empty()) {
                lockOp.lock->users = None{};
            }
            toWake = reconcileLock(*lockOp.lock).toWake;
        }
        sendWakeups(toWake);
    }
    if (enqueuedFor) {
        sendWakeups(cleanupEnqueuedFor());
    }
}

} // namespace kv
