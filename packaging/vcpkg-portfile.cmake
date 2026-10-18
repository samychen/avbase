# vcpkg portfile for avbase (docs/10 §12 V4, "integration via vcpkg").
#
# STATUS: written 2026-10-18 (M13); NOT yet submitted to the vcpkg registry
# and not exercised by CI (no vcpkg in the sandbox). What is verified locally
# is the half this file relies on: `cmake --install` produces a package whose
# find_package(avbase) works from an out-of-tree consumer (see the install
# rules in cmake/AvbaseInstall.cmake). vcpkg_cmake_config_fixup below renames
# the config into vcpkg's expected layout, which is the only step this file
# adds on top of that verified path.
#
# To submit: copy to vcpkg's ports/avbase/ as portfile.cmake, add vcpkg.json
# with the same version, and open a PR. SHA512 must be the release tarball's.

set(VCPKG_POLICY_EMPTY_PACKAGE_ERROR enabled)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO samychen/avbase
    REF "v${VERSION}"
    SHA512 00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    HEAD_REF main
)

# Tests, examples and the developer CLI are build-time only; a consumer of
# the package wants the libraries and headers.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DAVBASE_BUILD_TESTS=OFF
        -DAVBASE_BUILD_EXAMPLES=OFF
        -DAVBASE_BUILD_FUZZ=OFF
        -DAVBASE_ENABLE_FFMPEG=ON
)
vcpkg_cmake_install()

# The install rules already export avbase::player et al.; fixup moves the
# config from lib/cmake/avbase to share/avbase, which is where vcpkg looks.
vcpkg_cmake_config_fixup(PACKAGE_NAME avbase CONFIG_PATH lib/cmake/avbase)

# FFmpeg and libyuv are the package's link-time dependencies (static
# archives carry their PRIVATE deps as $<LINK_ONLY:...> into the export).
# vcpkg resolves them from the triplet; a non-vcpkg host needs them on
# CMAKE_PREFIX_PATH (FindFFmpeg/FindLibyuv ship inside the package).
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
