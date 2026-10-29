"""Host unit tests for PR #7, compiling real C++20 production translation units.

Run from the repository root: python3 -m unittest discover -s tests -p test_native.py -v
Requires Linux, a C++20 compiler, pkg-config, and development packages for libcurl,
json-c, libarchive, libjpeg, and mbedTLS 2.x (the network backend uses mbedcrypto).
No devkitARM, hardware, network, or additional Python packages are needed.
The adapters are only on the test include path; production sources are unchanged.
Every executable and its synthetic SD card are automatically deleted.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMON = ["tests/support/test_support.cpp"]
STORAGE = ["source/backend/store.cpp", "source/backend/json.cpp",
           "source/backend/sd_card.cpp", "source/core/status.cpp"]
NETWORK = ["source/backend/net.cpp", "source/backend/sd_card.cpp",
           "source/core/status.cpp"]


class NativeTests(unittest.TestCase):
    def build_and_run(self, name, sources, *, crypto=False, real_log=False):
        # Missing prerequisites are failures, so an unavailable compiler/library
        # cannot silently turn a CI run green by skipping all C++ checks.
        packages = ["libcurl", "json-c"]
        if name == "install":
            packages.append("libarchive")
        if name in {"thumbnails", "thumbnail_source"}:
            packages.append("libjpeg")
        flags = subprocess.run(
            ["pkg-config", "--cflags", "--libs", *packages],
            capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertEqual(flags.returncode, 0, flags.stderr)
        with tempfile.TemporaryDirectory(prefix="ctgp7-build-") as directory:
            executable = str(Path(directory) / name)
            command = [
                *shlex.split(os.environ.get("CXX", "c++")), "-std=c++20",
                "-Wall", "-Wextra", "-pthread", "-Itests/support", "-Isource",
                "-Ilibs/imgui", f"tests/test_{name}.cpp", *COMMON, *sources,
                *(["-DTEST_REAL_LOG"] if real_log else []),
                *shlex.split(flags.stdout),
                *(["-lmbedcrypto"] if crypto else []),
                "-Wl,--wrap=time", "-Wl,--wrap=rename",
                "-Wl,--wrap=json_object_to_file_ext",
                "-Wl,--wrap=curl_easy_setopt", "-Wl,--wrap=curl_easy_getinfo",
                "-Wl,--wrap=curl_easy_perform", "-Wl,--wrap=curl_easy_cleanup",
                "-o", executable,
            ]
            build = subprocess.run(command, cwd=ROOT, capture_output=True,
                                   text=True, timeout=90, check=False)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([executable], cwd=directory, capture_output=True,
                                    text=True, timeout=20, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_core(self):
        self.build_and_run("core", ["source/core/status.cpp", "source/core/progress.cpp",
                                    "source/core/time.cpp"])

    def test_model(self):
        self.build_and_run("model", ["source/frontend/model.cpp"])

    def test_storage(self):
        self.build_and_run("storage", STORAGE)

    def test_network(self):
        self.build_and_run("network", NETWORK, crypto=True)

    def test_feed(self):
        self.build_and_run("feed", [*STORAGE, "source/core/progress.cpp",
                                    "source/backend/feed.cpp", "source/backend/net.cpp"], crypto=True)

    def test_install(self):
        self.build_and_run("install", [*STORAGE, "source/frontend/model.cpp",
                                       "source/backend/install.cpp",
                                       "source/backend/install_archive.cpp"])

    def test_thumbnails(self):
        self.build_and_run("thumbnails", ["source/frontend/model.cpp",
                                          "source/frontend/thumbnail_cache.cpp"])

    def test_log(self):
        self.build_and_run("log", ["source/core/log.cpp", "source/backend/sd_card.cpp",
                                   "source/core/status.cpp"], real_log=True)

    def test_audio(self):
        self.build_and_run("audio", ["source/backend/audio.cpp", "source/backend/sd_card.cpp",
                                     "source/core/status.cpp"])

    def test_thumbnail_source(self):
        self.build_and_run("thumbnail_source", ["source/backend/thumbnail_source.cpp",
                                                "source/backend/sd_card.cpp",
                                                "source/core/status.cpp"])


class FormatLiteralTests(unittest.TestCase):
    def test_rejects_wrong_argument_counts_at_compile_time(self):
        compiler = [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++20",
                    "-Isource", "-x", "c++", "-fsyntax-only", "-"]
        # A positive control prevents a broken toolchain/header from satisfying
        # the negative tests just because every source fails to compile.
        control = subprocess.run(
            compiler, cwd=ROOT, input='#include "core/format.h"\n'
            'auto a = mm::fmt::format("{{}}");\n'
            'auto b = mm::fmt::format("{{{}}}", 1);\n',
            capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertEqual(control.returncode, 0, control.stderr)
        for expression in ('mm::fmt::format("{}")', 'mm::fmt::format("", 1)',
                           'mm::fmt::format("{{}}", 1)', 'mm::fmt::format("{}{}", 1)'):
            with self.subTest(expression=expression):
                result = subprocess.run(
                    compiler, cwd=ROOT,
                    input=f'#include "core/format.h"\nauto value = {expression};\n',
                    capture_output=True, text=True, timeout=10, check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("placeholder_count_error", result.stderr)


if __name__ == "__main__":
    unittest.main()
