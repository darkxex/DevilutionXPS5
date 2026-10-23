# SDL2 for the PS5: the project's static build of SDL2, with a video driver for the console's one display and a
# render driver on Vulkan (RADV): SDL_ps5vk.c, SDL_render_ps5vk.c and the presenter they share, ps5vk.c.
cmake_minimum_required(VERSION 3.16)
project(SDL2PS5 C)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_PTHREADS ON CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_SOURCE_VERSION "2.30.12-g8c56053f13ca-devilutionx-vk")
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
# SDL_RENDER stays on: the PS5 Vulkan render driver below is registered in it.
foreach(feature HAPTIC SENSOR OPENGL OPENGLES VULKAN X11
                WAYLAND KMSDRM OFFSCREEN JACK PULSEAUDIO DBUS IBUS FCITX LIBUDEV)
    set(SDL_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(PS5 ON CACHE BOOL "" FORCE)
# The native folder's libc does not export these header-declared symbols.
foreach(symbol WCSLCPY WCSLCAT WCSCASECMP WCSNCASECMP)
    set(HAVE_${symbol} 0 CACHE INTERNAL "native libc contract" FORCE)
endforeach()
add_subdirectory("${SDL_SOURCE}" sdl)

# Replace upstream's OSMesa/scanout video code with the Vulkan one.
get_target_property(sources SDL2-static SOURCES)
list(FILTER sources EXCLUDE REGEX "/video/ps5/")
set_property(TARGET SDL2-static PROPERTY SOURCES "${sources}")
target_sources(SDL2-static PRIVATE SDL_ps5vk.c SDL_render_ps5vk.c ps5vk.c)
target_include_directories(SDL2-static PRIVATE "${SDL_SOURCE}/src" "${PS5_VULKAN_PREFIX}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(SDL2-static PRIVATE SDL_VIDEO_DRIVER_PS5=1 SDL_VIDEO_RENDER_PS5VK=1)

set_property(TARGET SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES
    "ScePad;SceUserService;SceSystemService;SceAudioOut")
target_compile_definitions(SDL2-static INTERFACE SDL_MAIN_HANDLED=1)
configure_file(sdl2.pc.in "${CMAKE_CURRENT_BINARY_DIR}/sdl/sdl2.pc" @ONLY)
