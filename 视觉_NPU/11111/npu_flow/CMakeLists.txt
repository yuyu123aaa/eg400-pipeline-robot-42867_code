cmake_minimum_required(VERSION 3.15)

### OpenCV
find_package(OpenCV REQUIRED)
message(STATUS "OpenCV library status:")
message(STATUS "    version: ${OpenCV_VERSION}")
# message(STATUS "    libraries: ${OpenCV_LIBS}")
message(STATUS "    include path: ${OpenCV_INCLUDE_DIRS}")
function(target_link_opencv)
    foreach (arg ${ARGV})
        target_link_libraries(${arg} PRIVATE ${OpenCV_LIBS})
    endforeach ()
endfunction()

### Utils 
file(GLOB UTILS_SRC utils/*.cpp utils/media/*.cpp)
add_library(flow_utils STATIC ${UTILS_SRC})
target_include_directories(flow_utils PUBLIC utils utils/media)
# 当前板端SoftNPU执行这些YOLOv8软算子会中断超时，正式程序必须固定到CpuAcc。
target_compile_definitions(flow_utils PUBLIC FORCE_SOFTOPS_CPUACC=1)
if(CAMERA_BUFFER_COUNT)
    target_compile_definitions(flow_utils PUBLIC CAMERA_BUFFER_COUNT=${CAMERA_BUFFER_COUNT})
endif()
# 引入jpu opencv
target_link_libraries(flow_utils PUBLIC ${OpenCV_LIBS}) 
target_link_libraries(flow_utils PUBLIC jpu_driver) 
# 引入drm
target_include_directories(flow_utils PUBLIC utils/drm)
target_link_libraries(flow_utils PUBLIC drm)
target_link_directories(flow_utils PUBLIC utils/drm)
target_compile_definitions(flow_utils PUBLIC ENABLE_LIBDRM_HDMI)
# 通过这个扩散到所有依赖的库
link_libraries(flow_utils) 


### Armnn
# find_package(Armnn REQUIRED HINTS ${Armnn_DIR})
# target_link_libraries(flow_utils PUBLIC Armnn::fmt Armnn::Armnn Armnn::armnnUtils Armnn::armnnOnnxParser)
# target_link_directories(flow_utils PUBLIC ${Armnn_DIR}/../../) # TODO: 修改armnn的cmake脚本

### Armnn 第二种导入方式
set(armnn_install_perfix ${Armnn_DIR})
target_include_directories(flow_utils PUBLIC ${armnn_install_perfix}/include)
target_link_libraries(flow_utils PUBLIC armnn armnnUtils armnnOnnxParser protobuf fmt)
target_link_directories(flow_utils PUBLIC ${armnn_install_perfix}/lib)

# 扩散到所有依赖的库
link_libraries(flow_utils)



# add subdirectory 
file(GLOB THE_LIST ./*/CMakeLists.txt)

foreach(ITEM ${THE_LIST})
    message(STATUS "Find CMakeLists.txt at " ${ITEM})
    string(REPLACE ${CMAKE_CURRENT_SOURCE_DIR}/./ "" ITEM ${ITEM})
    string(REPLACE "/CMakeLists.txt" "" ITEM ${ITEM})
    message(STATUS "add_subdirectory(" ${ITEM} ")")
    add_subdirectory(${ITEM})
endforeach()

