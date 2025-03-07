/*BHEADER*********************************************************************
 *
 *  Copyright (c) 1995-2009, Lawrence Livermore National Security,
 *  LLC. Produced at the Lawrence Livermore National Laboratory. Written
 *  by the Parflow Team (see the CONTRIBUTORS file)
 *  <parflow@lists.llnl.gov> CODE-OCEC-08-103. All rights reserved.
 *
 *  This file is part of Parflow. For details, see
 *  http://www.llnl.gov/casc/parflow
 *
 *  Please read the COPYRIGHT file or Our Notice and the LICENSE file
 *  for the GNU Lesser General Public License.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License (as published
 *  by the Free Software Foundation) version 2.1 dated February 1999.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms
 *  and conditions of the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 *  USA
 **********************************************************************EHEADER*/

/*
 * SGS TODO this needs some work in the overland flow current
 * implemnetation is doing communication and computations that are not
 * needed.  The C matrix is wasting a lot of space and communication.
 * Making it a set of vectors will save a great deal of space or adding
 * 2D Matrix support.
 *
 * SGS TODO There is a problem attempting to avoid doing the overland
 * flow additions since the flag is not local.  A neighbor doing
 * overland flow means the process does as well if the overland flow
 * cell is on the boundary.
 */

#include "parflow.h"
#include "llnlmath.h"
#include "llnltyps.h"
#include "assert.h"

/*---------------------------------------------------------------------
 * Define module structures
 *---------------------------------------------------------------------*/

// Which Jacobian to use.
//
enum JacobianType {
  no_nonlinear_jacobian,
  not_set,
  simple,
  overland_flow 
};

typedef struct {
  enum JacobianType type;
  double SpinupDampP1; // NBE
  double SpinupDampP2; // NBE
  int tfgupwind;  // @RMM
} PublicXtra;

typedef struct {
  Problem      *problem;

  PFModule     *density_module;
  PFModule     *saturation_module;
  PFModule     *rel_perm_module;
  PFModule     *bc_pressure;
  PFModule     *bc_internal;
  PFModule     *overlandflow_module;  //DOK
  PFModule     *overlandflow_module_diff;  //@LEC
  PFModule     *overlandflow_module_kin;

  /* The analytic Jacobian matrix is decomposed as follows:
   *
   *      [ JC  JE ]
   * J =  |        |
   *      [ JF  JB ]
   *
   * where JC corresponds to surface-surface interations,
   *      JB corresponds to subsurface-subsurface interations,
   *      JE corresponds to surface-subsurface interations, and
   *      JF corresponds to subsurface-surface interations.
   *
   * To make for a more efficient implementation, we store the
   * interactions for JE and JF as part of JB, so that JC handles
   * only surface-surface interactions, and JB handles the rest
   *
   * To make this more general, JB = J whenever there is no
   * overland flow contributions to the Jacobian. Hence the
   * analytic Jacobian for the subsurface flow is invoked instead.
   */
  Matrix       *J;
  Matrix       *JC;

  Grid         *grid;
  double       *temp_data;
} InstanceXtra;

/*--------------------------------------------------------------------------
 * Static stencil-shape definition
 *--------------------------------------------------------------------------*/

int jacobian_stencil_shape[7][3] = { { 0, 0, 0 },
                                     { -1, 0, 0 },
                                     { 1, 0, 0 },
                                     { 0, -1, 0 },
                                     { 0, 1, 0 },
                                     { 0, 0, -1 },
                                     { 0, 0, 1 } };


int jacobian_stencil_shape_C[5][3] = { { 0, 0, 0 },
                                       { -1, 0, 0 },
                                       { 1, 0, 0 },
                                       { 0, -1, 0 },
                                       { 0, 1, 0 } };

/*---------------------------------------------------------------------
 * Define macros for jacobian evaluation
 *---------------------------------------------------------------------*/

#define PMean(a, b, c, d)    HarmonicMean(c, d)
#define PMeanDZ(a, b, c, d)     HarmonicMeanDZ(a, b, c, d)
#define RPMean(a, b, c, d)   UpstreamMean(a, b, c, d)
#define Mean(a, b) ArithmeticMean(a, b)  //@RMM

/*  This routine provides the interface between KINSOL and ParFlow
 *  for richards' equation jacobian evaluations and matrix-vector multiplies.*/

int       KINSolMatVec(
                       void *   current_state,
                       N_Vector x,
                       N_Vector y,
                       int *    recompute,
                       N_Vector pressure)
{
  PFModule    *richards_jacobian_eval = StateJacEval(((State*)current_state));
  Matrix      *J = StateJac(((State*)current_state));
  Matrix      *JC = StateJacC(((State*)current_state));
  Vector      *old_pressure = StateOldPressure(((State*)current_state));
  Vector      *saturation = StateSaturation(((State*)current_state));
  Vector      *density = StateDensity(((State*)current_state));
  ProblemData *problem_data = StateProblemData(((State*)current_state));
  double dt = StateDt(((State*)current_state));
  double time = StateTime(((State*)current_state));

  InstanceXtra  *instance_xtra = (InstanceXtra*)PFModuleInstanceXtra(richards_jacobian_eval);

  PFModule    *bc_pressure = (instance_xtra->bc_pressure);

  StateBCPressure((State*)current_state) = bc_pressure;

  InitVector(y, 0.0);

  /*
   * Compute Jacobian if needed.
   */
  if (*recompute)
  {
    PFModuleInvokeType(RichardsJacobianEvalInvoke, richards_jacobian_eval,
                       (pressure, old_pressure, &J, &JC, saturation, density, problem_data,
                        dt, time, 0));

    *recompute = 0;
    StateJac(((State*)current_state)) = J;
    StateJacC(((State*)current_state)) = JC;
  }

  if (JC == NULL)
    Matvec(1.0, J, x, 0.0, y);
  else
    MatvecSubMat(current_state, 1.0, J, JC, x, 0.0, y);

  return(0);
}


/*  This routine evaluates the Richards jacobian based on the current
 *  pressure values.  */

void    RichardsJacobianEval(
                             Vector *     pressure, /* Current pressure values */
			     Vector *     old_pressure, /* Pressure values at previous timestep */
                             Matrix **    ptr_to_J, /* Pointer to the J pointer - this will be set
                                                     * to instance_xtra pointer at end */
                             Matrix **    ptr_to_JC, /* Pointer to the JC pointer - this will be set
                                                      * to instance_xtra pointer at end */
                             Vector *     saturation, /* Saturation / work vector */
                             Vector *     density, /* Density vector */
                             ProblemData *problem_data, /* Geometry data for problem */
                             double       dt, /* Time step size */
                             double       time, /* New time value */
                             int          symm_part) /* Specifies whether to compute just the
                                                      * symmetric part of the Jacobian (1), or the
                                                      * full Jacobian */
{
  PFModule      *this_module = ThisPFModule;
  InstanceXtra  *instance_xtra = (InstanceXtra*)PFModuleInstanceXtra(this_module);
  PublicXtra    *public_xtra = (PublicXtra*)PFModulePublicXtra(this_module);

  Problem     *problem = (instance_xtra->problem);

  PFModule    *density_module = (instance_xtra->density_module);
  PFModule    *saturation_module = (instance_xtra->saturation_module);
  PFModule    *rel_perm_module = (instance_xtra->rel_perm_module);
  PFModule    *bc_pressure = (instance_xtra->bc_pressure);
  PFModule    *bc_internal = (instance_xtra->bc_internal);
  PFModule    *overlandflow_module = (instance_xtra->overlandflow_module);
  PFModule    *overlandflow_module_diff = (instance_xtra->overlandflow_module_diff);
  PFModule    *overlandflow_module_kin = (instance_xtra->overlandflow_module_kin);

  Matrix      *J = (instance_xtra->J);
  Matrix      *JC = (instance_xtra->JC);

  Vector      *density_der = NULL;
  Vector      *saturation_der = NULL;

  /* Re-use vectors to save memory */
  Vector      *rel_perm = NULL;
  Vector      *rel_perm_der = NULL;

  Vector      *porosity = ProblemDataPorosity(problem_data);
  Vector      *permeability_x = ProblemDataPermeabilityX(problem_data);
  Vector      *permeability_y = ProblemDataPermeabilityY(problem_data);
  Vector      *permeability_z = ProblemDataPermeabilityZ(problem_data);
  Vector      *sstorage = ProblemDataSpecificStorage(problem_data);           //sk
  Vector      *top = ProblemDataIndexOfDomainTop(problem_data);               //DOK
  Vector      *slope_x = ProblemDataTSlopeX(problem_data);                //DOK

  /* Overland flow variables */  //DOK
  Vector      *KW, *KE, *KN, *KS, *KWns, *KEns, *KNns, *KSns;
  Subvector   *kw_sub, *ke_sub, *kn_sub, *ks_sub, *kwns_sub, *kens_sub, *knns_sub, *ksns_sub, *top_sub, *sx_sub;
  double      *kw_der, *ke_der, *kn_der, *ks_der, *kwns_der, *kens_der, *knns_der, *ksns_der;

  double gravity = ProblemGravity(problem);
  double viscosity = ProblemPhaseViscosity(problem, 0);

  /* @RMM terrain following grid slope variables */
  Vector      *x_ssl = ProblemDataSSlopeX(problem_data);               //@RMM
  Vector      *y_ssl = ProblemDataSSlopeY(problem_data);               //@RMM
  Subvector   *x_ssl_sub, *y_ssl_sub;    //@RMM
  double      *x_ssl_dat, *y_ssl_dat;     //@RMM

  /* @RMM variable dz multiplier */
  Vector      *z_mult = ProblemDataZmult(problem_data);              //@RMM
  Subvector   *z_mult_sub;    //@RMM
  double      *z_mult_dat;    //@RMM

  Subgrid     *subgrid;

  Subvector   *p_sub, *d_sub, *s_sub, *po_sub, *rp_sub, *ss_sub;
  Subvector   *permx_sub, *permy_sub, *permz_sub, *dd_sub, *sd_sub, *rpd_sub;
  Submatrix   *J_sub;
  Submatrix   *JC_sub;

  Grid        *grid = VectorGrid(pressure);
  Grid        *grid2d = VectorGrid(slope_x);

  double      *pp, *sp, *sdp, *pop, *dp, *ddp, *rpp, *rpdp;
  double      *permxp, *permyp, *permzp;
  double      *cp, *wp, *ep, *sop, *np, *lp, *up, *op = NULL, *ss;

  double      *cp_c, *wp_c, *ep_c, *sop_c, *np_c, *top_dat;  //DOK

  int i, j, k, r, is;
  int ix, iy, iz;
  int nx, ny, nz;
  int nx_v, ny_v, nz_v;
  int nx_m, ny_m, nz_m;
  int nx_po, ny_po, nz_po;
  int sy_v, sz_v;
  int sy_m, sz_m;
  int ip, ipo, im, iv;


  int diffusive;             //@LEC

  diffusive = GetIntDefault("OverlandFlowDiffusive", 0);

  int overlandspinup;              //@RMM
  overlandspinup = GetIntDefault("OverlandFlowSpinUp", 0);

  int itop, k1, io, io1, ovlnd_flag;           //DOK
  int ioo;         //@RMM

  double dtmp, dx, dy, dz, vol, vol2, ffx, ffy, ffz;          //@RMM
  double diff, coeff, x_coeff, y_coeff, z_coeff, updir, sep;         //@RMM
  double prod, prod_rt, prod_no, prod_up, prod_val, prod_lo;
  double prod_der, prod_rt_der, prod_no_der, prod_up_der;
  double west_temp, east_temp, north_temp, south_temp;
  double lower_temp, upper_temp, o_temp = 0.0;
  double sym_west_temp, sym_east_temp, sym_south_temp, sym_north_temp;
  double sym_lower_temp, sym_upper_temp;
  double lower_cond, upper_cond;

  //@RMM : terms for gravity/terrain
  double x_dir_g, y_dir_g, z_dir_g, del_x_slope, del_y_slope, x_dir_g_c, y_dir_g_c;

  BCStruct    *bc_struct;
  GrGeomSolid *gr_domain = ProblemDataGrDomain(problem_data);
  double      *bc_patch_values;
  double value, den_d, dend_d;
  int         *fdir;
  int ipatch, ival;

  CommHandle  *handle;
  VectorUpdateCommHandle  *vector_update_handle;

  // Determine if an overland flow boundary condition is being used.
  // If so will use the analytic Jacobian.
  if (public_xtra->type == not_set)
  {
    // Default to simple
    public_xtra->type = simple;

    BCPressureData   *bc_pressure_data
      = ProblemDataBCPressureData(problem_data);
    int num_patches = BCPressureDataNumPatches(bc_pressure_data);

    if (num_patches > 0)
    {
      int i;
      for (i = 0; i < num_patches; i++)
      {
        int type = BCPressureDataType(bc_pressure_data, i);
        switch (type)
        {
          case 7:
          {
            public_xtra->type = overland_flow;
          }
          break;
        }
      }
    }
  }

  /*-----------------------------------------------------------------------
   * Allocate temp vectors
   *-----------------------------------------------------------------------*/
  density_der = NewVectorType(grid, 1, 1, vector_cell_centered);
  saturation_der = NewVectorType(grid, 1, 1, vector_cell_centered);

  /*-----------------------------------------------------------------------
   * reuse the temp vectors for both saturation and rel_perm calculations.
   *-----------------------------------------------------------------------*/
  rel_perm = saturation;
  rel_perm_der = saturation_der;

  /* Pass pressure values to neighbors.  */
  vector_update_handle = InitVectorUpdate(pressure, VectorUpdateAll);
  FinalizeVectorUpdate(vector_update_handle);

/* Define grid for surface contribution */
  KW = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KE = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KN = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KS = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KWns = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KEns = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KNns = NewVectorType(grid2d, 1, 1, vector_cell_centered);
  KSns = NewVectorType(grid2d, 1, 1, vector_cell_centered);

  InitVector(KW, 0.0);
  InitVector(KE, 0.0);
  InitVector(KN, 0.0);
  InitVector(KS, 0.0);
  InitVector(KWns, 0.0);
  InitVector(KEns, 0.0);
  InitVector(KNns, 0.0);
  InitVector(KSns, 0.0);

  // SGS set this to 1 since the off/on behavior does not work in
  // parallel.
  ovlnd_flag = 1;  // determines whether or not to set up data structs for overland flow contribution


  /* Initialize matrix values to zero. */
  InitMatrix(J, 0.0);
  InitMatrix(JC, 0.0);

  /* Calculate time term contributions. */

  PFModuleInvokeType(PhaseDensityInvoke, density_module, (0, pressure, density, &dtmp, &dtmp,
                                                          CALCFCN));
  PFModuleInvokeType(PhaseDensityInvoke, density_module, (0, pressure, density_der, &dtmp,
                                                          &dtmp, CALCDER));
  PFModuleInvokeType(SaturationInvoke, saturation_module, (saturation, pressure,
                                                           density, gravity, problem_data,
                                                           CALCFCN));
  PFModuleInvokeType(SaturationInvoke, saturation_module, (saturation_der, pressure,
                                                           density, gravity, problem_data,
                                                           CALCDER));

  ForSubgridI(is, GridSubgrids(grid))
  {
    subgrid = GridSubgrid(grid, is);

    J_sub = MatrixSubmatrix(J, is);
    cp = SubmatrixStencilData(J_sub, 0);

    p_sub = VectorSubvector(pressure, is);
    d_sub = VectorSubvector(density, is);
    s_sub = VectorSubvector(saturation, is);
    dd_sub = VectorSubvector(density_der, is);
    sd_sub = VectorSubvector(saturation_der, is);
    po_sub = VectorSubvector(porosity, is);
    ss_sub = VectorSubvector(sstorage, is);

    /* @RMM added to provide access to zmult */
    z_mult_sub = VectorSubvector(z_mult, is);
    /* @RMM added to provide variable dz */
    z_mult_dat = SubvectorData(z_mult_sub);
    /* @RMM added to provide access to x/y slopes */
    x_ssl_sub = VectorSubvector(x_ssl, is);
    y_ssl_sub = VectorSubvector(y_ssl, is);
    /* @RMM  added to provide slopes to terrain fns */
    x_ssl_dat = SubvectorData(x_ssl_sub);
    y_ssl_dat = SubvectorData(y_ssl_sub);

    /* RDF: assumes resolutions are the same in all 3 directions */
    r = SubgridRX(subgrid);

    ix = SubgridIX(subgrid);
    iy = SubgridIY(subgrid);
    iz = SubgridIZ(subgrid);

    nx = SubgridNX(subgrid);
    ny = SubgridNY(subgrid);
    nz = SubgridNZ(subgrid);

    dx = SubgridDX(subgrid);
    dy = SubgridDY(subgrid);
    dz = SubgridDZ(subgrid);

    vol = dx * dy * dz;

    nx_v = SubvectorNX(d_sub);
    ny_v = SubvectorNY(d_sub);
    nz_v = SubvectorNZ(d_sub);

    nx_po = SubvectorNX(po_sub);
    ny_po = SubvectorNY(po_sub);
    nz_po = SubvectorNZ(po_sub);

    nx_m = SubmatrixNX(J_sub);
    ny_m = SubmatrixNY(J_sub);
    nz_m = SubmatrixNZ(J_sub);

    pp = SubvectorData(p_sub);     //pressure
    dp = SubvectorData(d_sub);     // density
    sp = SubvectorData(s_sub);     //saturation
    ddp = SubvectorData(dd_sub);    // density derivative: del-rho / del-press
    sdp = SubvectorData(sd_sub);    // saturation derivative: del-S / del-press
    pop = SubvectorData(po_sub);     // porosity
    ss = SubvectorData(ss_sub);     // sepcific storage

    GrGeomInLoop(i, j, k, gr_domain, r, ix, iy, iz, nx, ny, nz,
    {
      im = SubmatrixEltIndex(J_sub, i, j, k);
      ipo = SubvectorEltIndex(po_sub, i, j, k);
      iv = SubvectorEltIndex(d_sub, i, j, k);
      vol2 = vol * z_mult_dat[ipo];
      cp[im] += (sdp[iv] * dp[iv] + sp[iv] * ddp[iv])
                * pop[ipo] * vol2 + ss[iv] * vol2 * (sdp[iv] * dp[iv] * pp[iv] + sp[iv] * ddp[iv] * pp[iv] + sp[iv] * dp[iv]); //sk start
    });
  }    /* End subgrid loop */

  bc_struct = PFModuleInvokeType(BCPressureInvoke, bc_pressure,
                                 (problem_data, grid, gr_domain, time));

  /* Get boundary pressure values for Dirichlet boundaries.   */
  /* These are needed for upstream weighting in mobilities - need boundary */
  /* values for rel perms and densities. */

  ForSubgridI(is, GridSubgrids(grid))
  {
    subgrid = GridSubgrid(grid, is);

    p_sub = VectorSubvector(pressure, is);

    nx_v = SubvectorNX(p_sub);
    ny_v = SubvectorNY(p_sub);
    nz_v = SubvectorNZ(p_sub);

    sy_v = nx_v;
    sz_v = ny_v * nx_v;

    pp = SubvectorData(p_sub);

    for (ipatch = 0; ipatch < BCStructNumPatches(bc_struct); ipatch++)
    {
      bc_patch_values = BCStructPatchValues(bc_struct, ipatch, is);

      switch (BCStructBCType(bc_struct, ipatch))
      {
        case DirichletBC:
        {
          BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
          {
            ip = SubvectorEltIndex(p_sub, i, j, k);
            value = bc_patch_values[ival];
            pp[ip + fdir[0] * 1 + fdir[1] * sy_v + fdir[2] * sz_v] = value;
          });
          break;
        }
      }        /* End switch BCtype */
    }          /* End ipatch loop */
  }            /* End subgrid loop */

  /* Calculate rel_perm and rel_perm_der */

  PFModuleInvokeType(PhaseRelPermInvoke, rel_perm_module,
                     (rel_perm, pressure, density, gravity, problem_data,
                      CALCFCN));

  PFModuleInvokeType(PhaseRelPermInvoke, rel_perm_module,
                     (rel_perm_der, pressure, density, gravity, problem_data,
                      CALCDER));

  /* Calculate contributions from second order derivatives and gravity */
  ForSubgridI(is, GridSubgrids(grid))
  {
    subgrid = GridSubgrid(grid, is);
    Subgrid* grid2d_subgrid = GridSubgrid(grid2d, is);
    int grid2d_iz = SubgridIZ(grid2d_subgrid);

    p_sub = VectorSubvector(pressure, is);
    d_sub = VectorSubvector(density, is);
    rp_sub = VectorSubvector(rel_perm, is);
    dd_sub = VectorSubvector(density_der, is);
    rpd_sub = VectorSubvector(rel_perm_der, is);
    permx_sub = VectorSubvector(permeability_x, is);
    permy_sub = VectorSubvector(permeability_y, is);
    permz_sub = VectorSubvector(permeability_z, is);
    J_sub = MatrixSubmatrix(J, is);

    /* @RMM added to provide access to x/y slopes */
    x_ssl_sub = VectorSubvector(x_ssl, is);
    y_ssl_sub = VectorSubvector(y_ssl, is);

    /* @RMM added to provide access to zmult */
    z_mult_sub = VectorSubvector(z_mult, is);
    /* @RMM added to provide variable dz */
    z_mult_dat = SubvectorData(z_mult_sub);

    r = SubgridRX(subgrid);

    ix = SubgridIX(subgrid) - 1;
    iy = SubgridIY(subgrid) - 1;
    iz = SubgridIZ(subgrid) - 1;

    nx = SubgridNX(subgrid) + 1;
    ny = SubgridNY(subgrid) + 1;
    nz = SubgridNZ(subgrid) + 1;

    dx = SubgridDX(subgrid);
    dy = SubgridDY(subgrid);
    dz = SubgridDZ(subgrid);

    ffx = dy * dz;
    ffy = dx * dz;
    ffz = dx * dy;

    nx_v = SubvectorNX(p_sub);
    ny_v = SubvectorNY(p_sub);
    nz_v = SubvectorNZ(p_sub);

    nx_m = SubmatrixNX(J_sub);
    ny_m = SubmatrixNY(J_sub);
    nz_m = SubmatrixNZ(J_sub);

    sy_v = nx_v;
    sz_v = ny_v * nx_v;
    sy_m = nx_m;
    sz_m = ny_m * nx_m;

    cp = SubmatrixStencilData(J_sub, 0);
    wp = SubmatrixStencilData(J_sub, 1);
    ep = SubmatrixStencilData(J_sub, 2);
    sop = SubmatrixStencilData(J_sub, 3);
    np = SubmatrixStencilData(J_sub, 4);
    lp = SubmatrixStencilData(J_sub, 5);
    up = SubmatrixStencilData(J_sub, 6);

    pp = SubvectorData(p_sub);
    dp = SubvectorData(d_sub);
    rpp = SubvectorData(rp_sub);
    ddp = SubvectorData(dd_sub);
    rpdp = SubvectorData(rpd_sub);
    permxp = SubvectorData(permx_sub);
    permyp = SubvectorData(permy_sub);
    permzp = SubvectorData(permz_sub);

    GrGeomInLoop(i, j, k, gr_domain, r, ix, iy, iz, nx, ny, nz,
    {
      ip = SubvectorEltIndex(p_sub, i, j, k);
      im = SubmatrixEltIndex(J_sub, i, j, k);
      ioo = SubvectorEltIndex(x_ssl_sub, i, j, grid2d_iz);

      prod = rpp[ip] * dp[ip];
      prod_der = rpdp[ip] * dp[ip] + rpp[ip] * ddp[ip];

      prod_rt = rpp[ip + 1] * dp[ip + 1];
      prod_rt_der = rpdp[ip + 1] * dp[ip + 1] + rpp[ip + 1] * ddp[ip + 1];

      prod_no = rpp[ip + sy_v] * dp[ip + sy_v];
      prod_no_der = rpdp[ip + sy_v] * dp[ip + sy_v]
                    + rpp[ip + sy_v] * ddp[ip + sy_v];

      prod_up = rpp[ip + sz_v] * dp[ip + sz_v];
      prod_up_der = rpdp[ip + sz_v] * dp[ip + sz_v]
                    + rpp[ip + sz_v] * ddp[ip + sz_v];

      //@RMM  tfgupwind == 0 (default) should give original behavior
      // tfgupwind 1 should still use sine but upwind
      // tfgupwdin 2 just upwind
            switch (public_xtra->tfgupwind)
            {
              case 0:
              {
                // default formulation in Maxwell 2013
            x_dir_g = Mean(gravity * sin(atan(x_ssl_dat[ioo])), gravity * sin(atan(x_ssl_dat[ioo + 1])));
            x_dir_g_c = Mean(gravity * cos(atan(x_ssl_dat[ioo])), gravity * cos(atan(x_ssl_dat[ioo + 1])));
            y_dir_g = Mean(gravity * sin(atan(y_ssl_dat[ioo])), gravity * sin(atan(y_ssl_dat[ioo + sy_v])));
            y_dir_g_c = Mean(gravity * cos(atan(y_ssl_dat[ioo])), gravity * cos(atan(y_ssl_dat[ioo + sy_v])));
            break;
          }
          case 1:
          {
            // direct upwinding, no averaging with sines
            x_dir_g =  gravity * sin(atan(x_ssl_dat[ioo]));
            x_dir_g_c = gravity * cos(atan(x_ssl_dat[ioo]));
            y_dir_g = gravity * sin(atan(y_ssl_dat[ioo]));
            y_dir_g_c = gravity * cos(atan(y_ssl_dat[ioo]));
            break;
            }
            case 2:
           {
            // direct upwinding, no averaging no sines
            x_dir_g =  x_ssl_dat[ioo];
            x_dir_g_c = 1.0;
            y_dir_g = y_ssl_dat[ioo];
            y_dir_g_c = 1.0;
            break;
            }
      }


      /* diff >= 0 implies flow goes left to right */
      diff = pp[ip] - pp[ip + 1];
      updir = (diff / dx) * x_dir_g_c - x_dir_g;

      x_coeff = dt * ffx * (1.0 / dx) * z_mult_dat[ip]
                * PMean(pp[ip], pp[ip + 1], permxp[ip], permxp[ip + 1])
                / viscosity;

      sym_west_temp = (-x_coeff
                       * RPMean(updir, 0.0, prod, prod_rt)) * x_dir_g_c; //@RMM TFG contributions, sym


      west_temp = (-x_coeff * diff
                   * RPMean(updir, 0.0, prod_der, 0.0)) * x_dir_g_c
                  + sym_west_temp;

      west_temp += (x_coeff * dx * RPMean(updir, 0.0, prod_der, 0.0)) * x_dir_g; //@RMM TFG contributions, non sym

      sym_east_temp = (-x_coeff
                       * RPMean(updir, 0.0, prod, prod_rt)) * x_dir_g_c; //@RMM added sym TFG contributions

      east_temp = (x_coeff * diff
                   * RPMean(updir, 0.0, 0.0, prod_rt_der)) * x_dir_g_c
                  + sym_east_temp;

      east_temp += -(x_coeff * dx * RPMean(updir, 0.0, 0.0, prod_rt_der)) * x_dir_g; //@RMM  TFG contributions non sym

      /* diff >= 0 implies flow goes south to north */
      diff = pp[ip] - pp[ip + sy_v];
      updir = (diff / dy) * y_dir_g_c - y_dir_g;

      y_coeff = dt * ffy * (1.0 / dy) * z_mult_dat[ip]
                * PMean(pp[ip], pp[ip + sy_v], permyp[ip], permyp[ip + sy_v])
                / viscosity;

      sym_south_temp = -y_coeff
                       * RPMean(updir, 0.0, prod, prod_no) * y_dir_g_c; //@RMM TFG contributions, SYMM

      south_temp = -y_coeff * diff
                   * RPMean(updir, 0.0, prod_der, 0.0) * y_dir_g_c
                   + sym_south_temp;

      south_temp += (y_coeff * dy * RPMean(updir, 0.0, prod_der, 0.0)) * y_dir_g; //@RMM TFG contributions, non sym


      sym_north_temp = y_coeff
                       * -RPMean(updir, 0.0, prod, prod_no) * y_dir_g_c; //@RMM  TFG contributions non SYMM

      north_temp = y_coeff * diff
                   * RPMean(updir, 0.0, 0.0,
                            prod_no_der) * y_dir_g_c
                   + sym_north_temp;

      north_temp += -(y_coeff * dy * RPMean(updir, 0.0, 0.0, prod_no_der)) * y_dir_g; //@RMM  TFG contributions non sym

      sep = (dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v]));
      /* diff >= 0 implies flow goes lower to upper */


      lower_cond = pp[ip] / sep - (z_mult_dat[ip] / (z_mult_dat[ip] + z_mult_dat[ip + sz_v])) * dp[ip] * gravity;

      upper_cond = pp[ip + sz_v] / sep + (z_mult_dat[ip + sz_v] / (z_mult_dat[ip] + z_mult_dat[ip + sz_v])) * dp[ip + sz_v] * gravity;


      diff = lower_cond - upper_cond;

      z_coeff = dt * ffz
                * PMeanDZ(permzp[ip], permzp[ip + sz_v], z_mult_dat[ip], z_mult_dat[ip + sz_v])
                / viscosity;

      sym_lower_temp = -z_coeff * (1.0 / (dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])))
                       * RPMean(lower_cond, upper_cond, prod,
                                prod_up);

      lower_temp = -z_coeff
                   * (diff * RPMean(lower_cond, upper_cond, prod_der, 0.0)
                      + (-gravity * 0.5 * dz * (Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])) * ddp[ip]
                         * RPMean(lower_cond, upper_cond, prod,
                                  prod_up)))
                   + sym_lower_temp;

      sym_upper_temp = z_coeff * (1.0 / (dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])))
                       * -RPMean(lower_cond, upper_cond, prod,
                                 prod_up);

      upper_temp = z_coeff
                   * (diff * RPMean(lower_cond, upper_cond, 0.0,
                                    prod_up_der)
                      + (-gravity * 0.5 * dz * (Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])) * ddp[ip + sz_v]
                         * RPMean(lower_cond, upper_cond, prod,
                                  prod_up)))
                   + sym_upper_temp;



      cp[im] -= west_temp + south_temp + lower_temp;
      cp[im + 1] -= east_temp;
      cp[im + sy_m] -= north_temp;
      cp[im + sz_m] -= upper_temp;

      if (!symm_part)
      {
        ep[im] += east_temp;
        np[im] += north_temp;
        up[im] += upper_temp;

        wp[im + 1] += west_temp;
        sop[im + sy_m] += south_temp;
        lp[im + sz_m] += lower_temp;
      }
      else     /* Symmetric matrix: just update upper coeffs */
      {
        ep[im] += sym_east_temp;
        np[im] += sym_north_temp;
        up[im] += sym_upper_temp;
      }
    });
  }  //

  /*  Calculate correction for boundary conditions */

  if (symm_part)
  {
    /*  For symmetric part only, we first adjust coefficients of normal */
    /*  direction boundary pressure by adding in the nonsymmetric part. */
    /*  The entire coefficicent will be subtracted from the diagonal    */
    /*  and set to zero in the subsequent section - no matter what type */
    /*  of BC is involved.  Without this correction, only the symmetric */
    /*  part would be removed, incorrectly leaving the nonsymmetric     */
    /*  contribution on the diagonal.                                   */

    ForSubgridI(is, GridSubgrids(grid))
    {
      subgrid = GridSubgrid(grid, is);

      p_sub = VectorSubvector(pressure, is);
      dd_sub = VectorSubvector(density_der, is);
      rpd_sub = VectorSubvector(rel_perm_der, is);
      d_sub = VectorSubvector(density, is);
      rp_sub = VectorSubvector(rel_perm, is);
      permx_sub = VectorSubvector(permeability_x, is);
      permy_sub = VectorSubvector(permeability_y, is);
      permz_sub = VectorSubvector(permeability_z, is);
      J_sub = MatrixSubmatrix(J, is);

      dx = SubgridDX(subgrid);
      dy = SubgridDY(subgrid);
      dz = SubgridDZ(subgrid);

      ffx = dy * dz;
      ffy = dx * dz;
      ffz = dx * dy;

      nx_v = SubvectorNX(p_sub);
      ny_v = SubvectorNY(p_sub);
      nz_v = SubvectorNZ(p_sub);

      sy_v = nx_v;
      sz_v = ny_v * nx_v;
      /* @RMM added to provide access to zmult */
      z_mult_sub = VectorSubvector(z_mult, is);
      /* @RMM added to provide variable dz */
      z_mult_dat = SubvectorData(z_mult_sub);

      cp = SubmatrixStencilData(J_sub, 0);
      wp = SubmatrixStencilData(J_sub, 1);
      ep = SubmatrixStencilData(J_sub, 2);
      sop = SubmatrixStencilData(J_sub, 3);
      np = SubmatrixStencilData(J_sub, 4);
      lp = SubmatrixStencilData(J_sub, 5);
      up = SubmatrixStencilData(J_sub, 6);

      pp = SubvectorData(p_sub);
      ddp = SubvectorData(dd_sub);
      rpdp = SubvectorData(rpd_sub);
      dp = SubvectorData(d_sub);
      rpp = SubvectorData(rp_sub);
      permxp = SubvectorData(permx_sub);
      permyp = SubvectorData(permy_sub);
      permzp = SubvectorData(permz_sub);

      for (ipatch = 0; ipatch < BCStructNumPatches(bc_struct); ipatch++)
      {
        BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
        {
          ip = SubvectorEltIndex(p_sub, i, j, k);
          im = SubmatrixEltIndex(J_sub, i, j, k);

          // SGS added this as prod was not being set to anything. Check with carol.
          prod = rpp[ip] * dp[ip];

          if (fdir[0])
          {
            switch (fdir[0])
            {
              case -1:
                {
                  diff = pp[ip - 1] - pp[ip];
                  prod_der = rpdp[ip - 1] * dp[ip - 1] + rpp[ip - 1] * ddp[ip - 1];
                  coeff = dt * z_mult_dat[ip] * ffx * (1.0 / dx)
                          * PMean(pp[ip - 1], pp[ip], permxp[ip - 1], permxp[ip])
                          / viscosity;
                  wp[im] = -coeff * diff
                           * RPMean(pp[ip - 1], pp[ip], prod_der, 0.0);
                  break;
                }

              case 1:
                {
                  diff = pp[ip] - pp[ip + 1];
                  prod_der = rpdp[ip + 1] * dp[ip + 1] + rpp[ip + 1] * ddp[ip + 1];
                  coeff = dt * z_mult_dat[ip] * ffx * (1.0 / dx)
                          * PMean(pp[ip], pp[ip + 1], permxp[ip], permxp[ip + 1])
                          / viscosity;
                  ep[im] = coeff * diff
                           * RPMean(pp[ip], pp[ip + 1], 0.0, prod_der);
                  break;
                }
            }         /* End switch on fdir[0] */
          }           /* End if (fdir[0]) */

          else if (fdir[1])
          {
            switch (fdir[1])
            {
              case -1:
                {
                  diff = pp[ip - sy_v] - pp[ip];
                  prod_der = rpdp[ip - sy_v] * dp[ip - sy_v]
                             + rpp[ip - sy_v] * ddp[ip - sy_v];
                  coeff = dt * z_mult_dat[ip] * ffy * (1.0 / dy)
                          * PMean(pp[ip - sy_v], pp[ip],
                                  permyp[ip - sy_v], permyp[ip])
                          / viscosity;
                  sop[im] = -coeff * diff
                            * RPMean(pp[ip - sy_v], pp[ip], prod_der, 0.0);

                  break;
                }

              case 1:
                {
                  diff = pp[ip] - pp[ip + sy_v];
                  prod_der = rpdp[ip + sy_v] * dp[ip + sy_v]
                             + rpp[ip + sy_v] * ddp[ip + sy_v];
                  coeff = dt * z_mult_dat[ip] * ffy * (1.0 / dy)
                          * PMean(pp[ip], pp[ip + sy_v],
                                  permyp[ip], permyp[ip + sy_v])
                          / viscosity;
                  np[im] = -coeff * diff
                           * RPMean(pp[ip], pp[ip + sy_v], 0.0, prod_der);
                  break;
                }
            }         /* End switch on fdir[1] */
          }           /* End if (fdir[1]) */

          else if (fdir[2])
          {
            switch (fdir[2])
            {
              case -1:
                {
                  lower_cond = (pp[ip - sz_v])
                               - 0.5 * dz * Mean(z_mult_dat[ip], z_mult_dat[ip - sz_v])
                               * dp[ip - sz_v] * gravity;
                  upper_cond = (pp[ip]) + 0.5 * dz * Mean(z_mult_dat[ip], z_mult_dat[ip - sz_v])
                               * dp[ip] * gravity;
                  diff = lower_cond - upper_cond;
                  prod_der = rpdp[ip - sz_v] * dp[ip - sz_v]
                             + rpp[ip - sz_v] * ddp[ip - sz_v];
                  prod_lo = rpp[ip - sz_v] * dp[ip - sz_v];
                  coeff = dt * ffz * (1.0 / (dz * Mean(z_mult_dat[ip], z_mult_dat[ip - sz_v])))
                          * PMeanDZ(permzp[ip - sz_v], permzp[ip],
                                    z_mult_dat[ip - sz_v], z_mult_dat[ip])
                          / viscosity;
                  lp[im] = -coeff *
                           (diff * RPMean(lower_cond, upper_cond,
                                          prod_der, 0.0)
                            - gravity * 0.5 * dz * Mean(z_mult_dat[ip], z_mult_dat[ip - sz_v]) * ddp[ip]
                            * RPMean(lower_cond, upper_cond, prod_lo, prod));

                  break;
                }

              case 1:
                {
                  lower_cond = (pp[ip]) - 0.5 * dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v]) * dp[ip] * gravity;
                  upper_cond = (pp[ip + sz_v])
                               + 0.5 * dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v]) * dp[ip + sz_v] * gravity;
                  diff = lower_cond - upper_cond;
                  prod_der = rpdp[ip + sz_v] * dp[ip + sz_v]
                             + rpp[ip + sz_v] * ddp[ip + sz_v];
                  prod_up = rpp[ip + sz_v] * dp[ip + sz_v];
                  coeff = dt * ffz * (1.0 / (dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])))
                          * PMeanDZ(permzp[ip], permzp[ip + sz_v],
                                    z_mult_dat[ip], z_mult_dat[ip + sz_v])
                          / viscosity;
                  up[im] = -coeff *
                           (diff * RPMean(lower_cond, upper_cond,
                                          0.0, prod_der)
                            - gravity * 0.5 * dz * (Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])) * ddp[ip]
                            * RPMean(lower_cond, upper_cond, prod, prod_up));

                  break;
                }
            }         /* End switch on fdir[2] */
          }        /* End if (fdir[2]) */
        });       /* End Patch Loop */
      }           /* End ipatch loop */
    }             /* End subgrid loop */
  }                  /* End if symm_part */

  ForSubgridI(is, GridSubgrids(grid))
  {
    subgrid = GridSubgrid(grid, is);

    p_sub = VectorSubvector(pressure, is);
    s_sub = VectorSubvector(saturation, is);
    dd_sub = VectorSubvector(density_der, is);
    rpd_sub = VectorSubvector(rel_perm_der, is);
    d_sub = VectorSubvector(density, is);
    rp_sub = VectorSubvector(rel_perm, is);
    permx_sub = VectorSubvector(permeability_x, is);
    permy_sub = VectorSubvector(permeability_y, is);
    permz_sub = VectorSubvector(permeability_z, is);
    J_sub = MatrixSubmatrix(J, is);

    /* overland flow - DOK */
    kw_sub = VectorSubvector(KW, is);
    ke_sub = VectorSubvector(KE, is);
    kn_sub = VectorSubvector(KN, is);
    ks_sub = VectorSubvector(KS, is);
    kwns_sub = VectorSubvector(KWns, is);
    kens_sub = VectorSubvector(KEns, is);
    knns_sub = VectorSubvector(KNns, is);
    ksns_sub = VectorSubvector(KSns, is);

    dx = SubgridDX(subgrid);
    dy = SubgridDY(subgrid);
    dz = SubgridDZ(subgrid);

    /* @RMM added to provide access to zmult */
    z_mult_sub = VectorSubvector(z_mult, is);
    /* @RMM added to provide variable dz */
    z_mult_dat = SubvectorData(z_mult_sub);

    vol = dx * dy * dz;

    ix = SubgridIX(subgrid);
    iy = SubgridIY(subgrid);

    ffx = dy * dz;
    ffy = dx * dz;
    ffz = dx * dy;

    nx_v = SubvectorNX(p_sub);
    ny_v = SubvectorNY(p_sub);
    nz_v = SubvectorNZ(p_sub);

    sy_v = nx_v;
    sz_v = ny_v * nx_v;

    cp = SubmatrixStencilData(J_sub, 0);
    wp = SubmatrixStencilData(J_sub, 1);
    ep = SubmatrixStencilData(J_sub, 2);
    sop = SubmatrixStencilData(J_sub, 3);
    np = SubmatrixStencilData(J_sub, 4);
    lp = SubmatrixStencilData(J_sub, 5);
    up = SubmatrixStencilData(J_sub, 6);

    /* overland flow contribution */
    kw_der = SubvectorData(kw_sub);
    ke_der = SubvectorData(ke_sub);
    kn_der = SubvectorData(kn_sub);
    ks_der = SubvectorData(ks_sub);
    kwns_der = SubvectorData(kwns_sub);
    kens_der = SubvectorData(kens_sub);
    knns_der = SubvectorData(knns_sub);
    ksns_der = SubvectorData(ksns_sub);

    pp = SubvectorData(p_sub);
    sp = SubvectorData(s_sub);
    ddp = SubvectorData(dd_sub);
    rpdp = SubvectorData(rpd_sub);
    dp = SubvectorData(d_sub);
    rpp = SubvectorData(rp_sub);
    permxp = SubvectorData(permx_sub);
    permyp = SubvectorData(permy_sub);
    permzp = SubvectorData(permz_sub);

    for (ipatch = 0; ipatch < BCStructNumPatches(bc_struct); ipatch++)
    {
      bc_patch_values = BCStructPatchValues(bc_struct, ipatch, is);

      switch (BCStructBCType(bc_struct, ipatch))
      {
        case DirichletBC:
        {
          BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
          {
            ip = SubvectorEltIndex(p_sub, i, j, k);

            value = bc_patch_values[ival];

            PFModuleInvokeType(PhaseDensityInvoke, density_module,
                               (0, NULL, NULL, &value, &den_d, CALCFCN));
            PFModuleInvokeType(PhaseDensityInvoke, density_module,
                               (0, NULL, NULL, &value, &dend_d, CALCDER));

            ip = SubvectorEltIndex(p_sub, i, j, k);
            im = SubmatrixEltIndex(J_sub, i, j, k);

            prod = rpp[ip] * dp[ip];
            prod_der = rpdp[ip] * dp[ip] + rpp[ip] * ddp[ip];

            if (fdir[0])
            {
              coeff = dt * ffx * z_mult_dat[ip] * (2.0 / dx) * permxp[ip] / viscosity;

              switch (fdir[0])
              {
                case -1:
                  {
                    op = wp;
                    prod_val = rpp[ip - 1] * den_d;
                    diff = value - pp[ip];
                    o_temp = coeff
                             * (diff * RPMean(value, pp[ip], 0.0, prod_der)
                                - RPMean(value, pp[ip], prod_val, prod));
                    break;
                  }

                case 1:
                  {
                    op = ep;
                    prod_val = rpp[ip + 1] * den_d;
                    diff = pp[ip] - value;
                    o_temp = -coeff
                             * (diff * RPMean(pp[ip], value, prod_der, 0.0)
                                + RPMean(pp[ip], value, prod, prod_val));
                    break;
                  }
              }          /* End switch on fdir[0] */
            }            /* End if (fdir[0]) */

            else if (fdir[1])
            {
              coeff = dt * ffy * z_mult_dat[ip] * (2.0 / dy) * permyp[ip] / viscosity;

              switch (fdir[1])
              {
                case -1:
                  {
                    op = sop;
                    prod_val = rpp[ip - sy_v] * den_d;
                    diff = value - pp[ip];
                    o_temp = coeff
                             * (diff * RPMean(value, pp[ip], 0.0, prod_der)
                                - RPMean(value, pp[ip], prod_val, prod));
                    break;
                  }

                case 1:
                  {
                    op = np;
                    prod_val = rpp[ip + sy_v] * den_d;
                    diff = pp[ip] - value;
                    o_temp = -coeff
                             * (diff * RPMean(pp[ip], value, prod_der, 0.0)
                                + RPMean(pp[ip], value, prod, prod_val));
                    break;
                  }
              }          /* End switch on fdir[1] */
            }            /* End if (fdir[1]) */

            else if (fdir[2])
            {
              coeff = dt * ffz * (2.0 / (dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v]))) * permzp[ip] / viscosity;

              switch (fdir[2])
              {
                case -1:
                  {
                    op = lp;
                    prod_val = rpp[ip - sz_v] * den_d;

                    lower_cond = (value) - 0.5 * dz * z_mult_dat[ip] * den_d * gravity;
                    upper_cond = (pp[ip]) + 0.5 * dz * z_mult_dat[ip] * dp[ip] * gravity;
                    diff = lower_cond - upper_cond;

//                    o_temp = coeff
//                             * (diff * RPMean(lower_cond, upper_cond, 0.0, prod_der)
//                                + ((-1.0 - gravity * 0.5 * dz * Mean(z_mult_dat[ip], z_mult_dat[ip - sz_v]) * ddp[ip])
//                                   * RPMean(lower_cond, upper_cond, prod_val, prod)));

                   o_temp = coeff
                      * (diff * RPMean(lower_cond, upper_cond, 0.0, prod_der)
                     + ((-1.0 - gravity * 0.5 * dz * z_mult_dat[ip] * ddp[ip])
                        * RPMean(lower_cond, upper_cond, prod_val, prod)));

//printf("jacobian lower BC: o_temp=%f prod_der=%f op=%f \n",o_temp, prod_der, op);

                    break;
                  }

                case 1:
                  {
                    op = up;
                    prod_val = rpp[ip + sz_v] * den_d;

                 lower_cond = (pp[ip]) - 0.5 * dz * z_mult_dat[ip] * dp[ip] * gravity;
                 upper_cond = (value) + 0.5 * dz * z_mult_dat[ip] * den_d * gravity;
                  diff = lower_cond - upper_cond;

                 o_temp = -coeff* (diff * RPMean(lower_cond, upper_cond, prod_der, 0.0)
                        + ((1.0 - gravity * 0.5 * dz * z_mult_dat[ip] * ddp[ip])
                                                  * RPMean(lower_cond, upper_cond, prod, prod_val)));


                    break;
                  }
              }          /* End switch on fdir[2] */
            }         /* End if (fdir[2]) */

            cp[im] += op[im];
            cp[im] -= o_temp;
//            printf("jacobian: cp[im]+=%f cp[im]-=%f \n",op[im], o_temp);

            op[im] = 0.0;
          });

          break;
        }               /* End DirichletBC case */

        case FluxBC:
        {
          BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
          {
            im = SubmatrixEltIndex(J_sub, i, j, k);

            if (fdir[0] == -1)
              op = wp;
            if (fdir[0] == 1)
              op = ep;
            if (fdir[1] == -1)
              op = sop;
            if (fdir[1] == 1)
              op = np;
            if (fdir[2] == -1)
              op = lp;
            if (fdir[2] == 1)
              op = up;

            cp[im] += op[im];
            op[im] = 0.0;
          });

          break;
        }         /* End fluxbc case */

        case OverlandBC:     //sk
        {
          BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
          {
            im = SubmatrixEltIndex(J_sub, i, j, k);

            //remove contributions to this row corresponding to boundary
            if (fdir[0] == -1)
              op = wp;
            else if (fdir[0] == 1)
              op = ep;
            else if (fdir[1] == -1)
              op = sop;
            else if (fdir[1] == 1)
              op = np;
            else if (fdir[2] == -1)
              op = lp;
            else       // (fdir[2] ==  1)
            {
              op = up;
              /* check if overland flow kicks in */
              if (!ovlnd_flag)
              {
                ip = SubvectorEltIndex(p_sub, i, j, k);
                if ((pp[ip]) > 0.0)
                {
                  ovlnd_flag = 1;
                }
              }
            }

            cp[im] += op[im];
            op[im] = 0.0;       //zero out entry in row of Jacobian
          });

          switch (public_xtra->type)
          {
            case no_nonlinear_jacobian:
            case not_set:
            {
              assert(1);
            }

            case simple:
            {
              BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
              {
                if (fdir[2] == 1)
                {
                  ip = SubvectorEltIndex(p_sub, i, j, k);
                  io = SubvectorEltIndex(p_sub, i, j, 0);
                  im = SubmatrixEltIndex(J_sub, i, j, k);

                  if ((pp[ip]) > 0.0)
                  {
                    cp[im] += (vol * z_mult_dat[ip]) / (dz * Mean(z_mult_dat[ip], z_mult_dat[ip + sz_v])) * (dt + 1);
                  }
                }
              });
              break;
            }

            case overland_flow:
            {
              /* Get overland flow contributions - DOK*/
              // SGS can we skip this invocation if !overland_flow?
              //PFModuleInvokeType(OverlandFlowEvalInvoke, overlandflow_module,
              //		(grid, is, bc_struct, ipatch, problem_data, pressure,
              //		 ke_der, kw_der, kn_der, ks_der, NULL, NULL, CALCDER));

              if (overlandspinup == 1)
              {
                /* add flux loss equal to excess head  that overwrites the prior overland flux */
                BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
                {
                  if (fdir[2] == 1)
                  {
                    ip = SubvectorEltIndex(p_sub, i, j, k);
                    io = SubvectorEltIndex(p_sub, i, j, 0);
                    im = SubmatrixEltIndex(J_sub, i, j, k);
                    vol = dx * dy * dz;

                    if ((pp[ip]) >= 0.0)
                    {
                      cp[im] += (vol / dz) * dt * (1.0 + 0.0);                     //LEC
//                      printf("Jac SU: CP=%f im=%d  \n", cp[im], im);
                    }
                    else
                    {
                      cp[im] += 0.0;
                    }
                  }
                });
              }
              else
              {
                /* Get overland flow contributions for using kinematic or diffusive - LEC */
                if (diffusive == 0)
                {
                  // @MCB: Need to validate this is not needed under the OverlandBC case,
                  // should instead be moved to the OverlandKinematicBC case
                  PFModuleInvokeType(OverlandFlowEvalInvoke, overlandflow_module,
                                     (grid, is, bc_struct, ipatch, problem_data, pressure, old_pressure,
                                      ke_der, kw_der, kn_der, ks_der, NULL, NULL, CALCDER));
                }
                else
                {
                  /* Test running Diffuisve calc FCN */
                  //double *dummy1, *dummy2, *dummy3, *dummy4;
                  //PFModuleInvokeType(OverlandFlowEvalDiffInvoke, overlandflow_module_diff, (grid, is, bc_struct, ipatch, problem_data, pressure,
                  //                                             ke_der, kw_der, kn_der, ks_der,
                  //       dummy1, dummy2, dummy3, dummy4,
                  //                                                    NULL, NULL, CALCFCN));

                  PFModuleInvokeType(OverlandFlowEvalDiffInvoke, overlandflow_module_diff,
                                     (grid, is, bc_struct, ipatch, problem_data, pressure, old_pressure,
                                      ke_der, kw_der, kn_der, ks_der,
                                      kens_der, kwns_der, knns_der, ksns_der, NULL, NULL, CALCDER));
                }
              }


              break;
            }
          }
		  break;
        }         /* End overland flow case */

        case SeepageFaceBC:
        {

            /* add flux loss equal to excess head  that overwrites the prior overland flux */
            BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
            {
              if (fdir[2] == 1)
              {
                ip = SubvectorEltIndex(p_sub, i, j, k);
                io = SubvectorEltIndex(p_sub, i, j, 0);
                im = SubmatrixEltIndex(J_sub, i, j, k);
                vol = dx * dy * dz;

                if ((pp[ip]) >= 0.0)
                {
                  cp[im] += (vol / dz) * dt * (1.0 + 0.0);                     //@RMM
//                  printf("Jac SF: CP=%f im=%d  \n", cp[im], im);

                }
                else
                {
                  cp[im] += 0.0;
                }
              }
            });
            break;
        }  // end case seepage face

        /*  OverlandBC for KWE with upwinding, call module */
        case OverlandKinematicBC:
        {
          BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
          {
            im = SubmatrixEltIndex(J_sub, i, j, k);
                  public_xtra->type = overland_flow;
            //remove contributions to this row corresponding to boundary
            if (fdir[0] == -1)
              op = wp;
            else if (fdir[0] == 1)
              op = ep;
            else if (fdir[1] == -1)
              op = sop;
            else if (fdir[1] == 1)
              op = np;
            else if (fdir[2] == -1)
              op = lp;
            else       // (fdir[2] ==  1)
            {
              op = up;
              /* check if overland flow kicks in */
              if (!ovlnd_flag)
              {
                ip = SubvectorEltIndex(p_sub, i, j, k);
                if ((pp[ip]) > 0.0)
                {
                  ovlnd_flag = 1;
                }
              }
            }

            cp[im] += op[im];
            op[im] = 0.0;       //zero out entry in row of Jacobian
          });

          PFModuleInvokeType(OverlandFlowEvalKinInvoke, overlandflow_module_kin,
                             (grid, is, bc_struct, ipatch, problem_data, pressure,
                              ke_der, kw_der, kn_der, ks_der,
                              NULL, NULL, NULL, NULL, NULL, NULL, CALCDER));
		  break;
        } /* End OverlandKinematicBC */

        /* OverlandDiffusiveBC */
        case OverlandDiffusiveBC:
        {
          BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
          {
            im = SubmatrixEltIndex(J_sub, i, j, k);
                public_xtra->type = overland_flow;

            //remove contributions to this row corresponding to boundary
            if (fdir[0] == -1)
              op = wp;
            else if (fdir[0] == 1)
              op = ep;
            else if (fdir[1] == -1)
              op = sop;
            else if (fdir[1] == 1)
              op = np;
            else if (fdir[2] == -1)
              op = lp;
            else       // (fdir[2] ==  1)
            {
              op = up;
              /* check if overland flow kicks in */
              if (!ovlnd_flag)
              {
                ip = SubvectorEltIndex(p_sub, i, j, k);
                if ((pp[ip]) > 0.0)
                {
                  ovlnd_flag = 1;
                }
              }
            }

            cp[im] += op[im];
            op[im] = 0.0;       //zero out entry in row of Jacobian
          });

          PFModuleInvokeType(OverlandFlowEvalDiffInvoke, overlandflow_module_diff,
                          (grid, is, bc_struct, ipatch, problem_data, pressure, old_pressure,
                            ke_der, kw_der, kn_der, ks_der,
                            kens_der, kwns_der, knns_der, ksns_der, NULL, NULL, CALCDER));

          break;
        } /* End OverlandDiffusiveBC */


      }        /* End switch BCtype */
    }          /* End ipatch loop */
  }            /* End subgrid loop */

  PFModuleInvokeType(RichardsBCInternalInvoke, bc_internal, (problem, problem_data, NULL, J, time,
                                                             pressure, CALCDER));



  if (public_xtra->type == overland_flow)
  {
    // SGS always have to do communication here since
    // each processor may/may not be doing overland flow.
    /* Update ghost points for JB before building JC */
    if (MatrixCommPkg(J))
    {
      handle = InitMatrixUpdate(J);
      FinalizeMatrixUpdate(handle);
    }

    /* Pass KW values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KW, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KE values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KE, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KS values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KS, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KN values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KN, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KWns values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KWns, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KEns values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KEns, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KSns values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KSns, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
    /* Pass KNns values to neighbors.  */
    vector_update_handle = InitVectorUpdate(KNns, VectorUpdateAll);
    FinalizeVectorUpdate(vector_update_handle);
  }

  /* Build submatrix JC if overland flow case */
  if (ovlnd_flag && public_xtra->type == overland_flow)
  {
    /* begin loop to build JC */
    ForSubgridI(is, GridSubgrids(grid))
    {
      subgrid = GridSubgrid(grid, is);

      dx = SubgridDX(subgrid);
      dy = SubgridDY(subgrid);
      dz = SubgridDZ(subgrid);

      vol = dx * dy * dz;

      ffx = dy * dz;
      ffy = dx * dz;
      ffz = dx * dy;

      p_sub = VectorSubvector(pressure, is);

      J_sub = MatrixSubmatrix(J, is);
      JC_sub = MatrixSubmatrix(JC, is);

      kw_sub = VectorSubvector(KW, is);
      ke_sub = VectorSubvector(KE, is);
      kn_sub = VectorSubvector(KN, is);
      ks_sub = VectorSubvector(KS, is);
      kwns_sub = VectorSubvector(KWns, is);
      kens_sub = VectorSubvector(KEns, is);
      knns_sub = VectorSubvector(KNns, is);
      ksns_sub = VectorSubvector(KSns, is);

      top_sub = VectorSubvector(top, is);
      sx_sub = VectorSubvector(slope_x, is);

      sy_v = SubvectorNX(sx_sub);
      nx_m = SubmatrixNX(J_sub);
      ny_m = SubmatrixNY(J_sub);
      sy_m = nx_m;
      sz_m = nx_m * ny_m;

      ix = SubgridIX(subgrid);
      iy = SubgridIY(subgrid);
      iz = SubgridIZ(subgrid);

      nx = SubgridNX(subgrid);
      ny = SubgridNY(subgrid);

      pp = SubvectorData(p_sub);
      /* for Bmat */
      cp = SubmatrixStencilData(J_sub, 0);
      wp = SubmatrixStencilData(J_sub, 1);
      ep = SubmatrixStencilData(J_sub, 2);
      sop = SubmatrixStencilData(J_sub, 3);
      np = SubmatrixStencilData(J_sub, 4);
      lp = SubmatrixStencilData(J_sub, 5);
      up = SubmatrixStencilData(J_sub, 6);

      /* for Cmat */
      cp_c = SubmatrixStencilData(JC_sub, 0);
      wp_c = SubmatrixStencilData(JC_sub, 1);
      ep_c = SubmatrixStencilData(JC_sub, 2);
      sop_c = SubmatrixStencilData(JC_sub, 3);
      np_c = SubmatrixStencilData(JC_sub, 4);

      kw_der = SubvectorData(kw_sub);
      ke_der = SubvectorData(ke_sub);
      kn_der = SubvectorData(kn_sub);
      ks_der = SubvectorData(ks_sub);
      kwns_der = SubvectorData(kwns_sub);
      kens_der = SubvectorData(kens_sub);
      knns_der = SubvectorData(knns_sub);
      ksns_der = SubvectorData(ksns_sub);

      top_dat = SubvectorData(top_sub);

      for (ipatch = 0; ipatch < BCStructNumPatches(bc_struct); ipatch++)
      {
        switch (BCStructBCType(bc_struct, ipatch))
        {
          /* Fall through cases for new Overland types */
          case OverlandKinematicBC:
          {
            BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
            {
              if (fdir[2] == 1)
              {
                /* Loop over boundary patches to build JC matrix.
                 */
                io = SubmatrixEltIndex(J_sub, i, j, iz);
                io1 = SubvectorEltIndex(sx_sub, i, j, 0);
                itop = SubvectorEltIndex(top_sub, i, j, 0);

                /* Update JC */
                ip = SubvectorEltIndex(p_sub, i, j, k);
                im = SubmatrixEltIndex(J_sub, i, j, k);

                /* First put contributions from subsurface diagonal onto diagonal of JC */
                cp_c[io] = cp[im];
                cp[im] = 0.0;         // update JB
                /* Now check off-diagonal nodes to see if any surface-surface connections exist */
                /* West */
                k1 = (int)top_dat[itop - 1];

                if (k1 >= 0)
                {
                  if (k1 == k)         /*west node is also surface node */
                  {
                    wp_c[io] += wp[im];
                    wp[im] = 0.0;           // update JB
                  }
                }
                /* East */
                k1 = (int)top_dat[itop + 1];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*east node is also surface node */
                  {
                    ep_c[io] += ep[im];
                    ep[im] = 0.0;           //update JB
                  }
                }
                /* South */
                k1 = (int)top_dat[itop - sy_v];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*south node is also surface node */
                  {
                    sop_c[io] += sop[im];
                    sop[im] = 0.0;           //update JB
                  }
                }
                /* North */
                k1 = (int)top_dat[itop + sy_v];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*north node is also surface node */
                  {
                    np_c[io] += np[im];
                    np[im] = 0.0;           // Update JB
                  }
                }

                /* Now add overland contributions to JC */
                if ((pp[ip]) > 0.0)
                {
                  /*diagonal term */
                  cp_c[io] += (vol / dz) + (vol / ffy) * dt * (ke_der[io1] - kw_der[io1])
                              + (vol / ffx) * dt * (kn_der[io1] - ks_der[io1]);
                }

                  /*west term */
                  wp_c[io] -= (vol / ffy) * dt * (ke_der[io1 - 1]);

                  /*East term */
                  ep_c[io] += (vol / ffy) * dt * (kw_der[io1 + 1]);

                  /*south term */
                  sop_c[io] -= (vol / ffx) * dt * (kn_der[io1 - sy_v]);

                  /*north term */
                  np_c[io] += (vol / ffx) * dt * (ks_der[io1 + sy_v]);

              }
            });
            break;
          }
          case OverlandDiffusiveBC:
          {
            BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
            {
              if (fdir[2] == 1)
              {
                /* Loop over boundary patches to build JC matrix.
                 */
                io = SubmatrixEltIndex(J_sub, i, j, iz);
                io1 = SubvectorEltIndex(sx_sub, i, j, 0);
                itop = SubvectorEltIndex(top_sub, i, j, 0);

                /* Update JC */
                ip = SubvectorEltIndex(p_sub, i, j, k);
                im = SubmatrixEltIndex(J_sub, i, j, k);

                /* First put contributions from subsurface diagonal onto diagonal of JC */
                cp_c[io] = cp[im];
                cp[im] = 0.0;         // update JB
                /* Now check off-diagonal nodes to see if any surface-surface connections exist */
                /* West */
                k1 = (int)top_dat[itop - 1];

                if (k1 >= 0)
                {
                  if (k1 == k)         /*west node is also surface node */
                  {
                    wp_c[io] += wp[im];
                    wp[im] = 0.0;           // update JB
                  }
                }
                /* East */
                k1 = (int)top_dat[itop + 1];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*east node is also surface node */
                  {
                    ep_c[io] += ep[im];
                    ep[im] = 0.0;           //update JB
                  }
                }
                /* South */
                k1 = (int)top_dat[itop - sy_v];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*south node is also surface node */
                  {
                    sop_c[io] += sop[im];
                    sop[im] = 0.0;           //update JB
                  }
                }
                /* North */
                k1 = (int)top_dat[itop + sy_v];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*north node is also surface node */
                  {
                    np_c[io] += np[im];
                    np[im] = 0.0;           // Update JB
                  }
                }

                /* Now add overland contributions to JC */
                if ((pp[ip]) > 0.0)
                {
                  /*diagonal term */
                  cp_c[io] += (vol / dz) + (vol / ffy) * dt * (ke_der[io1] - kw_der[io1])
                              + (vol / ffx) * dt * (kn_der[io1] - ks_der[io1]);
                }
                /*west term */
                wp_c[io] -= (vol / ffy) * dt * (kwns_der[io1]);

                /*East term */
                ep_c[io] += (vol / ffy) * dt * (kens_der[io1]);

                /*south term */
                sop_c[io] -= (vol / ffx) * dt * (ksns_der[io1]);

                /*north term */
                np_c[io] += (vol / ffx) * dt * (knns_der[io1]);

              }
            });
            break;
          }
          case OverlandBC:
          {
            BCStructPatchLoop(i, j, k, fdir, ival, bc_struct, ipatch, is,
            {
              if (fdir[2] == 1)
              {
                /* Loop over boundary patches to build JC matrix.
                 */
                io = SubmatrixEltIndex(J_sub, i, j, iz);
                io1 = SubvectorEltIndex(sx_sub, i, j, 0);
                itop = SubvectorEltIndex(top_sub, i, j, 0);

                /* Update JC */
                ip = SubvectorEltIndex(p_sub, i, j, k);
                im = SubmatrixEltIndex(J_sub, i, j, k);

                /* First put contributions from subsurface diagonal onto diagonal of JC */
                cp_c[io] = cp[im];
                cp[im] = 0.0;         // update JB
                /* Now check off-diagonal nodes to see if any surface-surface connections exist */
                /* West */
                k1 = (int)top_dat[itop - 1];

                if (k1 >= 0)
                {
                  if (k1 == k)         /*west node is also surface node */
                  {
                    wp_c[io] += wp[im];
                    wp[im] = 0.0;           // update JB
                  }
                }
                /* East */
                k1 = (int)top_dat[itop + 1];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*east node is also surface node */
                  {
                    ep_c[io] += ep[im];
                    ep[im] = 0.0;           //update JB
                  }
                }
                /* South */
                k1 = (int)top_dat[itop - sy_v];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*south node is also surface node */
                  {
                    sop_c[io] += sop[im];
                    sop[im] = 0.0;           //update JB
                  }
                }
                /* North */
                k1 = (int)top_dat[itop + sy_v];
                if (k1 >= 0)
                {
                  if (k1 == k)         /*north node is also surface node */
                  {
                    np_c[io] += np[im];
                    np[im] = 0.0;           // Update JB
                  }
                }

                /* Now add overland contributions to JC */
                if ((pp[ip]) > 0.0)
                {
                  /*diagonal term */
                  cp_c[io] += (vol / dz) + (vol / ffy) * dt * (ke_der[io1] - kw_der[io1])
                              + (vol / ffx) * dt * (kn_der[io1] - ks_der[io1]);
                }
                else
                {
                  // Laura's version
                  cp_c[io] += 0.0 + dt * (vol / dz) * (public_xtra->SpinupDampP1 * exp(pfmin(pp[ip], 0.0) * public_xtra->SpinupDampP1) * public_xtra->SpinupDampP2); //NBE
                }

                if (diffusive == 0)
                {
                  /*west term */
                  wp_c[io] -= (vol / ffy) * dt * (ke_der[io1 - 1]);

                  /*East term */
                  ep_c[io] += (vol / ffy) * dt * (kw_der[io1 + 1]);

                  /*south term */
                  sop_c[io] -= (vol / ffx) * dt * (kn_der[io1 - sy_v]);

                  /*north term */
                  np_c[io] += (vol / ffx) * dt * (ks_der[io1 + sy_v]);
                }
                else
                {
                  /*west term */
                  wp_c[io] -= (vol / ffy) * dt * (kwns_der[io1]);

                  /*East term */
                  ep_c[io] += (vol / ffy) * dt * (kens_der[io1]);

                  /*south term */
                  sop_c[io] -= (vol / ffx) * dt * (ksns_der[io1]);

                  /*north term */
                  np_c[io] += (vol / ffx) * dt * (knns_der[io1]);
                }
              }
            });
            break;
          }
        }         /* End switch BCtype */
      }           /* End ipatch loop */
    }             /* End subgrid loop */
  }



  /* Set pressures outside domain to zero.
   * Recall: equation to solve is f = 0, so components of f outside
   * domain are set to the respective pressure value.
   *
   * Should change this to set pressures to scaling value.
   * CSW: Should I set this to pressure * vol * dt ??? */

  ForSubgridI(is, GridSubgrids(grid))
  {
    subgrid = GridSubgrid(grid, is);

    J_sub = MatrixSubmatrix(J, is);

    /* RDF: assumes resolutions are the same in all 3 directions */
    r = SubgridRX(subgrid);

    ix = SubgridIX(subgrid);
    iy = SubgridIY(subgrid);
    iz = SubgridIZ(subgrid);

    nx = SubgridNX(subgrid);
    ny = SubgridNY(subgrid);
    nz = SubgridNZ(subgrid);

    cp = SubmatrixStencilData(J_sub, 0);
    wp = SubmatrixStencilData(J_sub, 1);
    ep = SubmatrixStencilData(J_sub, 2);
    sop = SubmatrixStencilData(J_sub, 3);
    np = SubmatrixStencilData(J_sub, 4);
    lp = SubmatrixStencilData(J_sub, 5);
    up = SubmatrixStencilData(J_sub, 6);

    /* for Cmat */
    JC_sub = MatrixSubmatrix(JC, is);
    cp_c = SubmatrixStencilData(JC_sub, 0);
    wp_c = SubmatrixStencilData(JC_sub, 1);
    ep_c = SubmatrixStencilData(JC_sub, 2);
    sop_c = SubmatrixStencilData(JC_sub, 3);
    np_c = SubmatrixStencilData(JC_sub, 4);

    GrGeomOutLoop(i, j, k, gr_domain, r, ix, iy, iz, nx, ny, nz,
    {
      im = SubmatrixEltIndex(J_sub, i, j, k);
      cp[im] = 1.0;
      wp[im] = 0.0;
      ep[im] = 0.0;
      sop[im] = 0.0;
      np[im] = 0.0;
      lp[im] = 0.0;
      up[im] = 0.0;

//#if 0
//		       /* JC matrix */
//		       cp_c[im] = 1.0;
//		       wp_c[im] = 0.0;
//		       ep_c[im] = 0.0;
//		       sop_c[im] = 0.0;
//		       np_c[im] = 0.0;
//#endif */
    });
  }


  /*-----------------------------------------------------------------------
   * Update matrix ghost points
   *-----------------------------------------------------------------------*/
  if (public_xtra->type == overland_flow)
  {
    /* Update matrices and setup pointers */
    if (MatrixCommPkg(J))
    {
      handle = InitMatrixUpdate(J);
      FinalizeMatrixUpdate(handle);
    }
    *ptr_to_J = J;

    if (MatrixCommPkg(JC))
    {
      handle = InitMatrixUpdate(JC);
      FinalizeMatrixUpdate(handle);
    }
    *ptr_to_JC = JC;
  }
  else  /* No overland flow */
  {
    *ptr_to_JC = NULL;

    if (MatrixCommPkg(J))
    {
      handle = InitMatrixUpdate(J);
      FinalizeMatrixUpdate(handle);
    }

    *ptr_to_J = J;
  } /* end if ovlnd_flag */

  /*-----------------------------------------------------------------------
   * Free temp vectors
   *-----------------------------------------------------------------------*/

  FreeBCStruct(bc_struct);

  FreeVector(density_der);
  FreeVector(saturation_der);
  FreeVector(KW);
  FreeVector(KE);
  FreeVector(KN);
  FreeVector(KS);
  FreeVector(KWns);
  FreeVector(KEns);
  FreeVector(KNns);
  FreeVector(KSns);

  return;
}


/*--------------------------------------------------------------------------
 * RichardsJacobianEvalInitInstanceXtra
 *--------------------------------------------------------------------------*/

PFModule    *RichardsJacobianEvalInitInstanceXtra(
                                                  Problem *    problem,
                                                  Grid *       grid,
                                                  ProblemData *problem_data,
                                                  double *     temp_data,
                                                  int          symmetric_jac)
{
  PFModule      *this_module = ThisPFModule;
  InstanceXtra  *instance_xtra;

  Stencil       *stencil, *stencil_C;

  (void)problem_data;

  if (PFModuleInstanceXtra(this_module) == NULL)
    instance_xtra = ctalloc(InstanceXtra, 1);
  else
    instance_xtra = (InstanceXtra*)PFModuleInstanceXtra(this_module);

  if (grid != NULL)
  {
    /* free old data */
    if ((instance_xtra->grid) != NULL)
    {
      FreeMatrix(instance_xtra->J);
      FreeMatrix(instance_xtra->JC);      /* DOK */
    }

    /* set new data */
    (instance_xtra->grid) = grid;

    /* set up jacobian matrix */
    stencil = NewStencil(jacobian_stencil_shape, 7);
    stencil_C = NewStencil(jacobian_stencil_shape, 7);  //DOK

    if (symmetric_jac)
    {
      (instance_xtra->J) = NewMatrixType(grid, NULL, stencil, ON, stencil,
                                         matrix_cell_centered);
      (instance_xtra->JC) = NewMatrixType(grid, NULL, stencil_C, ON, stencil_C,
                                          matrix_cell_centered);
    }
    else
    {
      (instance_xtra->J) = NewMatrixType(grid, NULL, stencil, OFF, stencil,
                                         matrix_cell_centered);
      (instance_xtra->JC) = NewMatrixType(grid, NULL, stencil_C, OFF, stencil_C,
                                          matrix_cell_centered);
    }
  }

  if (temp_data != NULL)
  {
    (instance_xtra->temp_data) = temp_data;
  }

  if (problem != NULL)
  {
    (instance_xtra->problem) = problem;
  }

  if (PFModuleInstanceXtra(this_module) == NULL)
  {
    (instance_xtra->density_module) =
      PFModuleNewInstance(ProblemPhaseDensity(problem), ());
    (instance_xtra->bc_pressure) =
      PFModuleNewInstanceType(BCPressureInitInstanceXtraInvoke, ProblemBCPressure(problem), (problem));
    (instance_xtra->saturation_module) =
      PFModuleNewInstanceType(SaturationInitInstanceXtraInvoke, ProblemSaturation(problem), (NULL, NULL));
    (instance_xtra->rel_perm_module) =
      PFModuleNewInstanceType(PhaseRelPermInitInstanceXtraInvoke, ProblemPhaseRelPerm(problem), (NULL, NULL));
    (instance_xtra->bc_internal) =
      PFModuleNewInstance(ProblemBCInternal(problem), ());
    (instance_xtra->overlandflow_module) =
      PFModuleNewInstance(ProblemOverlandFlowEval(problem), ());     //DOK
    (instance_xtra->overlandflow_module_diff) =
      PFModuleNewInstance(ProblemOverlandFlowEvalDiff(problem), ());   //RMM-LEC
    (instance_xtra->overlandflow_module_kin)
      = PFModuleNewInstance(ProblemOverlandFlowEvalKin(problem), ());
  }
  else
  {
    PFModuleReNewInstance((instance_xtra->density_module), ());
    PFModuleReNewInstanceType(BCPressureInitInstanceXtraInvoke, (instance_xtra->bc_pressure), (problem));
    PFModuleReNewInstanceType(SaturationInitInstanceXtraInvoke, (instance_xtra->saturation_module),
                              (NULL, NULL));
    PFModuleReNewInstanceType(PhaseRelPermInitInstanceXtraInvoke, (instance_xtra->rel_perm_module),
                              (NULL, NULL));
    PFModuleReNewInstance((instance_xtra->bc_internal), ());
    PFModuleReNewInstance((instance_xtra->overlandflow_module), ());     //DOK
    PFModuleReNewInstance((instance_xtra->overlandflow_module_diff), ());      //RMM-LEC
    PFModuleReNewInstance((instance_xtra->overlandflow_module_kin), ());
  }


  PFModuleInstanceXtra(this_module) = instance_xtra;
  return this_module;
}


/*--------------------------------------------------------------------------
 * RichardsJacobianEvalFreeInstanceXtra
 *--------------------------------------------------------------------------*/

void  RichardsJacobianEvalFreeInstanceXtra()
{
  PFModule      *this_module = ThisPFModule;
  InstanceXtra  *instance_xtra = (InstanceXtra*)PFModuleInstanceXtra(this_module);

  if (instance_xtra)
  {
    PFModuleFreeInstance(instance_xtra->density_module);
    PFModuleFreeInstance(instance_xtra->bc_pressure);
    PFModuleFreeInstance(instance_xtra->saturation_module);
    PFModuleFreeInstance(instance_xtra->rel_perm_module);
    PFModuleFreeInstance(instance_xtra->bc_internal);
    PFModuleFreeInstance(instance_xtra->overlandflow_module);     //DOK
    PFModuleFreeInstance(instance_xtra->overlandflow_module_diff);       //RMM-LEC
    PFModuleFreeInstance(instance_xtra->overlandflow_module_kin);

    FreeMatrix(instance_xtra->J);

    FreeMatrix(instance_xtra->JC);     /* DOK */

    tfree(instance_xtra);
  }
}


/*--------------------------------------------------------------------------
 * RichardsJacobianEvalNewPublicXtra
 *--------------------------------------------------------------------------*/

PFModule   *RichardsJacobianEvalNewPublicXtra(char *name)
{
  PFModule      *this_module = ThisPFModule;
  PublicXtra    *public_xtra;
  char key[IDB_MAX_KEY_LEN];
  char          *switch_name;
  int switch_value;
  NameArray switch_na;
  NameArray upwind_switch_na;


  (void)name;

  public_xtra = ctalloc(PublicXtra, 1);
/* These parameters dampen the transition/switching into overland flow to speedup
 * the spinup process */
  sprintf(key, "OverlandSpinupDampP1");
  public_xtra->SpinupDampP1 = GetDoubleDefault(key, 0.0);
  sprintf(key, "OverlandSpinupDampP2");
  public_xtra->SpinupDampP2 = GetDoubleDefault(key, 0.0);    // NBE

  ///* parameters for upwinding formulation for TFG */
  upwind_switch_na = NA_NewNameArray("Original UpwindSine Upwind");
  sprintf(key, "Solver.TerrainFollowingGrid.SlopeUpwindFormulation", name);
  switch_name = GetStringDefault(key, "Original");
  switch_value = NA_NameToIndex(upwind_switch_na, switch_name);
  switch (switch_value)
  {
    case 0:
    {
      public_xtra->tfgupwind = 0;
      break;
    }

    case 1:
    {
      public_xtra->tfgupwind = 1;
      break;
    }

    case 2:
    {
      public_xtra->tfgupwind = 2;
      break;
    }

    default:
    {
      InputError("Error: Invalid value <%s> for key <%s>\n", switch_name,
                 key);
    }
  }


  switch_na = NA_NewNameArray("False True");
  sprintf(key, "Solver.Nonlinear.UseJacobian");
  switch_name = GetStringDefault(key, "False");
  switch_value = NA_NameToIndex(switch_na, switch_name);
  switch (switch_value)
  {
    case 0:
    {
      public_xtra->type = no_nonlinear_jacobian;
      break;
    }

    case 1:
    {
      public_xtra->type = not_set;
      break;
    }

    default:
    {
      InputError("Error: Invalid value <%s> for key <%s>\n", switch_name,
                 key);
    }
  }
  NA_FreeNameArray(switch_na);

  PFModulePublicXtra(this_module) = public_xtra;
  return this_module;
}


/*--------------------------------------------------------------------------
 * RichardsJacobianEvalFreePublicXtra
 *--------------------------------------------------------------------------*/

void  RichardsJacobianEvalFreePublicXtra()
{
  PFModule    *this_module = ThisPFModule;
  PublicXtra  *public_xtra = (PublicXtra*)PFModulePublicXtra(this_module);


  if (public_xtra)
  {
    tfree(public_xtra);
  }
}


/*--------------------------------------------------------------------------
 * RichardsJacobianEvalSizeOfTempData
 *--------------------------------------------------------------------------*/

int  RichardsJacobianEvalSizeOfTempData()
{
  int sz = 0;

  return sz;
}
