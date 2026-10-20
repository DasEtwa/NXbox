# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production SSL descriptor ownership with a counting BSD socket layer."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SSL = ROOT / "src/core/hle/service/ssl/ssl.cpp"


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


DRIVER = r"""
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
using s32 = std::int32_t;
using Result = int;
template <typename T> using Out = T*;
constexpr Result ResultSuccess = 0, ResultInvalidSocket = 1, ResultInternalError = 2;
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define ASSERT(value) assert(value)
#define ASSERT_OR_EXECUTE(value, action) do { if (!(value)) action } while (false)
#define R_UNLESS(value, result) do { if (!(value)) return result; } while (false)
#define R_SUCCEED() return ResultSuccess
namespace Network { struct SocketBase {}; }
namespace Service::Sockets {
enum class Errno { SUCCESS, BADF };
struct BSD_USA {
    int duplicates = 0;
    bool fail_duplicate = false;
    bool fail_lookup = false;
    std::vector<s32> closes;
    std::shared_ptr<Network::SocketBase> socket = std::make_shared<Network::SocketBase>();
    std::variant<s32, Errno> DuplicateSocketImpl(s32) {
        if (fail_duplicate) return Errno::BADF;
        return 100 + duplicates++;
    }
    std::optional<std::shared_ptr<Network::SocketBase>> GetSocket(s32) {
        if (fail_lookup) return std::nullopt;
        return socket;
    }
    Errno CloseImpl(s32 fd) {
        for (auto previous : closes) assert(previous != fd);
        closes.push_back(fd);
        return Errno::SUCCESS;
    }
};
}
struct System {
    std::shared_ptr<Service::Sockets::BSD_USA> bsd = std::make_shared<Service::Sockets::BSD_USA>();
    System& ServiceManager() { return *this; }
    template <typename T> std::shared_ptr<T> GetService(const char*) { return bsd; }
};
struct Backend {
    int assignments = 0;
    void SetSocket(std::shared_ptr<Network::SocketBase>) { ++assignments; }
};
enum class OptionType { DoNotCloseSocket, GetServerCertChain, SkipDefaultVerify, EnableAlpn };
struct SharedData { int connection_count = 1; };
class ISslConnection {
public:
    explicit ISslConnection(System& system_in) : system{system_in} {}
    METHODS
    System& system;
    std::shared_ptr<SharedData> shared_data = std::make_shared<SharedData>();
    std::unique_ptr<Backend> backend = std::make_unique<Backend>();
    std::optional<int> fd_to_close;
    std::shared_ptr<Network::SocketBase> socket;
    bool do_not_close_socket = false;
    bool get_server_cert_chain = false;
    bool skip_default_verify = false;
    bool enable_alpn = false;
    bool did_handshake = false;
};
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    System system;
    s32 output = -77;
    {
        ISslConnection connection{system};
        if (scenario == "before" || scenario == "after_false" || scenario == "lookup_borrowed") {
            assert(connection.SetOption(OptionType::DoNotCloseSocket, 1) == ResultSuccess);
        }
        if (scenario == "duplicate_failure") system.bsd->fail_duplicate = true;
        if (scenario == "lookup_failure" || scenario == "lookup_borrowed") system.bsd->fail_lookup = true;
        const auto result = connection.SetSocketDescriptor(7, &output);
        if (scenario == "duplicate_failure" || scenario == "lookup_failure" || scenario == "lookup_borrowed") {
            assert(result != ResultSuccess);
            assert(!connection.socket);
            assert(connection.backend->assignments == 0);
            assert(output == -1);
        } else {
            assert(result == ResultSuccess);
            assert(connection.socket == system.bsd->socket);
            assert(connection.backend->assignments == 1);
            assert(output == (connection.do_not_close_socket ? 100 : -1));
            if (scenario == "after" || scenario == "after_false") {
                const bool previous = connection.do_not_close_socket;
                // Accepted (MK8D does this) but ownership stays as decided at SetSocketDescriptor.
                assert(connection.SetOption(OptionType::DoNotCloseSocket, !previous) == ResultSuccess);
                assert(connection.do_not_close_socket == !previous);
            }
            if (scenario == "reset_twice") {
                for (int attempt = 0; attempt < 2; ++attempt) {
                    assert(connection.SetSocketDescriptor(8, &output) != ResultSuccess);
                    assert(connection.socket == system.bsd->socket);
                    assert(connection.backend->assignments == 1);
                }
                assert(system.bsd->duplicates == 1);
            }
            if (output != -1) {
                assert(system.bsd->closes.empty());
                // The returned duplicate belongs to the caller in this HLE implementation.
                assert(system.bsd->CloseImpl(output) == Service::Sockets::Errno::SUCCESS);
            }
        }
    }
    if (scenario == "duplicate_failure") {
        assert(system.bsd->closes.empty());
    } else {
        assert(system.bsd->closes == std::vector<s32>{100});
    }
}
"""


class SSLSocketOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="nxbox-ssl-ownership-")
        cls.addClassCleanup(cls.temporary.cleanup)
        source = SSL.read_text()
        methods = "\n".join(
            method(source, signature)
            for signature in (
                "~ISslConnection()",
                "Result SetSocketDescriptor(",
                "Result SetOption(",
            )
        )
        driver = Path(cls.temporary.name) / "ownership.cpp"
        cls.binary = Path(cls.temporary.name) / "ownership"
        driver.write_text(DRIVER.replace("METHODS", methods))
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wshadow",
                str(driver),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def run_scenario(self, scenario):
        result = subprocess.run(
            [str(self.binary), scenario], check=False, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_default_closes_duplicate(self):
        self.run_scenario("default")

    def test_option_set_before_transfers_duplicate(self):
        self.run_scenario("before")

    def test_option_set_after_keeps_ownership(self):
        self.run_scenario("after")

    def test_option_cleared_after_keeps_ownership(self):
        self.run_scenario("after_false")

    def test_reset_descriptor_twice_is_rejected(self):
        self.run_scenario("reset_twice")

    def test_duplicate_failure_does_not_close(self):
        self.run_scenario("duplicate_failure")

    def test_lookup_failure_closes_duplicate(self):
        self.run_scenario("lookup_failure")

    def test_lookup_failure_with_option_closes_duplicate(self):
        self.run_scenario("lookup_borrowed")


if __name__ == "__main__":
    unittest.main()
