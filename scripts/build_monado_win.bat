@setlocal
 cd ..
 set BUILD_WIN=build_win
 set SHADER_PATH=%BUILD_WIN%\src\xrt\targets\service\shaders
 set LIBS_PATH=%BUILD_WIN%\src\xrt\targets\service\libs
 set CONFIG_PATH=%BUILD_WIN%\src\xrt\targets\service\config

 @rem Change current directory
 mkdir %BUILD_WIN%
 cd %BUILD_WIN%
@rem Set up environment for build
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
 
cmake .. -G Ninja -DCMAKE_BUILD_TYPE="Release" -DCMAKE_TOOLCHAIN_FILE="D:\vcpkg\scripts\buildsystems\vcpkg.cmake" -DXRT_FEATURE_SERVICE=ON
cd ..
ninja -C %BUILD_WIN%

mkdir %SHADER_PATH%
copy src\xrt\compositor\shaders\ffr_shader\shaders\frag.spv %SHADER_PATH%
copy src\xrt\compositor\shaders\ffr_shader\shaders\vert.spv %SHADER_PATH%

mkdir %CONFIG_PATH%
copy src\xrt\drivers\ovr\resources\session.json %CONFIG_PATH%

mkdir %LIBS_PATH%
copy E:\Monado\openxr_runtime\src\xrt\drivers\ovr\cloudxr\lib\* %LIBS_PATH%
