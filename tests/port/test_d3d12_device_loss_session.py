# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production session's device-loss gate without WinRT or a GPU."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

std::map<std::string, std::string> environment;
std::vector<std::string> reports;
unsigned GetEnvironmentVariableA(const char* name, char* out, unsigned capacity) {
    const auto it = environment.find(name);
    if (it == environment.end()) return 0;
    const auto size = static_cast<unsigned>(it->second.size());
    if (size >= capacity) return size + 1;
    std::memcpy(out, it->second.c_str(), size + 1);
    return size;
}
std::string read_report(const char* name) { return environment[name]; }
void log_report(const char* name, const std::string&) { reports.emplace_back(name); }
template <class Read, class Log> void CollectPsoDxil(Read, Log) { reports.emplace_back("pso"); }
template <class Read, class Log> void CollectBatchJournals(Read, Log) { reports.emplace_back("batch"); }
template <class Read, class Log> void CollectDeviceApiRing(Read, Log) { reports.emplace_back("api"); }
void PollSession() {
__PRODUCTION_GATE__
}

int main() {
    // Both native and Mesa must stop on terminal command failure. A stale Mesa-only
    // device-loss marker must not stop a healthy native renderer.
    for (bool native : {false, true}) {
        for (bool stopped : {false, true}) {
            for (bool lost : {false, true}) {
                environment.clear();
                reports.clear();
                environment["NXBOX_NATIVE_D3D12_ACTIVE"] = native ? "1" : "0";
                environment["NXBOX_D3D12_COMMAND_FAILURE"] = stopped ? "1" : "0";
                environment["NXBOX_D3D12_DEVICE_LOST"] = lost ? "1" : "0";
                bool threw = false;
                try { PollSession(); }
                catch (const std::runtime_error& error) {
                    threw = true;
                    assert(std::strstr(error.what(), "stopping the game"));
                    assert(reports.size() == 9);
                    assert(reports[3] == "NXBOX_D3D12_FIRST_FAILURE");
                    assert(reports.back() == "api");
                }
                assert(threw == (stopped || (!native && lost)));
                if (!threw) assert(reports.empty());
            }
        }
    }
    // Re-launch: Context clears its terminal marker; absence is healthy as well as "0".
    environment.clear();
    environment["NXBOX_NATIVE_D3D12_ACTIVE"] = "1";
    reports.clear();
    PollSession();
    assert(reports.empty());
}
"""


class DeviceLossSessionTests(unittest.TestCase):
    def test_native_and_mesa_terminal_failure_gate(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        gate = source.split("        char command_failure[2]{};", 1)[1].split(
            "        const int request = lifecycle.request.exchange", 1
        )[0]
        gate = "char command_failure[2]{};" + gate
        with tempfile.TemporaryDirectory(prefix="nxbox-device-loss-") as temp:
            source_path = Path(temp) / "session.cpp"
            binary = Path(temp) / "session"
            source_path.write_text(DRIVER.replace("__PRODUCTION_GATE__", gate))
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source_path),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_native_terminal_marker_is_only_set_for_removed_device(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        report = source.split("void Context::ReportFailure", 1)[1].split("bool Context::IsFree", 1)[
            0
        ]
        self.assertRegex(
            report,
            r"if \(device && FAILED\(device->GetDeviceRemovedReason\(\)\)\)\s+"
            r'SetEnvironmentVariableA\("NXBOX_D3D12_COMMAND_FAILURE", "1"\);',
        )
        self.assertIn('SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", nullptr);', source)
