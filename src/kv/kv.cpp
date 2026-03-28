#include <kv/kv.hpp>

namespace kv {

std::string_view version() noexcept { return kVersion; }

Store::Store(size_t maxConcurrentTxs)
    : txSemaphore(std::make_unique<userver::engine::CancellableSemaphore>(maxConcurrentTxs))
{
}

} // namespace kv
