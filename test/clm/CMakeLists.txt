
add_test (NAME clm COMMAND tclsh clm.tcl 1 1 1 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

set(TESTS "")
if(${PARFLOW_HAVE_CLM})
  if(${PARFLOW_HAVE_HYPRE})
    list(APPEND TESTS
      clm.tcl
      clm_forc_veg.tcl
      clm_varDZ.tcl)
  endif()
endif()

set(APPEND PARALLEL_TESTS)

if(${PARFLOW_AMPS_LAYER} STREQUAL "mpi1")
  if(${PARFLOW_HAVE_HYPRE})
    list(APPEND PARALLEL_TESTS
      clm.tcl
      clm.jac.tcl
      clm_forc_veg.tcl
      clm_varDZ.tcl)
  endif()
endif()

set(SAMRAI_TESTS)

if(${PARFLOW_HAVE_CLM})
  if(${PARFLOW_HAVE_SAMRAI})
    if(${PARFLOW_HAVE_HYPRE})
      list(APPEND SAMRAI_TESTS
	clm_samrai.tcl)
    endif()
  endif()
endif()

foreach(inputfile ${TESTS})
  pf_add_sequential_test(${inputfile})
endforeach()

foreach(inputfile ${PARALLEL_TESTS})
  foreach(processor_topology "1 2 1" "2 1 1" "2 2 1" "3 3 1" "1 4 1" "4 1 1")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()
endforeach()

foreach(inputfile ${SAMRAI_TESTS})
  foreach(processor_topology "1 1 1 1" "2 1 1 1")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()
endforeach()

add_subdirectory (clm-reuse)


