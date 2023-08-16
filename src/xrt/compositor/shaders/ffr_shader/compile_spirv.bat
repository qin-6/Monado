IF NOT EXIST "shaders/NUL" mkdir "shaders"
C:\VulkanSDK\1.2.141.2\Bin32\glslc.exe fix_foveated_render.vert -o shaders/vert.spv
C:\VulkanSDK\1.2.141.2\Bin32\glslc.exe fix_foveated_render.frag -o shaders/frag.spv
pause