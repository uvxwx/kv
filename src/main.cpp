#include <kv/kv.hpp>

#include <cstring>
#include <print>

namespace {

void printHelp(const char *argv0) { std::print("Usage: {} [--help] [--version]\n", argv0); }

} // namespace

int main(int argc, char **argv)
{
    if (argc <= 1) {
        printHelp(argv[0]);
        return 0;
    }

    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        printHelp(argv[0]);
        return 0;
    }

    if (std::strcmp(argv[1], "--version") == 0) {
        std::print("{}\n", kv::version());
        return 0;
    }

    std::print("Unknown option: {}\n", argv[1]);
    return 2;
}
