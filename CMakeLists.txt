# https://cliutils.gitlab.io/modern-cmake
cmake_minimum_required(VERSION 3.21...3.24)
project(
  wavestream
  VERSION 0.0
  LANGUAGES C
)

# set C standard
set(CMAKE_C_STANDARD 17)

# handle command line options
if(DEFINED DEBUG)
message("debug was set")
message("but nothing was done because of it")
endif()

# find necessary packages
# note: the libsoundio and libsndfile repositories are included in
# this repo as submodules, but it is still necessary to manually
# build and install them. refer to their documentation for details,
# but generally it goes like this:
#
# cd ${ROOT_OF_LIBRARY}
# mkdir build
# cd build
# cmake ..
# cmake --build .
# sudo make install

# libsoundio
set(LIBSOUNDIO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third-party/libsoundio)
set(LIBSOUNDIO_MODULE_PATH ${LIBSOUNDIO_ROOT}/doc)

# libsndfile
set(LIBSNDFILE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third-party/libsndfile)
set(LIBSNDFILE_MODULE_PATH ${LIBSNDFILE_ROOT}/cmake)

# add module paths to allow cmake to find the libraries
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third-party/libsndfile/cmake ${CMAKE_CURRENT_SOURCE_DIR}/third-party/libsoundio/doc)

# find the required libraries
find_package(SoundIo REQUIRED)
find_package(SndFile REQUIRED)

# configure the main target
set(WAVESTREAM_TARGET_NAME ${CMAKE_PROJECT_NAME})
add_executable(${WAVESTREAM_TARGET_NAME})
target_sources(${WAVESTREAM_TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src/wavestream.c
)
target_include_directories(${WAVESTREAM_TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
	${CMAKE_CURRENT_LIST_DIR}/include
)
target_link_libraries(${WAVESTREAM_TARGET_NAME} PRIVATE
  ${SOUNDIO_LIBRARY}
  SndFile::sndfile
)

# micstream
set(MICSTREAM_TARGET_NAME micstream)
add_executable(${MICSTREAM_TARGET_NAME})
target_sources(${MICSTREAM_TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src/micstream.c
)
target_include_directories(${MICSTREAM_TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
	${CMAKE_CURRENT_LIST_DIR}/include
)
target_link_libraries(${MICSTREAM_TARGET_NAME} PRIVATE
  ${SOUNDIO_LIBRARY}
  SndFile::sndfile
)

# configure an example client
add_executable(client ${CMAKE_CURRENT_SOURCE_DIR}/src/client.c)
