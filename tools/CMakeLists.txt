#------------------------------------------------------------------------------
# Include source and build directories
#------------------------------------------------------------------------------
include_directories(${CMAKE_SOURCE_DIR}/include)


#------------------------------------------------------------------------------
# FLAGS for building
#------------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-result -std=c++11")

find_package(MPI REQUIRED)
if(MPI_FOUND)
    include_directories(${MPI_CXX_INCLUDE_DIRS} ${MPI_C_INCLUDE_DIRS})
else()
    message(STATUS, "MPI not found")
endif()

find_package(ZLIB REQUIRED)
if(ZLIB_FOUND)
    include_directories(${ZLIB_INCLUDE_DIRS})
else()
    message(STATUS, "ZLIB not found")
endif()

#------------------------------------------------------------------------------
# Tools
#------------------------------------------------------------------------------

add_library(reader reader.c reader-cst-cfg.c)
target_link_libraries(reader
                        PUBLIC ${ZLIB_LIBRARIES}
                    )

add_executable(recorder2text recorder2text.c)
target_link_libraries(recorder2text
                        PUBLIC ${MPI_C_LIBRARIES}
                        PUBLIC m
                        reader
                     )
add_dependencies(recorder2text reader)


add_executable(recorder2timeline recorder2timeline.cpp)
target_link_libraries(recorder2timeline
        PUBLIC ${MPI_CXX_LIBRARIES} reader)
add_dependencies(recorder2timeline reader)


#add_executable(metaops-checker meta-ops-checker.c)
#target_link_libraries(metaops-checker reader)
#add_dependencies(metaops-checker reader)

add_executable(conflict-detector conflict-detector.cpp build-offset-intervals.cpp)
target_link_libraries(conflict-detector reader)
add_dependencies(conflict-detector reader)

add_executable(recorder-summary recorder-summary.c)
target_link_libraries(recorder-summary reader)
add_dependencies(recorder-summary reader)


if(RECORDER_ENABLE_PARQUET)
    message("-- " "Configuring Parquet tool: TRUE")
    find_package(Arrow REQUIRED)
    if(${Arrow_FOUND})
        message("-- " "Found Arrow: TRUE")
        include_directories(${ARROW_INCLUDE_DIR})
    else()
        message("-- " "Found Arrow: FALSE")
    endif()
    find_package(Parquet REQUIRED)
    if(${Parquet_FOUND})
        message("-- " "Found Parquet: TRUE")
        include_directories(${PARQUET_INCLUDE_DIR})
    else()
        message("-- " "Found Parquet: FALSE")
    endif()

    add_executable(recorder2parquet recorder2parquet.cpp)
    target_link_libraries(recorder2parquet
                          PUBLIC ${MPI_C_LIBRARIES}
                          reader
                          arrow_shared
                          parquet_shared
                         )
    add_dependencies(recorder2parquet reader)
    install(
        TARGETS
        recorder2parquet 
        EXPORT
        ${RECORDER_EXPORTED_TARGETS}
        LIBRARY DESTINATION ${RECORDER_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${RECORDER_INSTALL_LIB_DIR}
        RUNTIME DESTINATION ${RECORDER_INSTALL_BIN_DIR}
    )
endif()



#-----------------------------------------------------------------------------
# Add Target(s) to CMake Install
#-----------------------------------------------------------------------------
#set(targets reader recorder2text metaops_checker conflict_detector)
set(targets reader recorder2text recorder2timeline conflict-detector recorder-summary)
foreach(target ${targets})
    install(
        TARGETS
        ${target} 
        EXPORT
        ${RECORDER_EXPORTED_TARGETS}
        LIBRARY DESTINATION ${RECORDER_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${RECORDER_INSTALL_LIB_DIR}
        RUNTIME DESTINATION ${RECORDER_INSTALL_BIN_DIR}
    )
endforeach()

