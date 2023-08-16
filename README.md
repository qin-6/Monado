# Monado - XR Runtime (XRT)

<!--
Copyright 2018-2021, Collabora, Ltd.

SPDX-License-Identifier: CC-BY-4.0

This must stay in sync with the last section!
-->

> * Main homepage and documentation: <https://monado.freedesktop.org/>
> * Promotional homepage: <https://monado.dev>
> * Maintained at <https://gitlab.freedesktop.org/monado/monado>
> * Latest API documentation: <https://monado.pages.freedesktop.org/monado>
> * Continuously-updated changelog of the default branch:
>   <https://monado.pages.freedesktop.org/monado/_c_h_a_n_g_e_l_o_g.html>

Monado is an open source XR runtime delivering immersive experiences such as VR
and AR on mobile, PC/desktop, and any other device
(because gosh darn people
come up with a lot of weird hardware).
Monado aims to be a complete and conforming implementation
of the OpenXR API made by Khronos.
The project is primarily developed on GNU/Linux, but also runs on Android and Windows.
"Monado" has no specific meaning and is just a name.

## Monado source tree

* `src/xrt/include` - headers that define the internal interfaces of Monado.
* `src/xrt/compositor` - code for doing distortion and driving the display hardware of a device.
* `src/xrt/auxiliary` - utilities and other larger components.
* `src/xrt/drivers` - hardware drivers.
* `src/xrt/state_trackers/oxr` - OpenXR API implementation.
* `src/xrt/targets` - glue code and build logic to produce final binaries.
* `src/external` - a small collection of external code and headers.

## Getting Started

Dependencies include:

* [CMake][] 3.13 or newer (Note Ubuntu 18.04 only has 3.10)
* Python 3.6 or newer
* Vulkan headers and loader - Fedora package `vulkan-loader-devel`
* OpenGL headers
* Eigen3 - Debian/Ubuntu package 'libeigen3-dev'
* glslangValidator - Debian/Ubuntu package `glslang-tools`, Fedora package `glslang`.
* libusb
* libudev - Fedora package `systemd-devel`
* Video 4 Linux - Debian/Ubuntu package `libv4l-dev`.

Optional (but recommended) dependencies:

* libxcb and xcb-xrandr development packages
* [OpenHMD][] 0.3.0 or newer (found using pkg-config)

Truly optional dependencies, useful for some drivers, app support, etc.:

* Doxygen
* Wayland development packages
* Xlib development packages
* libhidapi
* OpenCV
* libuvc
* ffmpeg
* libjpeg
* libbluetooth

Experimental Windows support requires the Vulkan SDK and also needs or works
best with the following vcpkg packages installed:

* pthreads eigen3 libusb hidapi zlib doxygen

If you have a recent [vcpkg](https://vcpkg.io) installed and use the appropriate
CMake toolchain file, the vcpkg manifest in the Monado repository will instruct
vcpkg to locally install the dependencies automatically.

# Windows
1.安装vcpkg，自动包管理器

    git clone https://github.com/Microsoft/vcpkg.git

    .\vcpkg\bootstrap-vcpkg.bat

2.修改script目录下的build_monado_win.bat，将-DCMAKE_TOOLCHAIN_FILE的值改为
     xxx\vcpkg\scripts\buildsystems\vcpkg.cmake"，xxx为你的vcpkg的安装目录

3.运行build_monado_win.bat，在build_win\src\xrt\targets\service\下生成可执行文件

4.在运行hello_xr，
  set XR_RUNTIME_JSON=xx\src\monado\build\openxr_monado-dev.json
  hello_xr.exe -G Vulkan

