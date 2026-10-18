# conan 2.x recipe for avbase (docs/10 §12 V4, "integration via conan").
#
# STATUS: written 2026-10-18 (M13); NOT yet submitted to conancenter and not
# exercised by CI (no conan in the sandbox). The underlying build+install path
# it drives is the same one verified locally (cmake --install produces a
# find_package(avbase)-consumable package; see cmake/AvbaseInstall.cmake).

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
import os


class AvbaseConan(ConanFile):
    name = "avbase"
    # Keep in sync with VERSION.txt at the repo root; the CMake build reads
    # that file, so this attribute and project() agree by construction of the
    # release process.
    version = "0.1.0"
    license = "BSD-3-Clause"
    author = "avbase authors"
    url = "https://github.com/samychen/avbase"
    homepage = "https://github.com/samychen/avbase"
    description = "Chromium-style layered media player core"
    topics = ("media", "player", "ffmpeg", "video", "audio")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_ffmpeg": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_ffmpeg": True,
    }
    exports = "VERSION.txt", "LICENSE"
    generators = "CMakeDeps"

    def requirements(self):
        if self.options.with_ffmpeg:
            # Version floor mirrors cmake/FindFFmpeg.cmake's minimum; the
            # package's static archives carry FFmpeg symbols as LINK_ONLY, so
            # the dependency is mandatory for the default configuration.
            self.requires("ffmpeg/[>=6.1]")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(self, self.url + f"/archive/refs/tags/v{self.version}.tar.gz",
            strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["AVBASE_BUILD_TESTS"] = "OFF"
        tc.variables["AVBASE_BUILD_EXAMPLES"] = "OFF"
        tc.variables["AVBASE_ENABLE_FFMPEG"] = self.options.with_ffmpeg
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        # LICENSE travels with the package; conan reads it from the recipe's
        # exports, but a consumer inspecting the package wants it too.
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        # The install rules export avbase::player (and avbase::base/media/gl);
        # naming the file makes find_package(avbase) work for consumers that
        # use CMakeDeps + CMakeToolchain together.
        self.cpp_info.set_property("cmake_file_name", "avbase")
        self.cpp_info.set_property("cmake_target_name", "avbase::player")
        self.cpp_info.libs = ["avbase_player", "avbase_media", "avbase_base"]
