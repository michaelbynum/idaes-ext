/*------------------------------------------------------------------------------
# Institute for the Design of Advanced Energy Systems Process Systems
# Engineering Framework (IDAES PSE Framework) Copyright (c) 2018-2019, by the
# software owners: The Regents of the University of California, through
# Lawrence Berkeley National Laboratory,  National Technology & Engineering
# Solutions of Sandia, LLC, Carnegie Mellon University, West Virginia
# University Research Corporation, et al. All rights reserved.
#
# Please see the files COPYRIGHT.txt and LICENSE.txt for full copyright and
# license information, respectively. Both files are also available online
# at the URL "https://github.com/IDAES/idaes-pse".
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
 This file contains functions to solve for liquid and vapor denstiy from
 temperature and pressure.  It also contains functions for solving for
 saturation conditions (pressure, liquid density and vapor density).

 Author: John Eslick
 File helmholtz_solve.cpp
------------------------------------------------------------------------------*/

#include<stdio.h>
#include<cmath>
#include<iostream>

#include"helmholtz_memo.h"
#include"helmholtz_config.h"
#include"helmholtz_deriv_parts.h"
#include"helmholtz_solve.h"

/*------------------------------------------------------------------------------
In this section you will find function for calculating density from pressure
and teperature. This lets you calculate properties in the more standard way as
a function of temperature and pressure instead of density and pressure.

Unfortunatly this is difficult and a bit messy.
*-----------------------------------------------------------------------------*/
s_real delta_p_tau_rf(s_real pr, s_real tau, s_real a, s_real b, bool bisect){
  /*----------------------------------------------------------------------------
  Bracketing methods, false position and bisection, for finding a better initial
  guess for density when solving for density from temperature and pressure. This
  is only used in particularly diffucult areas.  At this point it probably
  overused, but there are places where it is probably necessary.

  Args:
    pr: pressure (kPa)
    tau: inverse of reduced pressure Tc/T
    bisect: 1 to use bisection (probably slow), 0 to use false position
            bisection isn't really used, but I kept it around for debugging
    a: first density bound (kg/m3)  | really density, why didn't use delta?
    b: second density bound (kg/m3) | it was easier to think in terms of density
  Returns:
    delta: reduced density at pr and tau (more or less approximate)
  ----------------------------------------------------------------------------*/
  s_real c=a, fa, fb, fc;
  int it = 0;
  // If right by critical point guess critical density. (okay this isn't part
  // of a backeting method but it was conveneint).
  if( fabs(T_c/tau - T_c) < 1e-7 && fabs(pr - P_c) < 1e-4) return 1;
  // solve f(delta, tau) = 0; f(delta, tau) = p(delta, tau) - pr
  a /= rho_c; // convert to reduced density
  b /= rho_c; // convert to reduced density
  fa = p(a, tau) - pr; // initial f(a, tau)
  fb = p(b, tau) - pr; // initial f(b, tau)
  while(it < MAX_IT_BRACKET && (a - b)*(a - b) > TOL_BRACKET){
    if(bisect) c = (a + b)/2.0; // bisection
    else c = b - fb*(b - a)/(fb - fa); //regula falsi
    fc = p(c, tau) - pr; // calcualte f(c)
    if(fc*fa >= 0){a = c; fa = fc;}
    else{b = c; fb = fc;}
    ++it;
  }
  return (a+b)/2.0;
}

s_real delta_p_tau(s_real pr, s_real tau, s_real delta_0, s_real tol, int *nit,
  s_real *grad, s_real *hes){
  /*----------------------------------------------------------------------------
  Halley's method to calculate density from temperature and pressure

  Args:
   pr: pressure (kPa)
   tau: inverse of reduced pressure Tc/T
   delta0: initial guess for delta
   tol: absolute residual tolerance for convergence
   nit: pointer to return number of iterations to, or NULL
   grad: location to return gradient of delta wrt pr and tau or NULL
   hes: location to return hessian (upper triangle) or NULL
  Returns:
   delta: reduced density (accuracy depends on tolarance and function shape)
  ----------------------------------------------------------------------------*/
  s_real delta = delta_0, fun, gradp[2], hesp[3];
  int it = 0; // iteration count
  fun = p_with_derivs(delta, tau, gradp, hesp) - pr;
  while(fabs(fun) > tol && it < MAX_IT_DELTA){
    delta = delta - fun*gradp[0]/(gradp[0]*gradp[0] - 0.5*fun*hesp[0]);
    fun = p_with_derivs(delta, tau, gradp, hesp) - pr;
    ++it;
  }
  if(nit != NULL) *nit = it;
  if(grad != NULL){ // calculate gradient if needed
    grad[0] = 1.0/gradp[0];
    grad[1] = -gradp[1]*grad[0]; //triple product
    if(hes != NULL){ // calculate hession if needed.
      hes[0] = -hesp[0]*grad[0]/gradp[0]/gradp[0];
      hes[1] = -(hesp[1] + hesp[0]*grad[1])/gradp[0]/gradp[0];
      hes[2] = -(grad[0]*(hesp[2] + grad[1]*hesp[1]) + gradp[1]*hes[1]);}}
  return delta;
}

s_real delta_liq(s_real p, s_real tau, s_real *grad, s_real *hes, int *nit){
  /*----------------------------------------------------------------------------
  Get a good liquid or super critical inital density guess then call
  delta_p_tau() to calculate density. In difficult cases an interval with the
  desired soultion is used with a backeting method to produce a better inital
  guess.  There is a area around the critical point and in the super critical
  region where this is hard to solve so there is some pretty extensive code for
  guessing.

  Since solving this takes some time and this function is called a lot with the
  exact same inputs when using the Pyomo wrapper, this function is memoized.

  Args:
   p: pressure (kPa)
   tau: inverse of reduced pressure Tc/T
   grad: location to return gradient of delta wrt pr and tau or NULL
   hes: location to return hessian (upper triangle) of NULL
   nit: pointer to return number of iterations to, or NULL
  Returns:
   delta: reduced density (accuracy depends on tolarance and function shape)
  ----------------------------------------------------------------------------*/
  s_real val = memoize::get_bin(memoize::delta_liq, p, tau, grad, hes);
  if(!std::isnan(val)) return val;
  s_real delta;
  bool free_grad = 0, free_hes = 0; // grad and/or hes not provided so allocate
  // Since I'm going to cache results, grad and hes will get calculated
  // whether requested or not.  If they were NULL allocate space.
  if(grad==NULL){grad = new s_real[2]; free_grad = 1;}
  if(hes==NULL){hes = new s_real[3]; free_hes = 1;}
  delta = delta_p_tau(p, tau, LIQUID_DELTA_GUESS, TOL_DELTA_LIQ, nit, grad, hes); //solve
  if(std::isnan(delta) || delta < 1e-12 || delta > 5.0){
    // This is just to avoid evaluation errors.  Want to be able to calucalte
    // vapor properties even when vapor doesn't exist.  In the IDAES Framework
    // these properties may be calculated and multipled by a zero liquid fraction,
    // so it doesn't mater that they are wrong.
    delta = 3.1;
    zero_derivs2(grad, hes);
  }
  memoize::add_bin(memoize::delta_liq, p, tau, delta, grad, hes); //store
  if(free_grad) delete[] grad; // free grad and hes if not allocated by calling
  if(free_hes) delete[] hes;   //   function
  return delta;
}

s_real delta_vap(s_real p, s_real tau, s_real *grad, s_real *hes, int *nit){
  /*----------------------------------------------------------------------------
  Get a good vapor or super critical inital density guess then call
  delta_p_tau() to calculate density. In the supercritical region this just
  calls the liquid function. In the rest of the vapor region the inital guess
  is pretty easy.

  Since solving this takes some time and this function is called a lot with the
  exact same inputs when using the Pyomo wrapper, this function is memoized.

  Args:
   p: pressure (kPa)
   tau: inverse of reduced pressure Tc/T
   grad: location to return gradient of delta wrt pr and tau or NULL
   hes: location to return hessian (upper triangle) of NULL
   nit: pointer to return number of iterations to, or NULL
  Returns:
   delta: reduced density (accuracy depends on tolarance and function shape)
  ----------------------------------------------------------------------------*/
  s_real val = memoize::get_bin(memoize::DV_FUNC, p, tau, grad, hes);
  if(!std::isnan(val)) return val; // return stored result if available
  s_real delta;
  bool free_grad = 0, free_hes = 0; // grad and/or hes not provided so allocate
  // Since I'm going to cache results, grad and hes will get calculated
  // whether requested or not.  If they were NULL allocate space.
  if(grad==NULL){grad = new s_real[2]; free_grad = 1;}
  if(hes==NULL){hes = new s_real[3]; free_hes = 1;}
  delta = delta_p_tau(p, tau, VAPOR_DELTA_GUESS, TOL_DELTA_VAP, nit, grad, hes);
  if(std::isnan(delta) || delta < 1e-12 || delta > 5.0){
    // This is just to avoid evaluation errors.  Want to be able to calucalte
    // vapor properties even when vapor doesn't exist.  In the IDAES Framework
    // these properties may be calculated and multipled by a zero vapor fraction,
    // so it doesn't mater that they are wrong.
    delta = 0.001;
    zero_derivs2(grad, hes);
  }
  memoize::add_bin(memoize::DV_FUNC, p, tau, delta, grad, hes); // store result
  if(free_grad) delete[] grad; // free grad and hes if not allocated by calling
  if(free_hes) delete[] hes; //   function
  return delta;
}


/*------------------------------------------------------------------------------
In this section you will find functions for calculating the saturation curve.
Staturation pressure and density as a function of temperature.
*-----------------------------------------------------------------------------*/
s_real sat_delta_liq(s_real tau){ //caculate saturated liquid density from tau
  s_real delta_l, delta_v;
  sat(tau, &delta_l, &delta_v);
  return delta_l;
}

s_real sat_delta_vap(s_real tau){ //caculate saturated vapor density from tau
  s_real delta_l, delta_v;
  sat(tau, &delta_l, &delta_v);
  return delta_v;
}

inline s_real J(s_real delta, s_real tau){
  // Term from Akasaka method for saturation state
  return delta*(1+delta*phir_delta(delta, tau));
}

inline s_real K(s_real delta, s_real tau){
  // Term from Akasaka method for saturation state
  return delta*phir_delta(delta,tau) + phir(delta, tau) + log(delta);
}

inline s_real J_delta(s_real delta, s_real tau){
  return 1.0 + 2.0*delta*phir_delta(delta, tau) + delta*delta*phir_delta2(delta, tau);
}

inline s_real K_delta(s_real delta, s_real tau){
  return 2.0*phir_delta(delta, tau) + delta*phir_delta2(delta, tau) + 1.0/delta;
}

inline s_real Delta_Aka(s_real delta_l, s_real delta_v, s_real tau){
  return J_delta(delta_v, tau)*K_delta(delta_l, tau) -
         J_delta(delta_l, tau)*K_delta(delta_v, tau);
}

int sat(s_real tau, s_real *delta_l_sol, s_real *delta_v_sol){
    //Get stautated phase densities at tau by Akasaka (2008) method
    s_real delta_l, delta_v, fg, gradl[1], hesl[1], gradv[1], hesv[1];
    int n=0, max_it=MAX_IT_SAT;

    if(tau - 1 < 1e-12){
      delta_l = 1.0;
      delta_v = 1.0;
      max_it=0;
    }
    else{
      // okay so you've decided to solve this thing
      delta_l = DELTA_LIQ_SAT_GUESS;
      delta_v = DELTA_VAP_SAT_GUESS;
    }
    // Since the equilibrium conditions are gl = gv and pl = pv, I am using the
    // the relative differnce in g as a convergence criteria, that is easy to
    // understand.  fg < tol for convergence, fg is calucalted upfront in the
    // off chance that the guess is the solution
    *delta_l_sol = delta_l; // just in case we don't do at least 1 iteration
    *delta_v_sol = delta_v; // just in case we don't do at least 1 iteration
    fg = fabs((g(delta_v, tau) - g(delta_l, tau))/g(delta_l, tau));
    while(n<max_it && fg > TOL_REL_SAT_G){
      ++n; // Count iterations
      //calculations deltas at next step (Akasaka (2008))
      *delta_l_sol = delta_l + SAT_GAMMA/Delta_Aka(delta_l, delta_v, tau)*(
             (K(delta_v, tau) - K(delta_l, tau))*J_delta(delta_v,tau) -
             (J(delta_v, tau) - J(delta_l, tau))*K_delta(delta_v,tau));
      *delta_v_sol = delta_v + SAT_GAMMA/Delta_Aka(delta_l, delta_v, tau)*(
             (K(delta_v, tau) - K(delta_l, tau))*J_delta(delta_l,tau) -
             (J(delta_v, tau) - J(delta_l, tau))*K_delta(delta_l,tau));
      delta_v = *delta_v_sol; //step
      delta_l = *delta_l_sol;
      //calculate convergence criterium
      fg = fabs((g(delta_v, tau) - g(delta_l, tau))/g(delta_l, tau));
    }

    //Calculate grad and hes for and memoize

    gradv[0] = LHM/LGM;
    gradl[0] = gradv[0]*LBV/LBL + (LCV - LCL)/LBL;
    hesv[0] = LdHdt(delta_l, delta_v, tau, gradl[0], gradv[0])/LGM
             - LHM/LGM/LGM*LdGdt(delta_l, delta_v, tau, gradl[0], gradv[0]);
    hesl[0] = hesv[0]*LBV*LFL + gradv[0]*(LBVt + LBVd*gradv[0])*LFL
              + gradv[0]*LBV*(LFLt + LFLd*gradl[0]) + (LFLt + LFLd*gradl[0])*(LCV - LCL)
              + LFL*(LCVt - LCLt + LCVd*gradv[0] - LCLd*gradl[0]);
    memoize::add_un(memoize::DL_SAT_FUNC, tau, delta_l, gradl, hesl);
    memoize::add_un(memoize::DV_SAT_FUNC, tau, delta_v, gradv, hesv);
    return n;
}
