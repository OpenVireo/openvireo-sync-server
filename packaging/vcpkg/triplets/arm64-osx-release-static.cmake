# Static libraries, release only, for the self-contained macOS binary (binaries-native.yml)
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
# std::format for floating point, which Drogon 1.9.14 uses, needs macOS 13.3. Keep in step with
# MACOSX_DEPLOYMENT_TARGET in binaries-native.yml and docs/install.md
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)
# Homebrew's libraries (e.g. util-linux's libuuid, which Drogon looks for) must not leak into the build: CMake also
# searches the prefix of every directory in PATH
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew\;/usr/local")
