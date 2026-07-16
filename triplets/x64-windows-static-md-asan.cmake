# AddressSanitizer variant of x64-windows-static-md: static libraries,
# dynamic CRT, ASan instrumentation compiled into every dependency so the
# DebugAsan configuration links a uniformly instrumented world. Carried
# in-repo (--overlay-triplets) so the build depends on no machine-local
# triplet.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_C_FLAGS "/fsanitize=address")
set(VCPKG_CXX_FLAGS "/fsanitize=address")
