cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME EdgeGateway)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/modbus_host.c        # 纯协议：CRC / 组帧 / 解析 / 双超时
    Core/Src/modbus_port_stm32.c  # 移植层：HAL 串口 + RS485 方向脚
    Core/Src/mqtt_codec.c         # 纯协议：MQTT Remaining Length 编解码（W3·S15）
    Core/Src/app_tasks.c          # 任务骨架：6 任务 + 4 队列 + 1 互斥锁（W4·S25）
    Core/Src/ring_buffer.c        # Flash 环形缓冲游标：wp/rp、满空判据（W4·S24）
    Core/Src/net_fsm.c            # 网络三态机 + 指数退避（W4·S24）
    Core/Src/rec_codec.c          # sensor_rec_t ↔ 32 B Flash 记录 + CRC（W4·S24）
    Core/Src/puback.c             # PUBACK 解析：40 02 xx xx（W4·S24）
    Core/Src/net_tx.c             # 窗口=1 发送编排：PUBACK 才推进 rp（W4·S24）
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# 让 printf 支持 %f（newlib-nano 默认剔除浮点格式化，靠 -u 强制拉进来）
# 注意：必须放在 add_executable() 之后，否则 target 还不存在
# 用连写形式 -u_printf_float，避免 "-u _printf_float" 被当成单个参数的坑
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u_printf_float)
