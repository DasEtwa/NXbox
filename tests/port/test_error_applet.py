# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the Xbox frontend's real three entry points and dismissal lifecycle."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include "eden_uwp/error_applet.h"
namespace Core::Frontend { ErrorApplet::~ErrorApplet() = default; }
int main() {
    auto display = std::make_shared<EdenXbox::ErrorDisplay>();
    EdenXbox::ErrorApplet applet(display);
    // Decimal, padded Switch presentation uses the result module plus 2000.
    const Result error{static_cast<ErrorModule>(56), 2306};
    for (int mode = 0; mode < 3; ++mode) {
        int calls = 0;
        auto finished = [&] { ++calls; assert(!display->Current()); };
        if (mode == 0) applet.ShowError(error, finished);
        if (mode == 1) applet.ShowErrorWithTimestamp(error, std::chrono::seconds{123}, finished);
        if (mode == 2) applet.ShowCustomErrorText(error, "Main message", "Detail message", finished);
        assert(calls == 0);
        auto current = display->Current();
        assert(current && current->code == "2056-2306");
        assert(current->message == (mode == 2 ? "Main message\n\nDetail message" : ""));
        display->Dismiss(current->id + 1); // stale events cannot dismiss another error
        assert(calls == 0);
        display->Dismiss(current->id); // A or B reaches this same dismissal path
        display->Dismiss(current->id);
        assert(calls == 0); // UI never invokes guest code
        display->CompleteDismissed();
        display->CompleteDismissed();
        assert(calls == 1);
        applet.Close();
        display->CompleteDismissed();
        assert(calls == 1);
    }
    for (int mode = 0; mode < 3; ++mode) {
        int padded_calls = 0;
        const Result padded{ErrorModule::Kernel, 7};
        auto finished = [&] { ++padded_calls; };
        if (mode == 0) applet.ShowError(padded, finished);
        if (mode == 1) applet.ShowErrorWithTimestamp(padded, std::chrono::seconds{0}, finished);
        if (mode == 2) applet.ShowCustomErrorText(padded, "", "", finished);
        assert(display->Current()->code == "2001-0007");
        assert(display->Pending());
        display->Dismiss(display->Current()->id);
        assert(padded_calls == 0 && display->Pending());
        display->CompleteDismissed();
        assert(padded_calls == 1 && !display->Pending());
    }
    int calls = 0;
    applet.ShowCustomErrorText(Result{ErrorModule::Kernel, 7}, "", "Detail only", [&] { ++calls; });
    assert(display->Current()->code == "2001-0007");
    assert(display->Current()->message == "Detail only");
    applet.Close();
    applet.Close();
    display->CompleteDismissed();
    assert(calls == 1 && !display->Current());
    // Reentrant completion can enqueue the next request without deadlocking.
    applet.ShowError(error, [&] { applet.ShowError(error, [&] { ++calls; }); });
    display->Dismiss(display->Current()->id);
    display->CompleteDismissed();
    assert(display->Current());
    display->Dismiss(display->Current()->id);
    display->CompleteDismissed();
    assert(calls == 2);
    // Teardown discards callbacks before their guest applet is destroyed.
    applet.ShowError(error, [&] { ++calls; });
    display->Clear();
    assert(calls == 2 && !display->Current());
}
"""


class ErrorAppletTests(unittest.TestCase):
    def test_modes_and_exactly_once_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "common").mkdir()
            # Logging is irrelevant to this test; use the actual Result and frontend interfaces.
            (folder / "common/logging.h").write_text(
                "#pragma once\n#define LOG_CRITICAL(...) ((void)0)\n"
            )
            source = folder / "driver.cpp"
            source.write_text(DRIVER)
            binary = folder / "driver"
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wno-unused-parameter",
                    "-I",
                    directory,
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    str(ROOT / "src/eden_uwp/error_applet.cpp"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_session_registration_overlay_and_translations(self):
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn("std::make_unique<ErrorApplet>(session_ui.errors)", session)
        self.assertIn("Ui::RunErrorOverlay(", session)
        self.assertIn("session_ui.errors->Current().has_value()", session)
        overlay = (ROOT / "src/eden_uwp/ui/error_overlay.cpp").read_text()
        self.assertIn("renderer.Initialize(window, true)", overlay)
        self.assertIn("DrawSheet(renderer,", overlay)
        self.assertIn("input.Pressed(Button::A) || input.Pressed(Button::B)", overlay)
        self.assertIn("display.Dismiss(current->id)", overlay)
        for language in ("en", "pt_br"):
            table = (ROOT / f"src/eden_uwp/ui/lang/{language}.inc").read_text()
            for key in ("ErrorTitle", "ErrorDismiss", "ErrorScroll"):
                self.assertIn(f"Text::{key},", table)


if __name__ == "__main__":
    unittest.main()
