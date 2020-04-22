# Get all propreties that cmake supports
execute_process(COMMAND cmake --help-property-list OUTPUT_VARIABLE CMAKE_PROPERTY_LIST)

# Convert command output into a CMake list
STRING(REGEX REPLACE ";" "\\\\;" CMAKE_PROPERTY_LIST "${CMAKE_PROPERTY_LIST}")
STRING(REGEX REPLACE "\n" ";" CMAKE_PROPERTY_LIST "${CMAKE_PROPERTY_LIST}")
# Fix https://stackoverflow.com/questions/32197663/how-can-i-remove-the-the-location-property-may-not-be-read-from-target-error-i
# list(FILTER CMAKE_PROPERTY_LIST EXCLUDE REGEX "^LOCATION$|^LOCATION_|_LOCATION$")
list(FILTER CMAKE_PROPERTY_LIST EXCLUDE REGEX "^LOCATION$|^LOCATION_")
# For some reason, "TYPE" shows up twice - others might too?
list(REMOVE_DUPLICATES CMAKE_PROPERTY_LIST)

# build whitelist by filtering down from CMAKE_PROPERTY_LIST in case cmake is
# a different version, and one of our hardcoded whitelisted properties
# doesn't exist!
unset(CMAKE_WHITELISTED_PROPERTY_LIST)
foreach(prop ${CMAKE_PROPERTY_LIST})
    if(prop MATCHES "^(INTERFACE|[_a-z]|IMPORTED_LIBNAME_|MAP_IMPORTED_CONFIG_)|^(COMPATIBLE_INTERFACE_(BOOL|NUMBER_MAX|NUMBER_MIN|STRING)|EXPORT_NAME|IMPORTED(_GLOBAL|_CONFIGURATIONS|_LIBNAME)?|NAME|TYPE|NO_SYSTEM_FROM_IMPORTED)$")
        list(APPEND CMAKE_WHITELISTED_PROPERTY_LIST ${prop})
    endif()
endforeach(prop)

function(print_properties)
    message (STATUS "CMAKE_PROPERTY_LIST = ${CMAKE_PROPERTY_LIST}")
endfunction(print_properties)

function(print_whitelisted_properties)
    message (STATUS "CMAKE_WHITELISTED_PROPERTY_LIST = ${CMAKE_WHITELISTED_PROPERTY_LIST}")
endfunction(print_whitelisted_properties)

function(print_target_properties tgt)
    if(NOT TARGET ${tgt})
      message(WARNING "There is no target named '${tgt}'")
      return()
    endif()

    set(options NO_LOCATION)
    cmake_parse_arguments(PARSE_ARGV 1 print_target_properties "${options}" "" "")
    # message (STATUS "print_target_properties_NO_LOCATION = ${print_target_properties_NO_LOCATION}")

    get_target_property(target_type ${tgt} TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(PROP_LIST ${CMAKE_WHITELISTED_PROPERTY_LIST})
    else()
        set(PROP_LIST ${CMAKE_PROPERTY_LIST})
    endif()

    if (print_target_properties_NO_LOCATION)
        list(FILTER PROP_LIST EXCLUDE REGEX "_LOCATION$")
    endif ()

    string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPERCASE)

    foreach (prop ${PROP_LIST})
        string(REPLACE "<CONFIG>" "${CMAKE_BUILD_TYPE_UPPERCASE}" prop ${prop})
        # message (STATUS "Checking ${prop}")
        get_property(propval TARGET ${tgt} PROPERTY ${prop} SET)
        if (propval)
            get_target_property(propval ${tgt} ${prop})
            message (STATUS "${tgt} ${prop} = ${propval}")
        endif()
    endforeach(prop)
endfunction(print_target_properties)
