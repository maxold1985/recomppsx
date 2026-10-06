cmake_minimum_required(VERSION 3.16)
project(r3000a_recomp_psxgl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PSXRECOMP_BUILD_GL "Build PSXGPU OpenGL 3.3 presenter" ON)
option(PSXRECOMP_BUILD_GLFW_APP "Build integrated GLFW PC runner" OFF)
option(PSXRECOMP_BUILD_TESTS "Build tests" ON)

add_library(r3kcore STATIC
    src/decoder.cpp
    src/psxexe.cpp
    src/recompiler.cpp
)
target_include_directories(r3kcore PUBLIC include)

add_executable(r3000a_recomp src/recompiler_main.cpp)
target_link_libraries(r3000a_recomp PRIVATE r3kcore)

add_library(psxgpu_core STATIC
    third_party/psxgpu/src/psx_gpu.cpp
    third_party/psxgpu/src/psx_gpu_mmio.cpp
)
target_include_directories(psxgpu_core PUBLIC third_party/psxgpu/include)

add_library(psxruntime STATIC
    src/irq.cpp
    src/timers.cpp
    src/cdrom.cpp
    src/spu.cpp
    src/mdec.cpp
    src/pad_sio.cpp
    src/gte.cpp
    src/bios_hle.cpp
    src/psx_memory.cpp
    src/psx_runtime.cpp
)
target_include_directories(psxruntime PUBLIC include)
target_link_libraries(psxruntime PUBLIC r3kcore psxgpu_core)

if(PSXRECOMP_BUILD_GL)
    add_library(psxgpu_gl STATIC
        third_party/psxgpu/src/gl_api.cpp
        third_party/psxgpu/src/psx_gpu_gl.cpp
    )
    target_include_directories(psxgpu_gl
        PUBLIC third_party/psxgpu/include
        PRIVATE third_party/psxgpu/src
    )
    target_link_libraries(psxgpu_gl PUBLIC psxgpu_core)
endif()

if(PSXRECOMP_BUILD_GLFW_APP)
    if(NOT PSXRECOMP_BUILD_GL)
        message(FATAL_ERROR "PSXRECOMP_BUILD_GLFW_APP requires PSXRECOMP_BUILD_GL=ON")
    endif()

    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/CMakeLists.txt")
        add_subdirectory(third_party/glfw)
    else()
        find_package(glfw3 CONFIG REQUIRED)
    endif()

    set(PSXRECOMP_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated"
        CACHE PATH "Directory containing split generated game sources")
    file(GLOB PSXRECOMP_SPLIT_SOURCES CONFIGURE_DEPENDS
    "${PSXRECOMP_GENERATED_DIR}/game_dispatch.cpp"
    "${PSXRECOMP_GENERATED_DIR}/game_part_*.cpp"
)

	if(PSXRECOMP_SPLIT_SOURCES)

		message(STATUS "Using split recompiled game sources:")
		message(STATUS "  ${PSXRECOMP_SPLIT_SOURCES}")

		set(
			PSXRECOMP_GENERATED_SOURCES
			${PSXRECOMP_SPLIT_SOURCES}
		)

	elseif(EXISTS "${PSXRECOMP_GENERATED_DIR}/game.cpp")

		message(STATUS "Using generated game.cpp")

		set(
			PSXRECOMP_GENERATED_SOURCES
			"${PSXRECOMP_GENERATED_DIR}/game.cpp"
		)

	elseif(EXISTS "${PSXRECOMP_GENERATED_DIR}/generated_game.cpp")

		message(STATUS "Using generated_game.cpp")

		set(
			PSXRECOMP_GENERATED_SOURCES
			"${PSXRECOMP_GENERATED_DIR}/generated_game.cpp"
		)

	else()

		message(WARNING
			"No generated game found. "
			"Using game_stub.cpp."
		)

		set(
			PSXRECOMP_GENERATED_SOURCES
			"${PSXRECOMP_GENERATED_DIR}/game_stub.cpp"
		)

	endif()

    add_executable(psx_recomp_gl
        app/main_glfw.cpp
        ${PSXRECOMP_GENERATED_SOURCES}
    )
    target_include_directories(psx_recomp_gl PRIVATE include third_party/psxgpu/include "${PSXRECOMP_GENERATED_DIR}")
    target_link_libraries(
	psx_recomp_gl
	PRIVATE

	psxruntime
	psxgpu_gl
	glfw

	opengl32
	gdi32
	user32
	shell32
	winmm
)
endif()

if(PSXRECOMP_BUILD_TESTS)
    enable_testing()

    add_executable(decoder_test tests/decoder_test.cpp)
    target_link_libraries(decoder_test PRIVATE r3kcore)
    add_test(NAME decoder_test COMMAND decoder_test)

    add_executable(psx_integration_test tests/integration_test.cpp)
    target_link_libraries(psx_integration_test PRIVATE psxruntime)
    add_test(NAME psx_integration_test COMMAND psx_integration_test)

    add_executable(psx_hardware_test tests/hardware_test.cpp)
    target_link_libraries(psx_hardware_test PRIVATE psxruntime)
    add_test(NAME psx_hardware_test COMMAND psx_hardware_test)

    add_executable(psx_peripheral_test tests/peripheral_test.cpp)
    target_link_libraries(psx_peripheral_test PRIVATE psxruntime r3kcore)
    add_test(NAME psx_peripheral_test COMMAND psx_peripheral_test)

    add_executable(psx_cdroot_test tests/cdroot_test.cpp)
    target_link_libraries(psx_cdroot_test PRIVATE psxruntime)
    add_test(NAME psx_cdroot_test COMMAND psx_cdroot_test)
endif()
