cmake_minimum_required(VERSION 3.15)

file(GLOB SRC *.cpp)

add_executable(yolo_web_camera ${SRC})
target_link_opencv(yolo_web_camera)
target_link_libraries(yolo_web_camera PRIVATE pthread)
if(WEB_SHARED_JPEG)
    target_compile_definitions(flow_utils PUBLIC WEB_SHARED_JPEG)
endif()
if(FORCE_CAMERA_GEOMETRY)
    target_compile_definitions(yolo_web_camera PRIVATE FORCE_CAMERA_GEOMETRY="${FORCE_CAMERA_GEOMETRY}")
endif()
install(TARGETS yolo_web_camera)
