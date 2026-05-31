### Project Properties ###
cmake_minimum_required (VERSION 3.5)

project (ptt)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(EXECUTABLE_NAME "ptt")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "../")

set(MAIN_CPP "ptt.cpp")

add_executable(${EXECUTABLE_NAME} ${MAIN_CPP})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON)

find_package(Libinput REQUIRED)

include_directories(${Libinput_INCLUDE_DIRS})

include(FetchContent)

FetchContent_Declare(rohrkabel GIT_REPOSITORY "https://github.com/Curve/rohrkabel" GIT_TAG v5.1)
FetchContent_MakeAvailable(rohrkabel)

target_link_libraries(${EXECUTABLE_NAME} cr::rohrkabel ${Libinput_LIBRARIES} udev xkbcommon X11 config++)
