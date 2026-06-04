cmake_minimum_required(VERSION 3.6.1)
project(imgmatlab)

# build matlab mex
find_package(Matlab COMPONENTS MX_LIBRARY MEX_COMPILER)
if (Matlab_FOUND AND ${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.14.0")
  message(STATUS "Matlab_ROOT_DIR: ${Matlab_ROOT_DIR}")
  message(STATUS "Matlab_INCLUDE_DIRS: ${Matlab_INCLUDE_DIRS}")
  message(STATUS "Matlab_MEX_LIBRARY: ${Matlab_MEX_LIBRARY}")
  message(STATUS "Matlab_MX_LIBRARY: ${Matlab_MX_LIBRARY}")
  message(STATUS "Matlab_LIBRARIES: ${Matlab_LIBRARIES}")
  message(STATUS "Matlab_MEX_COMPILER: ${Matlab_MEX_COMPILER}")

  if (APPLE)
    function(add_mex_target mexname)
      matlab_add_mex(
        NAME ${mexname}
        SRC ${CMAKE_CURRENT_SOURCE_DIR}/${mexname}.cpp
        LINK_TO img
        R2018a
      )
      add_custom_command(TARGET ${mexname}
                         POST_BUILD
                         COMMAND install_name_tool -change
                         @rpath/QtCore.framework/Versions/5/QtCore
                         @loader_path/QtCore
                         $<TARGET_FILE:${mexname}>
                         COMMAND install_name_tool -change
                         @rpath/libtbb.dylib
                         @loader_path/libtbb.dylib
                         $<TARGET_FILE:${mexname}>
                         COMMAND ${CMAKE_STRIP} -x $<TARGET_FILE:${mexname}>
                         )
    endfunction(add_mex_target)

    add_mex_target(correct_czi_shading_mex)
    add_mex_target(read_stack_info_mex)
    add_mex_target(read_stack_mex)
    add_mex_target(stack_czi_tiles_mex)
    add_mex_target(stack_czi_tiles_with_mask_mex)
    add_mex_target(read_puncta_mex)

    add_custom_command(TARGET correct_czi_shading_mex
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:TBB::tbb>
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Qt5::Core>
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND install_name_tool -id QtCore
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>/QtCore
                       COMMAND install_name_tool -id libtbb.dylib
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>/libtbb.dylib
                       )
  endif (APPLE)

  if(UNIX AND NOT APPLE)
    # for Linux, BSD, Solaris, Minix
    function(add_mex_target mexname)
      matlab_add_mex(
        NAME ${mexname}
        SRC ${CMAKE_CURRENT_SOURCE_DIR}/${mexname}.cpp
        LINK_TO img
        R2018a
      )
      add_custom_command(TARGET ${mexname}
                         POST_BUILD
                         COMMAND patchelf --set-rpath '$$ORIGIN' --force-rpath $<TARGET_FILE:${mexname}>
                         COMMAND ${CMAKE_STRIP} --discard-all $<TARGET_FILE:${mexname}>)
    endfunction(add_mex_target)

    add_mex_target(correct_czi_shading_mex)
    add_mex_target(read_stack_info_mex)
    add_mex_target(read_stack_mex)
    add_mex_target(stack_czi_tiles_mex)
    add_mex_target(stack_czi_tiles_with_mask_mex)
    add_mex_target(read_puncta_mex)

    add_custom_command(TARGET correct_czi_shading_mex
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:TBB::tbb>
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Qt5::Core>
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>/libQt5Core.so.5
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:Qt5::Core>/libicui18n.so.56
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:Qt5::Core>/libicuuc.so.56
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:Qt5::Core>/libicudata.so.56
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       )
  endif()
endif()
