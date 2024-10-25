CMAKE_MINIMUM_REQUIRED(VERSION 3.9)
project(hrtng)

#run ccmake to change IDASDK_VER
SET(IDASDK_VER "84" CACHE STRING "84" )
SET(IDASDK_DIR /home/tpo/tools/idapro8.4/idasdk_pro84)
MESSAGE("${PROJECT_NAME}")
MESSAGE("Using IDA SDK dir: ${IDASDK_DIR}")
MESSAGE("-------------------")

IF (${IDASDK_VER} STREQUAL "90")
  SET(UNI64 1)
ELSE()
  SET(UNI64 0)
ENDIF ()

IF( NOT CMAKE_BUILD_TYPE )
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
ENDIF()
SET(CMAKE_SKIP_RPATH TRUE)

INCLUDE(ida_plugin.cmake)

ADD_IDA_PLUGIN(hrtng 
  hrtng.cpp
  appcall.cpp
  appcall_view.cpp
  cast.cpp
  comhelper.cpp
  crpp.cpp
  decr.cpp
  deinline.cpp
  deob.cpp
  helpers.cpp
  lit.cpp
  MicrocodeExplorer.cpp
  microavx.cpp
  msig.cpp
  opt.cpp
  structures.cpp
  rename.cpp
  unflat.cpp
  )

if(NOT MSVC)
	add_compile_options(-flto)
endif()
set(CRYPTOPP_BUILD_TESTING OFF CACHE BOOL "Build library tests")
add_subdirectory(cryptopp-cmake)
INCLUDE_DIRECTORIES(${cryptopp_INCLUDE_DIRS})

IF(NOT ${UNI64})
  TARGET_LINK_LIBRARIES(hrtng_64 cryptopp)
ENDIF()
TARGET_LINK_LIBRARIES(hrtng    cryptopp)
