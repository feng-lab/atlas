include_guard(GLOBAL)

function(_atlas_host_sdk_json_escape input output_var)
  set(_escaped "${input}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  set(${output_var} "${_escaped}" PARENT_SCOPE)
endfunction()

function(_atlas_host_sdk_home_dir output_var)
  if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{HOME}" _atlas_home)
  elseif(WIN32 AND DEFINED ENV{USERPROFILE} AND NOT "$ENV{USERPROFILE}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _atlas_home)
  elseif(WIN32 AND DEFINED ENV{HOMEDRIVE} AND DEFINED ENV{HOMEPATH}
         AND NOT "$ENV{HOMEDRIVE}$ENV{HOMEPATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{HOMEDRIVE}$ENV{HOMEPATH}" _atlas_home)
  else()
    message(FATAL_ERROR "Could not determine the current user's home directory for Atlas host SDK resolution.")
  endif()
  set(${output_var} "${_atlas_home}" PARENT_SCOPE)
endfunction()

function(_atlas_host_sdk_select_version root_dir marker_suffix label output_var)
  if(NOT EXISTS "${root_dir}")
    message(FATAL_ERROR "${label} root not found: ${root_dir}")
  endif()

  file(GLOB _atlas_entries LIST_DIRECTORIES true "${root_dir}/*")
  set(_atlas_versions)
  foreach(_atlas_entry IN LISTS _atlas_entries)
    if(EXISTS "${_atlas_entry}/${marker_suffix}")
      get_filename_component(_atlas_name "${_atlas_entry}" NAME)
      list(APPEND _atlas_versions "${_atlas_name}")
    endif()
  endforeach()

  list(LENGTH _atlas_versions _atlas_version_count)
  if(_atlas_version_count EQUAL 0)
    message(FATAL_ERROR
            "No valid ${label} versions were found under '${root_dir}'. "
            "Expected marker: '${marker_suffix}'.")
  endif()

  list(SORT _atlas_versions COMPARE NATURAL ORDER ASCENDING)
  list(GET _atlas_versions -1 _atlas_selected)
  set(${output_var} "${_atlas_selected}" PARENT_SCOPE)
endfunction()

function(atlas_resolve_host_sdks)
  _atlas_host_sdk_home_dir(_atlas_home_dir)

  if(WIN32)
    set(_atlas_qt_install_dir "C:/Qt")
    if(NOT EXISTS "${_atlas_qt_install_dir}")
      set(_atlas_qt_install_dir "${_atlas_home_dir}/Qt")
    endif()
    set(_atlas_qt_compiler_name "msvc2022_64")
    set(_atlas_qmake_name "qmake.exe")
    set(_atlas_vulkan_sdk_root "C:/VulkanSDK")
    if(NOT EXISTS "${_atlas_vulkan_sdk_root}")
      set(_atlas_vulkan_sdk_root "${_atlas_home_dir}/VulkanSDK")
    endif()
    set(_atlas_vulkan_sdk_bin_suffix "Bin")
    set(_atlas_vulkan_sdk_env_suffix "")
  elseif(APPLE)
    set(_atlas_qt_install_dir "${_atlas_home_dir}/Qt")
    set(_atlas_qt_compiler_name "macos")
    set(_atlas_qmake_name "qmake")
    set(_atlas_vulkan_sdk_root "${_atlas_home_dir}/VulkanSDK")
    set(_atlas_vulkan_sdk_bin_suffix "macOS/bin")
    set(_atlas_vulkan_sdk_env_suffix "macOS")
  else()
    set(_atlas_qt_install_dir "${_atlas_home_dir}/Qt")
    set(_atlas_qt_compiler_name "gcc_64")
    set(_atlas_qmake_name "qmake")
    set(_atlas_vulkan_sdk_root "${_atlas_home_dir}/VulkanSDK")
    set(_atlas_vulkan_sdk_bin_suffix "x86_64/bin")
    set(_atlas_vulkan_sdk_env_suffix "x86_64")
  endif()

  _atlas_host_sdk_select_version(
    "${_atlas_qt_install_dir}"
    "${_atlas_qt_compiler_name}/bin/${_atlas_qmake_name}"
    "Qt"
    _atlas_qt_version)
  set(_atlas_qt_host_path "${_atlas_qt_install_dir}/${_atlas_qt_version}/${_atlas_qt_compiler_name}")

  set(_atlas_qtifw_root "${_atlas_qt_install_dir}/Tools/QtInstallerFramework")
  _atlas_host_sdk_select_version(
    "${_atlas_qtifw_root}"
    "bin"
    "Qt Installer Framework"
    _atlas_qtifw_version)
  set(_atlas_qtifw_bin_dir "${_atlas_qtifw_root}/${_atlas_qtifw_version}/bin")

  _atlas_host_sdk_select_version(
    "${_atlas_vulkan_sdk_root}"
    "${_atlas_vulkan_sdk_bin_suffix}"
    "Vulkan SDK"
    _atlas_vulkan_sdk_version)
  if(_atlas_vulkan_sdk_env_suffix STREQUAL "")
    set(_atlas_vulkan_sdk "${_atlas_vulkan_sdk_root}/${_atlas_vulkan_sdk_version}")
  else()
    set(_atlas_vulkan_sdk "${_atlas_vulkan_sdk_root}/${_atlas_vulkan_sdk_version}/${_atlas_vulkan_sdk_env_suffix}")
  endif()
  set(_atlas_vulkan_sdk_bin_dir "${_atlas_vulkan_sdk_root}/${_atlas_vulkan_sdk_version}/${_atlas_vulkan_sdk_bin_suffix}")

  set(ATLAS_QT_INSTALL_DIR "${_atlas_qt_install_dir}" PARENT_SCOPE)
  set(ATLAS_QT_VERSION "${_atlas_qt_version}" PARENT_SCOPE)
  set(ATLAS_QT_HOST_PATH "${_atlas_qt_host_path}" PARENT_SCOPE)
  set(ATLAS_QTIFW_BIN_DIR "${_atlas_qtifw_bin_dir}" PARENT_SCOPE)
  set(ATLAS_VULKAN_SDK_ROOT "${_atlas_vulkan_sdk_root}" PARENT_SCOPE)
  set(ATLAS_VULKAN_SDK_VERSION "${_atlas_vulkan_sdk_version}" PARENT_SCOPE)
  set(ATLAS_VULKAN_SDK "${_atlas_vulkan_sdk}" PARENT_SCOPE)
  set(ATLAS_VULKAN_SDK_BIN_DIR "${_atlas_vulkan_sdk_bin_dir}" PARENT_SCOPE)

  # Compatibility variables consumed by existing Atlas CMakeLists.
  set(QT_VERSION "${_atlas_qt_version}" PARENT_SCOPE)
  set(QT_HOST_PATH "${_atlas_qt_host_path}" PARENT_SCOPE)
endfunction()

function(atlas_write_host_sdks_json output_json)
  atlas_resolve_host_sdks()

  _atlas_host_sdk_json_escape("${ATLAS_QT_HOST_PATH}" _atlas_qt_host_path_json)
  _atlas_host_sdk_json_escape("${ATLAS_QT_VERSION}" _atlas_qt_version_json)
  _atlas_host_sdk_json_escape("${ATLAS_QTIFW_BIN_DIR}" _atlas_qtifw_bin_dir_json)
  _atlas_host_sdk_json_escape("${ATLAS_VULKAN_SDK_ROOT}" _atlas_vulkan_sdk_root_json)
  _atlas_host_sdk_json_escape("${ATLAS_VULKAN_SDK_VERSION}" _atlas_vulkan_sdk_version_json)
  _atlas_host_sdk_json_escape("${ATLAS_VULKAN_SDK}" _atlas_vulkan_sdk_json)
  _atlas_host_sdk_json_escape("${ATLAS_VULKAN_SDK_BIN_DIR}" _atlas_vulkan_sdk_bin_dir_json)

  file(WRITE "${output_json}"
       "{\n"
       "  \"qt_host_path\": \"${_atlas_qt_host_path_json}\",\n"
       "  \"qt_version\": \"${_atlas_qt_version_json}\",\n"
       "  \"qtifw_bin_dir\": \"${_atlas_qtifw_bin_dir_json}\",\n"
       "  \"vulkan_sdk_root\": \"${_atlas_vulkan_sdk_root_json}\",\n"
       "  \"vulkan_sdk_version\": \"${_atlas_vulkan_sdk_version_json}\",\n"
       "  \"vulkan_sdk\": \"${_atlas_vulkan_sdk_json}\",\n"
       "  \"vulkan_sdk_bin_dir\": \"${_atlas_vulkan_sdk_bin_dir_json}\"\n"
       "}\n")
endfunction()

if(DEFINED CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEFINED ATLAS_HOST_SDK_OUTPUT_JSON OR "${ATLAS_HOST_SDK_OUTPUT_JSON}" STREQUAL "")
    message(FATAL_ERROR "ATLAS_HOST_SDK_OUTPUT_JSON must be set to a writable file path.")
  endif()
  atlas_write_host_sdks_json("${ATLAS_HOST_SDK_OUTPUT_JSON}")
endif()
