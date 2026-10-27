# Static libraries and the static MSVC runtime (/MT), release only, for the self-contained Windows binary
# (binaries-native.yml)
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
