/*******************************************************************************
 * 2-fluid equations for drift-wave tests
 * 
 * Settings:
 *  - ZeroElMass
 *  - AparInEpar
 *******************************************************************************/

#include "bout.h"
#include "initialprofiles.h"
#include "derivs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// 2D initial profiles
Field2D Ni0, Ti0, Te0;

// 3D evolving fields
Field3D rho, Ni, Ajpar;

// 3D time-derivatives
Field3D F_rho, F_Ni, F_Ajpar;

// Derived 3D variables
Field3D phi, Apar, Ve, jpar;

// Non-linear coefficients
Field3D nu, mu_i;

// Metric coefficients
Field2D Rxy, Bpxy, Btxy, hthe;

// parameters
real Te_x, Ti_x, Ni_x, Vi_x, bmag, rho_s, fmei, AA, ZZ;
real lambda_ei, lambda_ii;
real nu_hat, mui_hat, wci, nueix, nuiix;
real beta_p;

// settings
bool estatic, ZeroElMass; // Switch for electrostatic operation (true = no Apar)
bool AparInEpar;
real zeff, nu_perp;
bool evolve_ajpar;
real ShearFactor;
real nu_factor;

int phi_flags, apar_flags; // Inversion flags

// Communication object
Communicator comms;
Communicator com_jp;

// Field routines
int solve_phi_tridag(Field3D &r, Field3D &p, int flags);
int solve_apar_tridag(Field3D &aj, Field3D &ap, int flags);

int physics_init()
{
  Field2D I; // Shear factor 
  
  output.write("Solving 6-variable 2-fluid equations\n");

  /************* LOAD DATA FROM GRID FILE ****************/

  // Load 2D profiles (set to zero if not found)
  mesh->get(Ni0,    "Ni0");
  mesh->get(Ti0,    "Ti0");
  mesh->get(Te0,    "Te0");

  // Load metrics
  mesh->get(Rxy,  "Rxy");
  mesh->get(Bpxy, "Bpxy");
  mesh->get(Btxy, "Btxy");
  mesh->get(hthe, "hthe");
  mesh->get(dx,   "dpsi");
  mesh->get(I,    "sinty");
  mesh->get(zShift, "qinty");

  // Load normalisation values
  mesh->get(Te_x, "Te_x");
  mesh->get(Ti_x, "Ti_x");
  mesh->get(Ni_x, "Ni_x");
  mesh->get(bmag, "bmag");

  Ni_x *= 1.0e14;
  bmag *= 1.0e4;

  /*************** READ OPTIONS *************************/

  options.setSection("2fluid");

  options.get("AA",          AA,          2.0);
  options.get("ZZ",          ZZ,          1.0);

  options.get("estatic",     estatic,     false);
  options.get("ZeroElMass",  ZeroElMass,  false);
  options.get("AparInEpar",  AparInEpar,  true);

  options.get("Zeff",        zeff,        1.0);
  options.get("nu_perp",     nu_perp,     0.0);
  options.get("ShearFactor", ShearFactor, 1.0);
  options.get("nu_factor",   nu_factor,   1.0);

  options.get("phi_flags", phi_flags, 0);
  options.get("apar_flags", apar_flags, 0);

  options.get("Ajpar", "evolve", evolve_ajpar, true);

  if(ZeroElMass)
    evolve_ajpar = 0; // Don't need ajpar - calculated from ohm's law

  /************* SHIFTED RADIAL COORDINATES ************/

  if(ShiftXderivs) {
    ShearFactor = 0.0;  // I disappears from metric
  }

  /************** CALCULATE PARAMETERS *****************/

  rho_s = 1.02*sqrt(AA*Te_x)/ZZ/bmag;
  fmei  = 1./1836.2/AA;

  lambda_ei = 24.-log(sqrt(Ni_x)/Te_x);
  lambda_ii = 23.-log(ZZ*ZZ*ZZ*sqrt(2.*Ni_x)/pow(Ti_x, 1.5));
  wci       = 9.58e3*ZZ*bmag/AA;
  nueix     = 2.91e-6*Ni_x*lambda_ei/pow(Te_x, 1.5);
  nuiix     = 4.78e-8*pow(ZZ,4.)*Ni_x*lambda_ii/pow(Ti_x, 1.5)/sqrt(AA);
  nu_hat    = nu_factor*zeff*nueix/wci;

  if(nu_perp < 1.e-10) {
    mui_hat      = (3./10.)*nuiix/wci;
  } else
    mui_hat      = nu_perp;

  if(estatic) {
    beta_p    = 1.e-29;
  }else
    beta_p    = 4.03e-11*Ni_x*Te_x/bmag/bmag;

  Vi_x = wci * rho_s;

  output.write("Normalisation: rho_s = %e  wci = %e  beta_p = %e\n", rho_s, wci, beta_p);

  /************** PRINT Z INFORMATION ******************/
  
  real hthe0;
  if(mesh->get(hthe0, "hthe0") == 0) {
    output.write("    ****NOTE: input from BOUT, Z length needs to be divided by %e\n", hthe0/rho_s);
  }

  /************** NORMALISE QUANTITIES *****************/

  output.write("\tNormalising to rho_s = %e\n", rho_s);

  // Normalise profiles
  Ni0 /= Ni_x/1.0e14;
  Ti0 /= Te_x;
  Te0 /= Te_x;
  
  // Normalise geometry 
  Rxy /= rho_s;
  hthe /= rho_s;
  I *= rho_s*rho_s*(bmag/1e4)*ShearFactor;
  dx /= rho_s*rho_s*(bmag/1e4);

  // Normalise magnetic field
  Bpxy /= (bmag/1.e4);
  Btxy /= (bmag/1.e4);
  Bxy  /= (bmag/1.e4);

  /**************** CALCULATE METRICS ******************/

  g11 = (Rxy*Bpxy)^2;
  g22 = 1.0 / (hthe^2);
  g33 = (I^2)*g11 + (Bxy^2)/g11;
  g12 = 0.0;
  g13 = -I*g11;
  g23 = -Btxy/(hthe*Bpxy*Rxy);
  
  J = hthe / Bpxy;
  
  g_11 = 1.0/g11 + ((I*Rxy)^2);
  g_22 = (Bxy*hthe/Bpxy)^2;
  g_33 = Rxy*Rxy;
  g_12 = Btxy*hthe*I*Rxy/Bpxy;
  g_13 = I*Rxy*Rxy;
  g_23 = Btxy*hthe*Rxy/Bpxy;

  // Twist-shift. NOTE: Should really use qsafe rather than qinty (small correction)

  if((jyseps2_2 / MYSUB) == MYPE) {
    for(int i=0;i<ngx;i++)
      ShiftAngle[i] = zShift[i][MYSUB]; // MYSUB+MYG-1
  }
  if(NYPE > 1)
    MPI_Bcast(ShiftAngle, ngx, PVEC_REAL_MPI_TYPE,jyseps2_2/MYSUB, MPI_COMM_WORLD);
  

  /**************** SET EVOLVING VARIABLES *************/

  // Tell BOUT++ which variables to evolve
  // add evolving variables to the communication object
  bout_solve(rho,   F_rho,   "rho");
  comms.add(rho);
  
  bout_solve(Ni,    F_Ni,    "Ni");
  comms.add(Ni);

  if(evolve_ajpar) {
    bout_solve(Ajpar, F_Ajpar, "Ajpar");
    comms.add(Ajpar);
    output.write("=> Evolving ajpar\n");
  }else {
    output.write("=> Not evolving Apar\n");
    initial_profile("Ajpar", Ajpar);
    if(ZeroElMass)
      dump.add(Ajpar, "Ajpar", 1); // output calculated Ajpar
  }
  
  /************** SETUP COMMUNICATIONS **************/

  // add extra variables to communication
  comms.add(phi);
  comms.add(Apar);

  // Add any other variables to be dumped to file
  dump.add(phi,  "phi",  1);
  dump.add(Apar, "Apar", 1);
  dump.add(jpar, "jpar", 1);

  dump.add(Ni0, "Ni0", 0);
  dump.add(Te0, "Te0", 0);
  dump.add(Ti0, "Ti0", 0);

  dump.add(Te_x,  "Te_x", 0);
  dump.add(Ti_x,  "Ti_x", 0);
  dump.add(Ni_x,  "Ni_x", 0);
  dump.add(rho_s, "rho_s", 0);
  dump.add(wci,   "wci", 0);

  dump.add(zeff, "Zeff", 0);
  dump.add(AA,   "AA",   0);

  com_jp.add(jpar);
  
  return(0);
}

// just define a macro for V_E dot Grad
#define vE_Grad(f, p) ( b0xGrad_dot_Grad(p, f) / Bxy )

int physics_run(real t)
{
  // Solve EM fields

  solve_phi_tridag(rho, phi, 0);

  if(estatic || ZeroElMass) {
    // Electrostatic operation
    
    Apar = 0.0;
  }else {
    solve_apar_tridag(Ajpar, Apar, 0); // Linear Apar solver
  }

  // Communicate variables
  comms.run();

  // Update non-linear coefficients on the mesh
  nu      = nu_hat * Ni0 / (Te0^1.5);
  mu_i    = mui_hat * Ni0 / (Ti0^0.5);

  if(ZeroElMass) {
    // Set jpar,Ve,Ajpar neglecting the electron inertia term

    jpar = ((Te0*Grad_par(Ni)) - (Ni0*Grad_par(phi)))/(fmei*0.51*nu);

    // Set radial boundary condition on jpar
    bndry_inner_flat(jpar);
    bndry_sol_flat(jpar);
    
    // Need to communicate jpar
    com_jp.run();

    Ve = -jpar/Ni0;
    Ajpar = Ve;
  }else {
    // Evolving electron parallel velocity

    if(AparInEpar) {
      // Include Apar term in Eparallel
      Ve = Ajpar + Apar;
    }else
      Ve = Ajpar;
    jpar = -Ni0*Ve;
  }

  // DENSITY EQUATION

  F_Ni = -vE_Grad(Ni0, phi);

  // VORTICITY

  F_rho = Bxy*Bxy*Div_par(jpar);
  
  // AJPAR

  F_Ajpar = 0.0;
  if(evolve_ajpar) {
    F_Ajpar += (1./fmei)*Grad_par(phi);
    F_Ajpar -= (1./fmei)*(Te0/Ni0)*Grad_par(Ni);
    F_Ajpar += 0.51*nu*jpar/Ni0;
  }

  // RADIAL BOUNDARY CONDITIONS

  // Inner (core and PF) - zero gradient
  bndry_inner_flat(F_rho);
  bndry_inner_flat(F_Ni);
  bndry_inner_flat(F_Ajpar);
 
  // Outer (SOL) - zero gradient
  bndry_sol_flat(F_rho);
  bndry_sol_flat(F_Ni);
  bndry_sol_flat(F_Ajpar);

  return(0);
}

/*******************************************************************************
 *                       FAST LINEAR FIELD SOLVERS
 *******************************************************************************/

#include "invert_laplace.h"

// Performs inversion of rho (r) to get phi (p)
int solve_phi_tridag(Field3D &r, Field3D &p, int flags)
{ 
  //output.write( "\n ---------- \n");
  if(invert_laplace(r/Ni0, p, flags, NULL)) {
    return 1;
  }
  return(0);
}

int solve_apar_tridag(Field3D &aj, Field3D &ap, int flags)
{
  static Field2D a;
  static int set = 0;

  if(set == 0) {
    // calculate a
    a = (-0.5*beta_p/fmei)*Ni0;
    set = 1;
  }

  if(invert_laplace(-a*aj, ap, flags, &a))
    return 1;

  return(0);
}
