cmake_minimum_required(VERSION 3.15)

file(GLOB SRC *.cpp)

add_executable(yolo_demo_hdmi_camera ${SRC})
target_link_opencv(yolo_demo_hdmi_camera)
install(TARGETS yolo_demo_hdmi_camera)
