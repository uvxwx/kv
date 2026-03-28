#pragma once

#include <cstddef>
#include <memory>

#include <userver/engine/semaphore.hpp>

#include <kv/version.hpp>

namespace kv {

class Store final {
public:
    explicit Store(size_t maxConcurrentTxs);

private:
    std::unique_ptr<userver::engine::CancellableSemaphore> txSemaphore;
};

} // namespace kv
