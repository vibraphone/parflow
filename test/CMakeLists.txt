
include(ParflowTest)

set(TESTS
  default_single.tcl
  default_richards_wells.tcl
  octree-simple.tcl
  octree-large-domain.tcl
  forsyth2.tcl
  harvey.flow.tcl
  harvey_flow_pgs.tcl
  crater2D.tcl
  crater2D_vangtable_spline.tcl
  crater2D_vangtable_linear.tcl
  small_domain.tcl
  richards_hydrostatic_equalibrium.tcl
)

if(${PARFLOW_HAVE_HYPRE})
  list(APPEND TESTS
    default_richards.tcl
    default_richards_nocluster.tcl
    default_overland.tcl
    LW_var_dz.tcl
    LW_var_dz_spinup.tcl
    terrain_following_grid_overland.tcl
    var_dz_1D.tcl
    pfmg.tcl
    pfmg_galerkin.tcl
    smg.tcl
    pfmg_octree.tcl
    van-genuchten-file.tcl
    overland_slopingslab_KWE.tcl
    overland_tiltedV_KWE.tcl
    overland_slopingslab_DWE.tcl
    overland_tiltedV_DWE.tcl
    overland_FlatICP.tcl)
endif()

if(${PARFLOW_HAVE_SILO})
  list(APPEND TESTS
    indicator_field.tcl)

if(${PARFLOW_HAVE_HYPRE})
  list(APPEND TESTS
	water_balance_y.tcl
	water_balance_x.tcl
	default_richards_with_silo.tcl)
endif()
endif()

if(${PARFLOW_HAVE_NETCDF})
  if(${PARFLOW_HAVE_HYPRE})
    #This test is failing on several platforms
    #list(APPEND TESTS
    #	default_richards_with_netcdf.tcl)
  endif()
endif()

set(SAMRAI_TESTS)
set(SAMRAI_TESTS_WITH_PATCH_COUNT)

if(${PARFLOW_HAVE_SAMRAI})
  list(APPEND SAMRAI_TESTS
    samrai_compute_domain.tcl)

  list(APPEND SAMRAI_TESTS_WITH_PATCH_COUNT
    samrai.tcl)
endif()

list(APPEND PARALLEL_3DTOPO_TESTS "")
list(APPEND PARALLEL_2DTOPO_TESTS "")

if(${PARFLOW_AMPS_LAYER} STREQUAL "mpi1")
  list(APPEND PARALLEL_3DTOPO_TESTS
    default_single.tcl)

  if(${PARFLOW_HAVE_HYPRE})
    list(APPEND PARALLEL_3DTOPO_TESTS
      default_richards.tcl)

    list(APPEND PARALLEL_2DTOPO_TESTS
      default_overland.tcl
      default_overland.pfmg.jac.tcl
      default_overland.pfmg_octree.jac.tcl
      default_overland.pfmg_octree.fulljac.tcl
      water_balance_x.hardflow.nojac.tcl
      water_balance_x.hardflow.jac.tcl
      LW_var_dz.tcl
      LW_var_dz_spinup.tcl)
  endif()

endif()

foreach(inputfile ${TESTS})
  pf_add_sequential_test(${inputfile})
endforeach()

foreach(inputfile ${PARALLEL_3DTOPO_TESTS})
  foreach(processor_topology "1 1 2" "1 2 1" "2 1 1" "2 2 2" "3 3 3" "1 1 4" "1 4 1" "4 1 1")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()
endforeach()

foreach(inputfile ${PARALLEL_2DTOPO_TESTS})
  foreach(processor_topology "1 2 1" "2 1 1" "1 4 1" "4 1 1")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()
endforeach()

foreach(inputfile ${SAMRAI_TESTS})
  foreach(processor_topology "1 1 1" "1 2 1" "2 1 1" "2 2 1" "3 3 1" "1 4 1" "4 1 1")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()

  foreach(processor_topology "2 2 1" "2 2 1" "2 2 1" "2 2 1" "2 2 1" "2 2 1" "2 2 1")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()
endforeach()

foreach(inputfile ${SAMRAI_TESTS_WITH_PATCH_COUNT})
  foreach(processor_topology "1 1 1 1" "1 1 1 2" "1 1 1 3" "2 1 1 2" "1 2 1 2" "1 1 2 2" "2 1 1 3" "1 2 1 3" "1 1 2 3" "2 1 1 4" "1 2 1 4" "1 1 2 4")
    pf_add_parallel_test(${inputfile} ${processor_topology})
  endforeach()
endforeach()

if (${PARFLOW_HAVE_CLM})
  add_subdirectory (clm)
  add_subdirectory (washita/tcl_scripts)
endif(${PARFLOW_HAVE_CLM})
