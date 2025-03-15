meson --cross-file build-win64.txt -Dopenxr_sdk=%OPENXR_SDK% -Dvulkan_sdk=%VULKAN_SDK% --buildtype debug build
ninja -C build