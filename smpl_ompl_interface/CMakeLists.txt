cmake_minimum_required(VERSION 2.8.12)
project(smpl_ompl_interface)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(catkin REQUIRED)
find_package(Boost REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(ompl REQUIRED)
find_package(smpl REQUIRED)

catkin_package(
    INCLUDE_DIRS include ${OMPL_INCLUDE_DIRS}
    LIBRARIES smpl_ompl_interface ${OMPL_LIBRARIES})

add_library(smpl_ompl_interface src/ompl_interface.cpp)
target_compile_options(smpl_ompl_interface PUBLIC -std=c++11)
target_compile_definitions(smpl_ompl_interface PRIVATE -DSMPL_CONSOLE_ROS)
target_include_directories(smpl_ompl_interface PUBLIC include)
target_include_directories(smpl_ompl_interface SYSTEM PUBLIC ${EIGEN3_INCLUDE_DIR})
target_include_directories(smpl_ompl_interface SYSTEM PUBLIC ${OMPL_INCLUDE_DIRS})
target_include_directories(smpl_ompl_interface SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(smpl_ompl_interface PRIVATE smpl::smpl)
target_link_libraries(smpl_ompl_interface PUBLIC ${OMPL_LIBRARIES})
target_link_libraries(smpl_ompl_interface PRIVATE ${Boost_LIBRARIES})

add_executable(smpl_ompl_test src/smpl_ompl.cpp)
target_compile_options(smpl_ompl_test PRIVATE -std=c++11)
target_include_directories(smpl_ompl_test SYSTEM PRIVATE ${OMPL_INCLUDE_DIRS})
target_include_directories(smpl_ompl_test SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(smpl_ompl_test PRIVATE smpl_ompl_interface)
target_link_libraries(smpl_ompl_test PRIVATE ${OMPL_LIBRARIES})
target_link_libraries(smpl_ompl_test PRIVATE ${Boost_LIBRARIES})
target_link_libraries(smpl_ompl_test PRIVATE smpl::smpl)

install(
    TARGETS smpl_ompl_interface
    RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
    LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
    ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION})
install(
    DIRECTORY include/smpl_ompl_interface/
    DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})


#================================================================================
# Formatting
#================================================================================


find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format-3.8)

if(CLANG_FORMAT_EXECUTABLE)
  message(STATUS "Found clang-format.")

  file(GLOB_RECURSE ALL_SOURCE_FILES
    LIST_DIRECTORIES false
    include/*.h include/*.hpp src/*.c src/*.cpp tests/*.cpp tests/*.hpp)
  list(LENGTH ALL_SOURCE_FILES NUM_SOURCE_FILES)

  add_custom_target(format
    COMMAND ${CMAKE_COMMAND} -E echo "Formatting ${NUM_SOURCE_FILES} files..."
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -style=file -i ${ALL_SOURCE_FILES}
    COMMAND ${CMAKE_COMMAND} -E echo "Done."
    DEPENDS ${CLANG_FORMAT_EXECUTABLE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_custom_target(check-format
    COMMAND ${CMAKE_COMMAND} -E echo "Checking code style of"
            "${NUM_SOURCE_FILES} files... "
    COMMAND ${CMAKE_SOURCE_DIR}/tools/check_format.sh
            ${CLANG_FORMAT_EXECUTABLE} ${ALL_SOURCE_FILES}
    COMMAND ${CMAKE_COMMAND} -E echo "Done."
    DEPENDS ${CLANG_FORMAT_EXECUTABLE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
else()
  message(STATUS "Looking for clang-format - NOT found, please install "
      "clang-format to enable automatic code formatting.")
endif()