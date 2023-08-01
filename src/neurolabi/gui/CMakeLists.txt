cmake_minimum_required(VERSION 3.17.5)
project(neutu)

# main stuff

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)

# main srcs
set(NEUTU_SOURCES
    zneurontracer.cpp zneurontracer.h
    zneurontracerconfig.cpp zneurontracerconfig.h
    zjsonobject.cpp zjsonobject.h
    )

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# our target
if (BUILD_WITH_CONDA)
  set(LIBRARY_NAME condaneutu)
else ()
  set(LIBRARY_NAME neutu)
endif ()
add_library(${LIBRARY_NAME} STATIC ${NEUTU_SOURCES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# target_compile_definitions(${LIBRARY_NAME} PUBLIC _QT_GUI_USED_)
target_compile_definitions(${LIBRARY_NAME} PUBLIC lapack_complex_float=std::complex<float> lapack_complex_double=std::complex<double>)
target_compile_definitions(${LIBRARY_NAME} PUBLIC QT_NO_KEYWORDS QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY QT_NO_CAST_TO_ASCII QT_NO_VERSION_TAGGING)
target_compile_definitions(${LIBRARY_NAME} PUBLIC BOOST_CONTAINER_NO_LIB BOOST_JSON_NO_LIB)
if (WIN32)
  target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 _USE_MATH_DEFINES _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING _HAS_DEPRECATED_RESULT_OF)

  target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  target_compile_options(${LIBRARY_NAME} PUBLIC /W4)
  target_compile_options(${LIBRARY_NAME} PUBLIC /bigobj) # increases the number of sections that an object file can contain.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4828) # The file contains a character starting at offset 0x60 that is illegal in the current source character set (codepage 65001).
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd5105) # macro expansion producing 'defined' has undefined behavior, from windows sdk
  target_compile_options(${LIBRARY_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${LIBRARY_NAME} PUBLIC /analyze) # The C/C++ Code Analysis tool provides information to developers about possible defects in their C/C++ source code
    # target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception. Consider using _malloca instead.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6326) # Potential comparison of a constant with another constant.
    # target_compile_options(${LIBRARY_NAME} PUBLIC /wd6330) # Incorrect type passed as parameter in call to function.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6386) # Buffer overrun while writing to 'data'.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd28020)
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6239) #  (<non-zero constant> && <expression>) always evaluates to the result of <expression>. Did you intend to use the bitwise-and operator?
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6336) # Arithmetic operator has precedence over question operator, use parentheses to clarify intent.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception.  Consider using _malloca instead.
    target_compile_options(${LIBRARY_NAME} PUBLIC /MP) # Build with Multiple Processes
    target_compile_options(${LIBRARY_NAME} PUBLIC /GL) # Enables whole program optimization.
  else ()
    target_compile_options(${LIBRARY_NAME} PUBLIC /MD)
  endif ()
  # standard conformance
  target_compile_options(${LIBRARY_NAME} PUBLIC /permissive- /volatile:iso /Zc:inline /Zc:preprocessor /Zc:lambda)
  target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:externConstexpr /Zc:throwingNew /Zc:enumTypes /Zc:templateScope)
  target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:__cplusplus) # The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated value for recent C++ language standards support

  target_compile_options(${LIBRARY_NAME} PUBLIC /arch:AVX)
  target_link_options(${LIBRARY_NAME} PUBLIC /LTCG:STATUS)
else (WIN32)
  target_compile_options(${LIBRARY_NAME} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
  target_compile_options(${LIBRARY_NAME} PUBLIC -fexceptions -fwrapv -fno-strict-aliasing)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Wall -Wextra)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress -Wdeprecated)
  if (APPLE AND AARCH64)
    target_compile_options(${LIBRARY_NAME} PUBLIC -mcpu=apple-m1 -fopenmp-simd -DSIMDE_ENABLE_OPENMP)
  else ()
    target_compile_options(${LIBRARY_NAME} PUBLIC -mavx -fopenmp-simd -DSIMDE_ENABLE_OPENMP)
  endif ()
  target_compile_options(${LIBRARY_NAME} PUBLIC -fno-common)

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wbool-conversion -Wconstant-conversion -Wenum-conversion)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress-of-temporary)
    target_compile_options(${LIBRARY_NAME} PRIVATE -Wundefined-reinterpret-cast)
    target_compile_options(${LIBRARY_NAME} PUBLIC -mllvm -inline-threshold=600)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wshadow)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wno-c++98-c++11-c++14-c++17-compat)

    set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)

    if (APPLE)
      target_compile_options(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
      target_link_options(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
    else ()
      target_compile_options(${LIBRARY_NAME} PUBLIC -fsized-deallocation)
    endif ()
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-but-set-variable")
  endif ()

  if (ATLAS_SANITIZE_ADDRESS)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
      target_link_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
    else ()
      target_compile_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
      target_link_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
    endif ()
  endif ()
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC neurolabi Qt6::Core)
