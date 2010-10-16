/*************************************************************************
*                                                                       *
* Open Dynamics Engine, Copyright (C) 2001-2003 Russell L. Smith.       *
* All rights reserved.  Email: russ@q12.org   Web: www.q12.org          *
*                                                                       *
* This library is free software; you can redistribute it and/or         *
* modify it under the terms of EITHER:                                  *
*   (1) The GNU Lesser General Public License as published by the Free  *
*       Software Foundation; either version 2.1 of the License, or (at  *
*       your option) any later version. The text of the GNU Lesser      *
*       General Public License is included with this library in the     *
*       file LICENSE.TXT.                                               *
*   (2) The BSD-style license that is included with this library in     *
*       the file LICENSE-BSD.TXT.                                       *
*                                                                       *
* This library is distributed in the hope that it will be useful,       *
* but WITHOUT ANY WARRANTY; without even the implied warranty of        *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the files    *
* LICENSE.TXT and LICENSE-BSD.TXT for more details.                     *
*                                                                       *
*************************************************************************/

#include <ode/common.h>
#include <ode/odemath.h>
#include <ode/rotation.h>
#include <ode/timer.h>
#include <ode/error.h>
#include <ode/matrix.h>
#include <ode/misc.h>
#include "config.h"
#include "objects.h"
#include "joints/joint.h"
#include "lcp.h"
#include "util.h"

#include <sys/time.h>



#undef REPORT_THREAD_TIMING
#define USE_TPROW
#undef TIMING
#undef REPORT_MONITOR
#define SHOW_CONVERGENCE
//#define LOCAL_STEPPING  // not yet implemented
#undef RECOMPUTE_RMS
#undef USE_1NORM



#ifdef USE_TPROW
// added for threading per constraint rows
#include <boost/thread/recursive_mutex.hpp>
#include <boost/bind.hpp>
#include "ode/odeinit.h"
#endif

typedef const dReal *dRealPtr;
typedef dReal *dRealMutablePtr;

//***************************************************************************
// configuration

// for the SOR and CG methods:
// uncomment the following line to use warm starting. this definitely
// help for motor-driven joints. unfortunately it appears to hurt
// with high-friction contacts using the SOR method. use with care

//#define WARM_STARTING 1


// for the SOR method:
// uncomment the following line to determine a new constraint-solving
// order for each iteration. however, the qsort per iteration is expensive,
// and the optimal order is somewhat problem dependent.
// @@@ try the leaf->root ordering.

//#define REORDER_CONSTRAINTS 1


// for the SOR method:
// uncomment the following line to randomly reorder constraint rows
// during the solution. depending on the situation, this can help a lot
// or hardly at all, but it doesn't seem to hurt.

#define RANDOMLY_REORDER_CONSTRAINTS 1
#undef LOCK_WHILE_RANDOMLY_REORDER_CONSTRAINTS

#define USE_JOINT_DAMPING

//****************************************************************************
// special matrix multipliers

// multiply block of B matrix (q x 6) with 12 dReal per row with C vektor (q)
static void Multiply1_12q1 (dReal *A, const dReal *B, const dReal *C, int q)
{
  dIASSERT (q>0 && A && B && C);

  dReal a = 0;
  dReal b = 0;
  dReal c = 0;
  dReal d = 0;
  dReal e = 0;
  dReal f = 0;
  dReal s;

  for(int i=0, k = 0; i<q; k += 12, i++)
  {
    s = C[i]; //C[i] and B[n+k] cannot overlap because its value has been read into a temporary.

    //For the rest of the loop, the only memory dependency (array) is from B[]
    a += B[  k] * s;
    b += B[1+k] * s;
    c += B[2+k] * s;
    d += B[3+k] * s;
    e += B[4+k] * s;
    f += B[5+k] * s;
  }

  A[0] = a;
  A[1] = b;
  A[2] = c;
  A[3] = d;
  A[4] = e;
  A[5] = f;
}

//***************************************************************************
// testing stuff

#ifdef TIMING
#define IFTIMING(x) x
#else
#define IFTIMING(x) ((void)0)
#endif

//***************************************************************************
// various common computations involving the matrix J

// compute iMJ = inv(M)*J'

static void compute_invM_JT (int m, dRealPtr J, dRealMutablePtr iMJ, int *jb,
  dxBody * const *body, dRealPtr invI)
{
  dRealMutablePtr iMJ_ptr = iMJ;
  dRealPtr J_ptr = J;
  for (int i=0; i<m; J_ptr += 12, iMJ_ptr += 12, i++) {
    int b1 = jb[i*2];
    int b2 = jb[i*2+1];
    dReal k1 = body[b1]->invMass;
    for (int j=0; j<3; j++) iMJ_ptr[j] = k1*J_ptr[j];
    const dReal *invIrow1 = invI + 12*b1;
    dMultiply0_331 (iMJ_ptr + 3, invIrow1, J_ptr + 3);
    if (b2 >= 0) {
      dReal k2 = body[b2]->invMass;
      for (int j=0; j<3; j++) iMJ_ptr[j+6] = k2*J_ptr[j+6];
      const dReal *invIrow2 = invI + 12*b2;
      dMultiply0_331 (iMJ_ptr + 9, invIrow2, J_ptr + 9);
    }
  }
}

// compute out = inv(M)*J'*in.
#ifdef WARM_STARTING
static void multiply_invM_JT (int m, int nb, dRealMutablePtr iMJ, int *jb,
  dRealPtr in, dRealMutablePtr out)
{
  dSetZero (out,6*nb);
  dRealPtr iMJ_ptr = iMJ;
  for (int i=0; i<m; i++) {
    int b1 = jb[i*2];
    int b2 = jb[i*2+1];
    const dReal in_i = in[i];
    dRealMutablePtr out_ptr = out + b1*6;
    for (int j=0; j<6; j++) out_ptr[j] += iMJ_ptr[j] * in_i;
    iMJ_ptr += 6;
    if (b2 >= 0) {
      out_ptr = out + b2*6;
      for (int j=0; j<6; j++) out_ptr[j] += iMJ_ptr[j] * in_i;
    }
    iMJ_ptr += 6;
  }
}
#endif

// compute out = J*in.

static void multiply_J (int m, dRealPtr J, int *jb,
  dRealPtr in, dRealMutablePtr out)
{
  dRealPtr J_ptr = J;
  for (int i=0; i<m; i++) {
    int b1 = jb[i*2];
    int b2 = jb[i*2+1];
    dReal sum = 0;
    dRealPtr in_ptr = in + b1*6;
    for (int j=0; j<6; j++) sum += J_ptr[j] * in_ptr[j];
    J_ptr += 6;
    if (b2 >= 0) {
      in_ptr = in + b2*6;
      for (int j=0; j<6; j++) sum += J_ptr[j] * in_ptr[j];
    }
    J_ptr += 6;
    out[i] = sum;
  }
}


// compute out = (J*inv(M)*J' + cfm)*in.
// use z as an nb*6 temporary.
#ifdef WARM_STARTING
static void multiply_J_invM_JT (int m, int nb, dRealMutablePtr J, dRealMutablePtr iMJ, int *jb,
  dRealPtr cfm, dRealMutablePtr z, dRealMutablePtr in, dRealMutablePtr out)
{
  multiply_invM_JT (m,nb,iMJ,jb,in,z);
  multiply_J (m,J,jb,z,out);

  // add cfm
  for (int i=0; i<m; i++) out[i] += cfm[i] * in[i];
}
#endif

//***************************************************************************
// conjugate gradient method with jacobi preconditioner
// THIS IS EXPERIMENTAL CODE that doesn't work too well, so it is ifdefed out.
//
// adding CFM seems to be critically important to this method.

#ifdef USE_CG_LCP

static inline dReal dot (int n, dRealPtr x, dRealPtr y)
{
  dReal sum=0;
  for (int i=0; i<n; i++) sum += x[i]*y[i];
  return sum;
}


// x = y + z*alpha

static inline void add (int n, dRealMutablePtr x, dRealPtr y, dRealPtr z, dReal alpha)
{
  for (int i=0; i<n; i++) x[i] = y[i] + z[i]*alpha;
}

static void CG_LCP (dxWorldProcessContext *context,
  int m, int nb, dRealMutablePtr J, int *jb, dxBody * const *body,
  dRealPtr invI, dRealMutablePtr lambda, dRealMutablePtr fc, dRealMutablePtr b,
  dRealMutablePtr lo, dRealMutablePtr hi, dRealPtr cfm, int *findex,
  dxQuickStepParameters *qs)
{
  const int num_iterations = qs->num_iterations;

  // precompute iMJ = inv(M)*J'
  dReal *iMJ = context->AllocateArray<dReal> (m*12);
  compute_invM_JT (m,J,iMJ,jb,body,invI);

  dReal last_rho = 0;
  dReal *r = context->AllocateArray<dReal> (m);
  dReal *z = context->AllocateArray<dReal> (m);
  dReal *p = context->AllocateArray<dReal> (m);
  dReal *q = context->AllocateArray<dReal> (m);

  // precompute 1 / diagonals of A
  dReal *Ad = context->AllocateArray<dReal> (m);
  dRealPtr iMJ_ptr = iMJ;
  dRealPtr J_ptr = J;
  for (int i=0; i<m; i++) {
    dReal sum = 0;
    for (int j=0; j<6; j++) sum += iMJ_ptr[j] * J_ptr[j];
    if (jb[i*2+1] >= 0) {
      for (int j=6; j<12; j++) sum += iMJ_ptr[j] * J_ptr[j];
    }
    iMJ_ptr += 12;
    J_ptr += 12;
    Ad[i] = REAL(1.0) / (sum + cfm[i]);
  }

#ifdef WARM_STARTING
  // compute residual r = b - A*lambda
  multiply_J_invM_JT (m,nb,J,iMJ,jb,cfm,fc,lambda,r);
  for (int k=0; k<m; k++) r[k] = b[k] - r[k];
#else
  dSetZero (lambda,m);
  memcpy (r,b,m*sizeof(dReal));		// residual r = b - A*lambda
#endif

  for (int iteration=0; iteration < num_iterations; iteration++) {
    for (int i=0; i<m; i++) z[i] = r[i]*Ad[i];	// z = inv(M)*r
    dReal rho = dot (m,r,z);		// rho = r'*z

    // @@@
    // we must check for convergence, otherwise rho will go to 0 if
    // we get an exact solution, which will introduce NaNs into the equations.
    if (rho < 1e-10) {
      printf ("CG returned at iteration %d\n",iteration);
      break;
    }

    if (iteration==0) {
      memcpy (p,z,m*sizeof(dReal));	// p = z
    }
    else {
      add (m,p,z,p,rho/last_rho);	// p = z + (rho/last_rho)*p
    }

    // compute q = (J*inv(M)*J')*p
    multiply_J_invM_JT (m,nb,J,iMJ,jb,cfm,fc,p,q);

    dReal alpha = rho/dot (m,p,q);		// alpha = rho/(p'*q)
    add (m,lambda,lambda,p,alpha);		// lambda = lambda + alpha*p
    add (m,r,r,q,-alpha);			// r = r - alpha*q
    last_rho = rho;
  }

  // compute fc = inv(M)*J'*lambda
  multiply_invM_JT (m,nb,iMJ,jb,lambda,fc);

#if 0
  // measure solution error
  multiply_J_invM_JT (m,nb,J,iMJ,jb,cfm,fc,lambda,r);
  dReal error = 0;
  for (int i=0; i<m; i++) error += dFabs(r[i] - b[i]);
  printf ("lambda error = %10.6e\n",error);
#endif
}

#endif

//***************************************************************************
// SOR-LCP method

// nb is the number of bodies in the body array.
// J is an m*12 matrix of constraint rows
// jb is an array of first and second body numbers for each constraint row
// invI is the global frame inverse inertia for each body (stacked 3x3 matrices)
//
// this returns lambda and fc (the constraint force).
// note: fc is returned as inv(M)*J'*lambda, the constraint force is actually J'*lambda
//
// b, lo and hi are modified on exit

struct IndexError {
#ifdef REORDER_CONSTRAINTS
  dReal error;		// error to sort on
  int findex;
#endif
  int index;		// row index
};


#ifdef REORDER_CONSTRAINTS

static int compare_index_error (const void *a, const void *b)
{
  const IndexError *i1 = (IndexError*) a;
  const IndexError *i2 = (IndexError*) b;
  if (i1->findex < 0 && i2->findex >= 0) return -1;
  if (i1->findex >= 0 && i2->findex < 0) return 1;
  if (i1->error < i2->error) return -1;
  if (i1->error > i2->error) return 1;
  return 0;
}

#endif

static void ComputeRows(
                int thread_id,
                IndexError* order,
                dxBody* const *body,
                int* tmpInt,
                dReal* tmpReal,
                const int** tmpIntPtr,
                dRealPtr* tmpRealPtr,
                dRealMutablePtr* tmpMutablePtr,
                boost::recursive_mutex* mutex)
{
  struct timeval tv;
  double cur_time;
  gettimeofday(&tv,NULL);
  cur_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  //printf("thread %d started at time %f\n",thread_id,cur_time);

  //boost::recursive_mutex::scoped_lock lock(*mutex); // put in fc read/writes?
  int startRow           = tmpInt[0];
  int nRows              = tmpInt[1];
  int m                  = tmpInt[2];
  int nb                 = tmpInt[3];
  int m_damp             = tmpInt[4];
  int num_iterations     = tmpInt[5];
  dReal stepsize         = tmpReal[0];
  dReal sor_lcp_tolerance= tmpReal[1];
  const int* jb                = tmpIntPtr[0];
  const int* findex            = tmpIntPtr[1];
  const int* jb_damp           = tmpIntPtr[2];
  dRealPtr        Ad           = tmpRealPtr[0];
  dRealPtr        hi           = tmpRealPtr[1];
  dRealPtr        lo           = tmpRealPtr[2];
  dRealPtr        Adcfm        = tmpRealPtr[3];
  dRealPtr        JiM          = tmpRealPtr[4];
  dRealPtr        invI         = tmpRealPtr[5];
  dRealPtr        coeff_damp   = tmpRealPtr[6];
  dRealMutablePtr b            = tmpMutablePtr[0];
  dRealMutablePtr J            = tmpMutablePtr[1];
  dRealMutablePtr fc           = tmpMutablePtr[2];
  dRealMutablePtr lambda       = tmpMutablePtr[3];
  dRealMutablePtr iMJ          = tmpMutablePtr[4];
#ifdef USE_JOINT_DAMPING
  dRealMutablePtr b_damp       = tmpMutablePtr[5];
  dRealMutablePtr f_damp       = tmpMutablePtr[6];
  dRealMutablePtr v_damp       = tmpMutablePtr[7];
  dRealMutablePtr J_damp       = tmpMutablePtr[8];
  dRealMutablePtr v_joint_damp = tmpMutablePtr[9];
#ifdef REORDER_CONSTRAINTS
  dRealMutablePtr last_lambda  = tmpMutablePtr[10];
#endif
#endif
  dRealMutablePtr delta_error  = tmpMutablePtr[11];

  //printf("iiiiiiiii %d %d %d\n",thread_id,jb[0],jb[1]);
  //for (int i=startRow; i<startRow+nRows; i++) // swap within boundary of our own segment
  //  printf("wwwwwwwwwwwww>id %d start %d n %d  order[%d].index=%d\n",thread_id,startRow,nRows,i,order[i].index);

  for (int iteration=0; iteration < num_iterations; iteration++) {

#ifdef REORDER_CONSTRAINTS
    // constraints with findex < 0 always come first.
    if (iteration < 2) {
      // for the first two iterations, solve the constraints in
      // the given order
      IndexError *ordercurr = order+startRow;
      //for (int i = 0; i != m; ordercurr++, i++) { }
      for (int i = startRow; i != startRow+nRows; ordercurr++, i++) {
        ordercurr->error = i;
        ordercurr->findex = findex[i];
        ordercurr->index = i;
      }
    }
    else {
      // sort the constraints so that the ones converging slowest
      // get solved last. use the absolute (not relative) error.
      //for (int i=0; i<m; i++) { }
      for (int i=startRow; i<startRow+nRows; i++) {
        dReal v1 = dFabs (lambda[i]);
        dReal v2 = dFabs (last_lambda[i]);
        dReal max = (v1 > v2) ? v1 : v2;
        if (max > 0) {
          //@@@ relative error: order[i].error = dFabs(lambda[i]-last_lambda[i])/max;
          order[i].error = dFabs(lambda[i]-last_lambda[i]);
        }
        else {
          order[i].error = dInfinity;
        }
        order[i].findex = findex[i];
        order[i].index = i;
      }
    }

    //if (thread_id == 0) for (int i=startRow;i<startRow+nRows;i++) printf("=====> %d %d %d %f %d\n",thread_id,iteration,i,order[i].error,order[i].index);

    //qsort (order,m,sizeof(IndexError),&compare_index_error);
    qsort (order+startRow,nRows,sizeof(IndexError),&compare_index_error);

    //@@@ potential optimization: swap lambda and last_lambda pointers rather
    //    than copying the data. we must make sure lambda is properly
    //    returned to the caller
    //memcpy (last_lambda,lambda,m*sizeof(dReal));
    memcpy (last_lambda+startRow,lambda+startRow,nRows*sizeof(dReal));

    //if (thread_id == 0) for (int i=startRow;i<startRow+nRows;i++) printf("-----> %d %d %d %f %d\n",thread_id,iteration,i,order[i].error,order[i].index);

#endif
#ifdef RANDOMLY_REORDER_CONSTRAINTS
    if ((iteration & 7) == 0) {
      #ifdef LOCK_WHILE_RANDOMLY_REORDER_CONSTRAINTS
        boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every swap
      #endif
      //for (int i=1; i<m; i++) {}   // swap across engire matrix
      //  int swapi = dRandInt(i+1); // swap across engire matrix
      for (int i=startRow+1; i<startRow+nRows; i++) { // swap within boundary of our own segment
        int swapi = dRandInt(i+1-startRow)+startRow; // swap within boundary of our own segment
        //printf("xxxxxxxx>id %d swaping order[%d].index=%d order[%d].index=%d\n",thread_id,i,order[i].index,swapi,order[swapi].index);
        IndexError tmp = order[i];
        order[i] = order[swapi];
        order[swapi] = tmp;
      }

      // {
      //   // verify
      //   boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every row
      //   printf("  random id %d iter %d\n",thread_id,iteration);
      //   for (int i=startRow+1; i<startRow+nRows; i++)
      //     printf(" %5d,",i);
      //   printf("\n");
      //   for (int i=startRow+1; i<startRow+nRows; i++)
      //     printf(" %5d;",(int)order[i].index);
      //   printf("\n");
      // }
    }
#endif

    //dSetZero (delta_error,m);
    dReal rms_error = 0;

    for (int i=startRow; i<startRow+nRows; i++) {

      //boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every row

      // @@@ potential optimization: we could pre-sort J and iMJ, thereby
      //     linearizing access to those arrays. hmmm, this does not seem
      //     like a win, but we should think carefully about our memory
      //     access pattern.

      int index = order[i].index;

      dRealMutablePtr fc_ptr1;
      dRealMutablePtr fc_ptr2;
      dReal delta;

      {
        int b1 = jb[index*2];
        int b2 = jb[index*2+1];
        fc_ptr1 = fc + 6*b1;
        fc_ptr2 = (b2 >= 0) ? fc + 6*b2 : NULL;
      }

#ifdef USE_JOINT_DAMPING
      /*************************************************************/
      /* compute b_damp                                            */
      /* b is to be modified by b_damp                             */
      /* where b_damp = -J*inv(M)*f_damp / Ad  (since b is rhs/Ad) */
      /*                                                           */
      /* initially f_damp is 0, so motion is undamped on first     */
      /* iteration.                                                */
      /*                                                           */
      /*************************************************************/
      {
        b_damp[index] = 0;
        int b1 = jb[index*2];
        int b2 = jb[index*2+1];
        dRealMutablePtr f_damp_ptr1 = f_damp + 6*b1;
        dRealMutablePtr f_damp_ptr2 = (b2 >= 0) ? f_damp + 6*b2 : NULL;
   
        dRealPtr JiM_ptr = JiM + index*12;

        // compute b_damp = JiM * f_damp, b_damp is preset to zero already
        for (int j=0;j<6;j++) {
          b_damp[index] += JiM_ptr[j] * f_damp_ptr1[j];
          if (b2>=0) b_damp[index] += JiM_ptr[j+6] * f_damp_ptr2[j];
        }
   
        // and scale JiM by Ad
        b_damp[index] *= Ad[index];
        // FIXME: find some kind of limiters that works as artificial dampers
        // b_damp must make b smaller
        // so b_damp must have opposite sign as b
        // and abs(b_damp) < abs(b)
        //if (b_damp[index]*b[index]>0) b_damp[index]=0;
        //if (dFabs(b_damp[index])>dFabs(b[index])) b_damp[index]=-b[index];
      }
#endif

      dReal old_lambda = lambda[index];

      {
        delta = b[index] - old_lambda*Adcfm[index];
#ifdef USE_JOINT_DAMPING
        /***************************************************************************/
        /* b is to be modified by b_damp = -J*inv(M)*f_damp / Ad since b is rhs/Ad */
        /***************************************************************************/
        delta += b_damp[index];
#endif

        dRealPtr J_ptr = J + index*12;
        // @@@ potential optimization: SIMD-ize this and the b2 >= 0 case
        delta -=fc_ptr1[0] * J_ptr[0] + fc_ptr1[1] * J_ptr[1] +
          fc_ptr1[2] * J_ptr[2] + fc_ptr1[3] * J_ptr[3] +
          fc_ptr1[4] * J_ptr[4] + fc_ptr1[5] * J_ptr[5];
        // @@@ potential optimization: handle 1-body constraints in a separate
        //     loop to avoid the cost of test & jump?
        if (fc_ptr2) {
          delta -=fc_ptr2[0] * J_ptr[6] + fc_ptr2[1] * J_ptr[7] +
            fc_ptr2[2] * J_ptr[8] + fc_ptr2[3] * J_ptr[9] +
            fc_ptr2[4] * J_ptr[10] + fc_ptr2[5] * J_ptr[11];
        }
      }

      {
        dReal hi_act, lo_act;

        // set the limits for this constraint. 
        // this is the place where the QuickStep method differs from the
        // direct LCP solving method, since that method only performs this
        // limit adjustment once per time step, whereas this method performs
        // once per iteration per constraint row.
        // the constraints are ordered so that all lambda[] values needed have
        // already been computed.
        if (findex[index] >= 0) {
          hi_act = dFabs (hi[index] * lambda[findex[index]]);
          lo_act = -hi_act;
        } else {
          hi_act = hi[index];
          lo_act = lo[index];
        }

        // compute lambda and clamp it to [lo,hi].
        // @@@ potential optimization: does SSE have clamping instructions
        //     to save test+jump penalties here?
        dReal new_lambda = old_lambda + delta;
        if (new_lambda < lo_act) {
          delta = lo_act-old_lambda;
          lambda[index] = lo_act;
        }
        else if (new_lambda > hi_act) {
          delta = hi_act-old_lambda;
          lambda[index] = hi_act;
        }
        else {
          lambda[index] = new_lambda;
        }
      }

      rms_error += delta*delta;
      delta_error[index] = dFabs(delta);

      //@@@ a trick that may or may not help
      //dReal ramp = (1-((dReal)(iteration+1)/(dReal)num_iterations));
      //delta *= ramp;
      
      {
        // no longer need iMJ, just need J' here
        dRealPtr iMJ_ptr = iMJ + index*12;
        // update fc.
        // @@@ potential optimization: SIMD for this and the b2 >= 0 case
        fc_ptr1[0] += delta * iMJ_ptr[0];
        fc_ptr1[1] += delta * iMJ_ptr[1];
        fc_ptr1[2] += delta * iMJ_ptr[2];
        fc_ptr1[3] += delta * iMJ_ptr[3];
        fc_ptr1[4] += delta * iMJ_ptr[4];
        fc_ptr1[5] += delta * iMJ_ptr[5];
        // @@@ potential optimization: handle 1-body constraints in a separate
        //     loop to avoid the cost of test & jump?
        if (fc_ptr2) {
          fc_ptr2[0] += delta * iMJ_ptr[6];
          fc_ptr2[1] += delta * iMJ_ptr[7];
          fc_ptr2[2] += delta * iMJ_ptr[8];
          fc_ptr2[3] += delta * iMJ_ptr[9];
          fc_ptr2[4] += delta * iMJ_ptr[10];
          fc_ptr2[5] += delta * iMJ_ptr[11];
        }
      }
    } // end of for loop on m


// do we need to compute norm across entire solution space (0,m)?
// since local convergence might produce errors in other nodes?
#ifdef RECOMPUTE_RMS
    // recompute rms_error to be sure swap is not corrupting arrays
    rms_error = 0;
    #ifdef USE_1NORM
        //for (int i=startRow; i<startRow+nRows; i++)
        for (int i=0; i<m; i++)
        {
          rms_error = dFabs(delta_error[order[i].index]) > rms_error ? dFabs(delta_error[order[i].index]) : rms_error; // 1norm test
        }
    #else // use 2 norm
        //for (int i=startRow; i<startRow+nRows; i++)
        for (int i=0; i<m; i++)  // use entire solution vector errors
          rms_error += delta_error[order[i].index]*delta_error[order[i].index]; ///(dReal)nRows;
        rms_error = sqrt(rms_error); ///(dReal)nRows;
    #endif
#else
    rms_error = sqrt(rms_error); ///(dReal)nRows;
#endif

    //printf("------ %d %d %20.18f\n",thread_id,iteration,rms_error);

    //for (int i=startRow; i<startRow+nRows; i++) printf("debug: %d %f\n",i,delta_error[i]);


    //{
    //  // verify
    //  boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every row
    //  printf("  random id %d iter %d\n",thread_id,iteration);
    //  for (int i=startRow+1; i<startRow+nRows; i++)
    //    printf(" %10d,",i);
    //  printf("\n");
    //  for (int i=startRow+1; i<startRow+nRows; i++)
    //    printf(" %10d;",order[i].index);
    //  printf("\n");
    //  for (int i=startRow+1; i<startRow+nRows; i++)
    //    printf(" %10.8f,",delta_error[i]);
    //  printf("\n%f\n",rms_error);
    //}


#ifdef USE_JOINT_DAMPING
    /****************************************************************/
    /* compute v_damp per fc update                                 */
    /*   based on all external forces fe, fc, f_damp                */
    /*   v_damp should have started out same as v(n)                */
    /*   v_damp should end up being v(n+1)                          */
    /*                                                              */
    /*  v_damp = v_current + stepsize * invM * f_all                */
    /*                                                              */
    /****************************************************************/
    {
      const dReal *invIrow = invI;
      dRealMutablePtr f_damp_ptr = f_damp;
      dRealMutablePtr v_damp_ptr = v_damp;
      dxBody *const *const bodyend = body + nb;
      const dReal *fc_ptr = fc;

      for (dxBody *const *bodycurr = body; bodycurr != bodyend; fc_ptr+=6, invIrow += 12, f_damp_ptr+=6, v_damp_ptr+=6, bodycurr++) {
        // f_damp should be updated in SOR LCP

        // compute the velocity update:
        // add stepsize * invM * f_damp to the body velocity
        dxBody *b = *bodycurr;
        dReal body_invMass_mul_stepsize = stepsize * b->invMass;
        dReal tmp3[3];
        for (int j=0; j<3; j++) {
          // note that cforce(fc) is really not a force but an acceleration, hence there is
          // no premultiplying of invM here (compare to update due to external force 'facc' below)
          // add stepsize * cforce(fc) to the body velocity
          v_damp_ptr[j]   = b->lvel[j] + stepsize * fc_ptr[j]   + body_invMass_mul_stepsize * ( b->facc[j] + f_damp_ptr[j] );
          v_damp_ptr[j+3] = b->avel[j] + stepsize * fc_ptr[j+3];

          // accumulate step*torques
          tmp3[j] = stepsize*(b->tacc[j] + f_damp_ptr[j+3]);
        }
        // v_damp = invI * f_damp
        dMultiplyAdd0_331 (v_damp_ptr+3, invIrow, tmp3);

      }
    }

    /****************************************************************/
    /* compute f_damp per v_damp update                             */
    /* compute damping force f_damp = J_damp' * B * J_damp * v_damp */
    /*                                                              */
    /*  we probably want to apply some kind of limiter on f_damp    */
    /*  based on changes in v_damp.                                 */
    /*                                                              */
    /*  for starters, ramp up damping to increase stability.        */
    /*                                                              */
    /****************************************************************/
    {
      dSetZero (f_damp,6*nb); // reset f_damp, following update skips around, so cannot set to 0 inline
      dRealPtr J_damp_ptr = J_damp;
      // compute f_damp and velocity updates
      // first compute v_joint_damp = J_damp * v_damp
      // v_joint_damp is m_damp X 1 single column vector
      for (int j=0; j<m_damp;J_damp_ptr+=12, j++) {
        int b1 = jb_damp[j*2];
        int b2 = jb_damp[j*2+1];
        v_joint_damp[j] = 0;

        // ramp-up : option to skip first few iterations to let the joint settle first
        int skip = 10; //num_iterations-1;
        dReal alpha = (iteration>=skip)?(dReal)(iteration-skip+1) / (dReal)(num_iterations-skip):0;

        for (int k=0;k<6;k++) v_joint_damp[j] += alpha*J_damp_ptr[k] * v_damp[b1*6+k];
        if (b2 >= 0) for (int k=0;k<6;k++) v_joint_damp[j] += alpha*J_damp_ptr[k+6] * v_damp[b2*6+k];
        // multiply by damping coefficients (B is diagnoal)
        v_joint_damp[j] *= coeff_damp[j];

        // so now v_joint_damp = B * J_damp * v_damp
        // update f_damp = J_damp' * v_joint_damp
        for (int k=0; k<6; k++) f_damp[b1*6+k] -= J_damp_ptr[k]*v_joint_damp[j];
        if (b2 >= 0) for (int k=0; k<6; k++) f_damp[b2*6+k] -= J_damp_ptr[6+k]*v_joint_damp[j];

        //if (v_joint_damp[j] < 1000)
        //  printf("ITER: %d j: %d m1: %f m2: %f v: %f\n",iteration,j,1.0/body[b1]->invMass,1.0/body[b2]->invMass,v_joint_damp[j]);
      }

    }
#endif

#ifdef SHOW_CONVERGENCE
    printf("MONITOR: id: %d iteration: %d error: %20.16f\n",thread_id,iteration,rms_error);
#endif

    if (rms_error < sor_lcp_tolerance)
    {
      #ifdef REPORT_MONITOR
        printf("CONVERGED: id: %d steps: %d rms(%20.18f)\n",thread_id,iteration,rms_error);
      #endif
      break;
    }
    else if (iteration == num_iterations -1)
    {
      #ifdef REPORT_MONITOR
        printf("**********ERROR: id: %d did not converge in %d steps, rms(%20.18f)\n",thread_id,num_iterations,rms_error);
      #endif
    }

  } // end of for loop on iterations

  gettimeofday(&tv,NULL);
  double end_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  #ifdef REPORT_THREAD_TIMING
  printf("      quickstep row thread %d start time %f ended time %f duration %f\n",thread_id,cur_time,end_time,end_time - cur_time);
  #endif
}

static void SOR_LCP (dxWorldProcessContext *context,
  const int m, const int nb, dRealMutablePtr J, int *jb, dxBody * const *body,
  dRealPtr invI, dRealMutablePtr lambda, dRealMutablePtr fc, dRealMutablePtr b,
  dRealPtr lo, dRealPtr hi, dRealPtr cfm, const int *findex,
  const dxQuickStepParameters *qs,
#ifdef USE_JOINT_DAMPING
  const int m_damp,dRealMutablePtr J_damp, dRealPtr coeff_damp, int *jb_damp,dRealMutablePtr v_damp,
  dRealMutablePtr f_damp,dRealMutablePtr v_joint_damp, dRealPtr JiM, // damping related
#endif
#ifdef USE_TPROW
  boost::threadpool::pool* row_threadpool,
#endif
  const dReal stepsize) // for updating v_damp along the way
{
#ifdef WARM_STARTING
  {
    // for warm starting, this seems to be necessary to prevent
    // jerkiness in motor-driven joints. i have no idea why this works.
    for (int i=0; i<m; i++) lambda[i] *= 0.9;
  }
#else
  dSetZero (lambda,m);
#endif

  // precompute iMJ = inv(M)*J'
  dReal *iMJ = context->AllocateArray<dReal> (m*12); // no longer needed
  //compute_invM_JT (m,J,iMJ,jb,body,invI);
  // iMJ should really just be J
  dRealMutablePtr iMJ_ptr = iMJ;
  dRealPtr J_ptr = J;
  for (int i=0; i<m; J_ptr += 12, iMJ_ptr += 12, i++)
    for (int j=0; j<12; j++) iMJ_ptr[j] = J_ptr[j];


  // compute fc=(inv(M)*J')*lambda. we will incrementally maintain fc
  // as we change lambda.
#ifdef WARM_STARTING
  multiply_invM_JT (m,nb,iMJ,jb,lambda,fc);
#else
  dSetZero (fc,nb*6);
#endif

  dReal *Ad = context->AllocateArray<dReal> (m);

  {
    const dReal sor_w = qs->w;		// SOR over-relaxation parameter
    // precompute 1 / diagonals of A
    dRealPtr iMJ_ptr = iMJ;
    dRealPtr J_ptr = J;
    for (int i=0; i<m; J_ptr += 12, iMJ_ptr += 12, i++) {
      dReal sum = 0;
      for (int j=0; j<6; j++) sum += iMJ_ptr[j] * J_ptr[j];
      if (jb[i*2+1] >= 0) {
        for (int k=6; k<12; k++) sum += iMJ_ptr[k] * J_ptr[k];
      }
      Ad[i] = sor_w / (sum + cfm[i]);
    }
  }


  /********************************/
  /* allocate for J*invM*f_damp   */
  /* which is a mX1 column vector */
  /********************************/
  dReal *Adcfm = context->AllocateArray<dReal> (m);


  {
    // NOTE: This may seem unnecessary but it's indeed an optimization 
    // to move multiplication by Ad[i] and cfm[i] out of iteration loop.

    // scale J and b by Ad
    dRealMutablePtr J_ptr = J;
    for (int i=0; i<m; J_ptr += 12, i++) {
      dReal Ad_i = Ad[i];
      for (int j=0; j<12; j++) {
        J_ptr[j] *= Ad_i;
      }
      b[i] *= Ad_i;
      // scale Ad by CFM. N.B. this should be done last since it is used above
      Adcfm[i] = Ad_i * cfm[i];
    }
  }


  // order to solve constraint rows in
  IndexError *order = context->AllocateArray<IndexError> (m);

  dReal *delta_error = context->AllocateArray<dReal> (m);

#ifndef REORDER_CONSTRAINTS
  {
    // make sure constraints with findex < 0 come first.
    IndexError *orderhead = order, *ordertail = order + (m - 1);

    // Fill the array from both ends
    for (int i=0; i<m; i++) {
      if (findex[i] < 0) {
        orderhead->index = i; // Place them at the front
        ++orderhead;
      } else {
        ordertail->index = i; // Place them at the end
        --ordertail;
      }
    }
    dIASSERT (orderhead-ordertail==1);
  }
#endif

#ifdef REORDER_CONSTRAINTS
  // the lambda computed at the previous iteration.
  // this is used to measure error for when we are reordering the indexes.
  dReal *last_lambda = context->AllocateArray<dReal> (m);
#endif

#ifdef USE_JOINT_DAMPING
  dReal *b_damp = context->AllocateArray<dReal> (m);
#endif


  boost::recursive_mutex* mutex = new boost::recursive_mutex();

  const int num_iterations = qs->num_iterations;
  // single iteration, through all the constraints









  int num_chunks = qs->num_chunks > 0 ? qs->num_chunks : 1; // min is 1

  // prepare pointers for threads
  int tmpInt_size = 6;
  int tmpInt[tmpInt_size*num_chunks];
  int tmpReal_size = 2;
  dReal tmpReal[tmpReal_size*num_chunks];
  int tmpIntPtr_size = 3;
  const int* tmpIntPtr[tmpIntPtr_size*num_chunks];
  int tmpRealPtr_size = 7;
  dRealPtr tmpRealPtr[tmpRealPtr_size*num_chunks];
  int tmpMutablePtr_size = 12;
  dRealMutablePtr tmpMutablePtr[tmpMutablePtr_size*num_chunks];

  int num_overlap = qs->num_overlap;
  int chunk = m / num_chunks+1;
  chunk = chunk > 0 ? chunk : 1;
  int thread_id = 0;




  struct timeval tv;
  double cur_time;
  gettimeofday(&tv,NULL);
  cur_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  //printf("    quickstep start threads at time %f\n",cur_time);




  IFTIMING (dTimerNow ("start pgs rows"));
  for (int i=0; i<m; i+= chunk,thread_id++)
  {
    //for (int ijk=0;ijk<m;ijk++) printf("aaaaaaaaaaaaaaaaaaaaa> id:%d jb[%d]=%d\n",thread_id,ijk,jb[ijk]);

    int nStart = i - num_overlap < 0 ? 0 : i - num_overlap;
    int nEnd   = i + chunk + num_overlap;
    if (nEnd > m) nEnd = m;
    // if every one reorders constraints, this might just work
    // comment out below if using defaults (0 and m) so every thread runs through all joints
    tmpInt[0+thread_id*tmpInt_size] = nStart;   // 0
    tmpInt[1+thread_id*tmpInt_size] = nEnd - nStart; // m
    tmpInt[2+thread_id*tmpInt_size] = m; // m
    tmpInt[3+thread_id*tmpInt_size] = nb;
    tmpInt[4+thread_id*tmpInt_size] = m_damp;
    tmpInt[5+thread_id*tmpInt_size] = num_iterations;
    tmpReal[0+thread_id*tmpReal_size] = stepsize;
    tmpReal[1+thread_id*tmpReal_size] = qs->sor_lcp_tolerance;
    tmpIntPtr[0+thread_id*tmpIntPtr_size] = jb;
    tmpIntPtr[1+thread_id*tmpIntPtr_size] = findex;
    tmpIntPtr[2+thread_id*tmpIntPtr_size] = jb_damp;
    tmpRealPtr[0+thread_id*tmpRealPtr_size] = Ad;
    tmpRealPtr[1+thread_id*tmpRealPtr_size] = hi;
    tmpRealPtr[2+thread_id*tmpRealPtr_size] = lo;
    tmpRealPtr[3+thread_id*tmpRealPtr_size] = Adcfm;
    tmpRealPtr[4+thread_id*tmpRealPtr_size] = JiM;
    tmpRealPtr[5+thread_id*tmpRealPtr_size] = invI;
    tmpRealPtr[6+thread_id*tmpRealPtr_size] = coeff_damp ;
    tmpMutablePtr[0+thread_id*tmpMutablePtr_size] = b;
    tmpMutablePtr[1+thread_id*tmpMutablePtr_size] = J;
    tmpMutablePtr[2+thread_id*tmpMutablePtr_size] = fc;
    tmpMutablePtr[3+thread_id*tmpMutablePtr_size] = lambda;
    tmpMutablePtr[4+thread_id*tmpMutablePtr_size] = iMJ;
    #ifdef USE_JOINT_DAMPING
      tmpMutablePtr[5+thread_id*tmpMutablePtr_size] = b_damp;
      tmpMutablePtr[6+thread_id*tmpMutablePtr_size] = f_damp;
      tmpMutablePtr[7+thread_id*tmpMutablePtr_size] = v_damp;
      tmpMutablePtr[8+thread_id*tmpMutablePtr_size] = J_damp;
      tmpMutablePtr[9+thread_id*tmpMutablePtr_size] = v_joint_damp;
      #ifdef REORDER_CONSTRAINTS
        tmpMutablePtr[10+thread_id*tmpMutablePtr_size] = last_lambda;
      #endif
    #endif
    tmpMutablePtr[11+thread_id*tmpMutablePtr_size] = delta_error ;

#ifdef REPORT_MONITOR
    printf("thread summary: id %d i %d m %d chunk %d start %d end %d \n",thread_id,i,m,chunk,nStart,nEnd);
#endif
#ifdef USE_TPROW
    if (row_threadpool && row_threadpool->size() > 0)
      row_threadpool->schedule(boost::bind(ComputeRows,thread_id,order, body, tmpInt+thread_id*tmpInt_size,
                           tmpReal+thread_id*tmpReal_size, tmpIntPtr+thread_id*tmpIntPtr_size,
                           tmpRealPtr+thread_id*tmpRealPtr_size, tmpMutablePtr+thread_id*tmpMutablePtr_size, mutex));
    else //automatically skip threadpool if only 1 thread allocated
      ComputeRows(thread_id,order, body, tmpInt+thread_id*tmpInt_size,
                           tmpReal+thread_id*tmpReal_size, tmpIntPtr+thread_id*tmpIntPtr_size,
                           tmpRealPtr+thread_id*tmpRealPtr_size, tmpMutablePtr+thread_id*tmpMutablePtr_size, mutex);
#else
    ComputeRows(thread_id,order, body, tmpInt+thread_id*tmpInt_size,
                           tmpReal+thread_id*tmpReal_size, tmpIntPtr+thread_id*tmpIntPtr_size,
                           tmpRealPtr+thread_id*tmpRealPtr_size, tmpMutablePtr+thread_id*tmpMutablePtr_size, mutex);
#endif
  }


  // check time for scheduling, this is usually very quick
  //gettimeofday(&tv,NULL);
  //double wait_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  //printf("      quickstep done scheduling start time %f stopped time %f duration %f\n",cur_time,wait_time,wait_time - cur_time);

#ifdef USE_TPROW
  IFTIMING (dTimerNow ("wait for threads"));
  if (row_threadpool && row_threadpool->size() > 0)
    row_threadpool->wait();
  IFTIMING (dTimerNow ("threads done"));
#endif



  gettimeofday(&tv,NULL);
  double end_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  #ifdef REPORT_THREAD_TIMING
  printf("    quickstep threads start time %f stopped time %f duration %f\n",cur_time,end_time,end_time - cur_time);
  #endif



  delete mutex;
}

struct dJointWithInfo1
{
  dxJoint *joint;
  dxJoint::Info1 info;
};

void dxQuickStepper (dxWorldProcessContext *context, 
  dxWorld *world, dxBody * const *body, int nb,
  dxJoint * const *_joint, int _nj, dReal stepsize)
{
  IFTIMING(dTimerStart("preprocessing"));

  const dReal stepsize1 = dRecip(stepsize);

  {
    // number all bodies in the body list - set their tag values
    for (int i=0; i<nb; i++) body[i]->tag = i;
  }

  // for all bodies, compute the inertia tensor and its inverse in the global
  // frame, and compute the rotational force and add it to the torque
  // accumulator. I and invI are a vertical stack of 3x4 matrices, one per body.
  dReal *invI = context->AllocateArray<dReal> (3*4*nb);
  dReal *I = context->AllocateArray<dReal> (3*4*nb);

  {
    dReal *invIrow = invI;
    dReal *Irow = I;
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; invIrow += 12, Irow += 12, bodycurr++) {
      dMatrix3 tmp;
      dxBody *b = *bodycurr;

      // compute inverse inertia tensor in global frame
      dMultiply2_333 (tmp,b->invI,b->posr.R);
      dMultiply0_333 (invIrow,b->posr.R,tmp);

      // also store I for later use by preconditioner
      dMultiply2_333 (tmp,b->mass.I,b->posr.R);
      dMultiply0_333 (Irow,b->posr.R,tmp);

      if (b->flags & dxBodyGyroscopic) {
        // compute rotational force
        dMultiply0_331 (tmp,Irow,b->avel);
        dSubtractVectorCross3(b->tacc,b->avel,tmp);
      }
    }
  }

  // get the masses for every body
  dReal *invM = context->AllocateArray<dReal> (nb);
  {
    dReal *invMrow = invM;
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; invMrow++, bodycurr++) {
      dxBody *b = *bodycurr;
      //*invMrow = b->mass.mass;
      *invMrow = b->invMass;

    }
  }


  {
    // add the gravity force to all bodies
    // since gravity does normally have only one component it's more efficient
    // to run three loops for each individual component
    dxBody *const *const bodyend = body + nb;
    dReal gravity_x = world->gravity[0];
    if (gravity_x) {
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
        dxBody *b = *bodycurr;
        if ((b->flags & dxBodyNoGravity)==0) {
          b->facc[0] += b->mass.mass * gravity_x;
        }
      }
    }
    dReal gravity_y = world->gravity[1];
    if (gravity_y) {
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
        dxBody *b = *bodycurr;
        if ((b->flags & dxBodyNoGravity)==0) {
          b->facc[1] += b->mass.mass * gravity_y;
        }
      }
    }
    dReal gravity_z = world->gravity[2];
    if (gravity_z) {
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
        dxBody *b = *bodycurr;
        if ((b->flags & dxBodyNoGravity)==0) {
          b->facc[2] += b->mass.mass * gravity_z;
        }
      }
    }
  }

  // get joint information (m = total constraint dimension, nub = number of unbounded variables).
  // joints with m=0 are inactive and are removed from the joints array
  // entirely, so that the code that follows does not consider them.
  dJointWithInfo1 *const jointiinfos = context->AllocateArray<dJointWithInfo1> (_nj);
  int nj;
  
  {
    dJointWithInfo1 *jicurr = jointiinfos;
    dxJoint *const *const _jend = _joint + _nj;
    for (dxJoint *const *_jcurr = _joint; _jcurr != _jend; _jcurr++) {	// jicurr=dest, _jcurr=src
      dxJoint *j = *_jcurr;
      j->getInfo1 (&jicurr->info);
      dIASSERT (jicurr->info.m >= 0 && jicurr->info.m <= 6 && jicurr->info.nub >= 0 && jicurr->info.nub <= jicurr->info.m);
      if (jicurr->info.m > 0) {
        jicurr->joint = j;
        jicurr++;
      }
    }
    nj = jicurr - jointiinfos;
  }

  context->ShrinkArray<dJointWithInfo1>(jointiinfos, _nj, nj);

  int m;
  int mfb; // number of rows of Jacobian we will have to save for joint feedback

  {
    int mcurr = 0, mfbcurr = 0;
    const dJointWithInfo1 *jicurr = jointiinfos;
    const dJointWithInfo1 *const jiend = jicurr + nj;
    for (; jicurr != jiend; jicurr++) {
      int jm = jicurr->info.m;
      mcurr += jm;
      if (jicurr->joint->feedback)
        mfbcurr += jm;
    }

    m = mcurr;
    mfb = mfbcurr;
  }

#ifdef USE_JOINT_DAMPING
  /************************************************************************/
  /* for joint damping, get the total number of rows for damping jacobian */
  /************************************************************************/
  int m_damp; // number of rows for damped joint jacobian
  {
    int mcurr = 0;
    const dJointWithInfo1 *jicurr = jointiinfos; // info1 stored in jointiinfos
    const dJointWithInfo1 *const jiend = jicurr + nj;
    for (; jicurr != jiend; jicurr++)
      if (jicurr->joint->use_damping)
        mcurr ++;

    m_damp = mcurr;
  }
#endif

  // if there are constraints, compute the constraint force
  dReal *J = NULL;
  int *jb = NULL;

#ifdef USE_JOINT_DAMPING
  /*********************************/
  /* do the same for damped joints */
  /*********************************/
  dReal *v_damp;
  dReal *J_damp = NULL;
  dReal *v_joint_damp = NULL;
  dReal* f_damp = NULL;
  dReal *JiM = NULL;
  int *jb_damp = NULL;
  dReal *coeff_damp = NULL;
#endif

  if (m > 0) {
    dReal *cfm, *lo, *hi, *rhs, *Jcopy;
    int *findex;

    {
      int mlocal = m;

      const unsigned jelements = mlocal*12;
      J = context->AllocateArray<dReal> (jelements);
      dSetZero (J,jelements);

      // create a constraint equation right hand side vector `c', a constraint
      // force mixing vector `cfm', and LCP low and high bound vectors, and an
      // 'findex' vector.
      cfm = context->AllocateArray<dReal> (mlocal);
      dSetValue (cfm,mlocal,world->global_cfm);

      lo = context->AllocateArray<dReal> (mlocal);
      dSetValue (lo,mlocal,-dInfinity);

      hi = context->AllocateArray<dReal> (mlocal);
      dSetValue (hi,mlocal, dInfinity);

      findex = context->AllocateArray<int> (mlocal);
      for (int i=0; i<mlocal; i++) findex[i] = -1;

      const unsigned jbelements = mlocal*2;
      jb = context->AllocateArray<int> (jbelements);

      rhs = context->AllocateArray<dReal> (mlocal);

      Jcopy = context->AllocateArray<dReal> (mfb*12);

#ifdef USE_JOINT_DAMPING
      JiM = context->AllocateArray<dReal> (mlocal*12); // for computing b_damp
      dSetZero (JiM,jelements);
#endif
    }

#ifdef USE_JOINT_DAMPING
    /*********************************/
    /* for damped joints             */
    /*********************************/
    {
      int mlocal = m_damp;

      const unsigned jelements = mlocal*12;
      J_damp = context->AllocateArray<dReal> (jelements);
      dSetZero (J_damp,jelements);

      // v_joint = J_damp * v
      // v_joint is the velocity of the joint in joint space
      // (relative angular rates of attached bodies)
      const unsigned v_joint_damp_elements = mlocal;
      v_joint_damp = context->AllocateArray<dReal> (v_joint_damp_elements);
      dSetZero (v_joint_damp,v_joint_damp_elements);

      // jb is the body index for each jacobian
      const unsigned jbelements = mlocal*2;
      jb_damp = context->AllocateArray<int> (jbelements);

      const unsigned f_damp_elements = nb*6;
      f_damp = context->AllocateArray<dReal> (f_damp_elements);
      dSetZero (f_damp,f_damp_elements);

      const unsigned v_damp_elements = nb*6;
      v_damp = context->AllocateArray<dReal> (v_damp_elements);
      dSetZero (v_damp,v_damp_elements);

      const unsigned coeffelements = mlocal;
      coeff_damp = context->AllocateArray<dReal> (coeffelements);
      dSetZero (coeff_damp,coeffelements);
    }
#endif

    BEGIN_STATE_SAVE(context, cstate) {
      dReal *c = context->AllocateArray<dReal> (m);
      dSetZero (c, m);

      {
        IFTIMING (dTimerNow ("create J"));
        // get jacobian data from constraints. an m*12 matrix will be created
        // to store the two jacobian blocks from each constraint. it has this
        // format:
        //
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 \    .
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2  )-- jacobian for joint 0, body 1 and body 2 (3 rows)
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 /
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 )--- jacobian for joint 1, body 1 and body 2 (3 rows)
        //   etc...
        //
        //   (lll) = linear jacobian data
        //   (aaa) = angular jacobian data
        //
        dxJoint::Info2 Jinfo;
        Jinfo.rowskip = 12;
        Jinfo.fps = stepsize1;
        Jinfo.erp = world->global_erp;

        dReal *Jcopyrow = Jcopy;
        unsigned ofsi = 0;
#ifdef USE_JOINT_DAMPING
        unsigned ofsi_damp = 0; // for joint damping
#endif
        const dJointWithInfo1 *jicurr = jointiinfos;
        const dJointWithInfo1 *const jiend = jicurr + nj;
        for (; jicurr != jiend; jicurr++) {
          dReal *const Jrow = J + ofsi * 12;
          Jinfo.J1l = Jrow;
          Jinfo.J1a = Jrow + 3;
          Jinfo.J2l = Jrow + 6;
          Jinfo.J2a = Jrow + 9;
          Jinfo.c = c + ofsi;
          Jinfo.cfm = cfm + ofsi;
          Jinfo.lo = lo + ofsi;
          Jinfo.hi = hi + ofsi;
          Jinfo.findex = findex + ofsi;



#ifdef USE_JOINT_DAMPING
          /*******************************************************/
          /*  allocate space for damped joint Jacobians          */
          /*******************************************************/
          if (jicurr->joint->use_damping)
          {
            // damping coefficient is in jicurr->info.damping_coefficient);
            coeff_damp[ofsi_damp] = jicurr->joint->damping_coefficient;

            // setup joint damping pointers so getinfo2 will fill in J_damp
            dReal *const Jrow_damp = J_damp + ofsi_damp * 12;
            Jinfo.J1ld = Jrow_damp;
            Jinfo.J1ad = Jrow_damp + 3;
            Jinfo.J2ld = Jrow_damp + 6;
            Jinfo.J2ad = Jrow_damp + 9;
            // one row of constraint per joint
            ofsi_damp ++;
          }
#endif


          
          // now write all information into J
          dxJoint *joint = jicurr->joint;
          joint->getInfo2 (&Jinfo);

          const int infom = jicurr->info.m;

          // we need a copy of Jacobian for joint feedbacks
          // because it gets destroyed by SOR solver
          // instead of saving all Jacobian, we can save just rows
          // for joints, that requested feedback (which is normally much less)
          if (joint->feedback) {
            const int rowels = infom * 12;
            memcpy(Jcopyrow, Jrow, rowels * sizeof(dReal));
            Jcopyrow += rowels;
          }

          // adjust returned findex values for global index numbering
          int *findex_ofsi = findex + ofsi;
          for (int j=0; j<infom; j++) {
            int fival = findex_ofsi[j];
            if (fival >= 0) 
              findex_ofsi[j] = fival + ofsi;
          }

          ofsi += infom;
        }
      }

      {
        // create an array of body numbers for each joint row
        int *jb_ptr = jb;
        const dJointWithInfo1 *jicurr = jointiinfos;
        const dJointWithInfo1 *const jiend = jicurr + nj;
        for (; jicurr != jiend; jicurr++) {
          dxJoint *joint = jicurr->joint;
          const int infom = jicurr->info.m;

          int b1 = (joint->node[0].body) ? (joint->node[0].body->tag) : -1;
          int b2 = (joint->node[1].body) ? (joint->node[1].body->tag) : -1;
          for (int j=0; j<infom; j++) {
            jb_ptr[0] = b1;
            jb_ptr[1] = b2;
            jb_ptr += 2;
          }
        }
        dIASSERT (jb_ptr == jb+2*m);
        //printf("jjjjjjjjj %d %d\n",jb[0],jb[1]);
      }

#ifdef USE_JOINT_DAMPING
      {
        /*************************************************************/
        /* create an array of body numbers for each damped joint row */
        /*************************************************************/
        int *jb_damp_ptr = jb_damp;
        const dJointWithInfo1 *jicurr = jointiinfos;
        const dJointWithInfo1 *const jiend = jicurr + nj;
        for (; jicurr != jiend; jicurr++) {
          if (jicurr->joint->use_damping)
          {
            dxJoint *joint = jicurr->joint;
            const int infom = 1; // one damping jacobian row per hinge joint

            int b1 = (joint->node[0].body) ? (joint->node[0].body->tag) : -1;
            int b2 = (joint->node[1].body) ? (joint->node[1].body->tag) : -1;
            for (int j=0; j<infom; j++) {
              jb_damp_ptr[0] = b1;
              jb_damp_ptr[1] = b2;
              jb_damp_ptr += 2;
            }
          }
        }
        dIASSERT (jb_damp_ptr == jb_damp+2*m_damp);
      }
#endif



      BEGIN_STATE_SAVE(context, tmp1state) {
        IFTIMING (dTimerNow ("compute rhs"));
        // compute the right hand side `rhs'
        dReal *tmp1 = context->AllocateArray<dReal> (nb*6);
        // put v/h + invM*fe into tmp1
        dReal *tmp1curr = tmp1;
        const dReal *invIrow = invI;
        dxBody *const *const bodyend = body + nb;
        for (dxBody *const *bodycurr = body; bodycurr != bodyend; tmp1curr+=6, invIrow+=12, bodycurr++) {
          dxBody *b = *bodycurr;
          dReal body_invMass = b->invMass;
          for (int j=0; j<3; j++) tmp1curr[j] = b->facc[j] * body_invMass + b->lvel[j] * stepsize1;
          dMultiply0_331 (tmp1curr + 3,invIrow,b->tacc);
          for (int k=0; k<3; k++) tmp1curr[3+k] += b->avel[k] * stepsize1;
        }

        // put J*tmp1 into rhs
        multiply_J (m,J,jb,tmp1,rhs);

#ifdef USE_JOINT_DAMPING
        /*************************************************************/
        /* compute J*inv(M) here JiM, it does not change             */
        /* where b_damp = -J*inv(M)*f_damp / Ad  (since b is rhs/Ad) */
        /* and b is to be modified by b_damp                         */
        /*************************************************************/
        {
          dRealPtr J_ptr = J;
          dRealMutablePtr JiM_ptr = JiM; // intermediate solution storage
          // no need for iM anymore, just copying J
          for (int i=0; i<m;J_ptr+=12,JiM_ptr+=12, i++)
            for (int j=0; j<12; j++) JiM_ptr[j] = J_ptr[j];
        }
#endif
      
      } END_STATE_SAVE(context, tmp1state);

      // complete rhs
      for (int i=0; i<m; i++) rhs[i] = c[i]*stepsize1 - rhs[i];













      // new rhs (or b) is preconditioned!
      // perform Gauss Seidel on J*invJrhs = rhs
      // invJrhs = GS(J,rhs,iterations)
      // iterate on
      //   
      //   
      // [m n] = size(A);
      // for iter = 1:num_iters,
      //   % sweep forwards
      //   for i = 1:m,
      //     [ma,mi] = max(A(i,:));
      //     delta = 0;
      //     for l = 1:n,
      //       delta = A(i,l)*x(l);
      //     end,
      //     delta = (y(i) - delta)/A(i,mi);
      //     x(mi) = x(mi) + delta;
      //   end,
      // end,
      // in the end,
      // new_rhs = J * M * invJ * rhs

      dReal *invJrhs = context->AllocateArray<dReal> (6*nb);
      dSetZero (invJrhs, 6*nb);

      for (int gsiter = 0 ; gsiter < 30; gsiter++)
      {
        //printf("===============================\n");
        //printf("   delta: ");
        int J_index = 0;
        for (int i = 0; i < m; i++)
        {
          // which x do we solve for?

          int b1 = jb[i*2];
          int b2 = jb[i*2+1];


          //printf("iter[%d] i[%d] j[",gsiter,i);

          dReal delta = 0;
          dRealPtr invJrhs_ptr = invJrhs + b1*6; // corresponding location on invJrhs

          dReal J_max = 0;
          int J_max_i = 0;

          // multiply J row * invJrhs
          for (int j=0; j<6; j++) {
            delta += J[J_index+j] * invJrhs_ptr[j];
            if (dFabs(J[J_index+j]) > dFabs(J_max)) {
              J_max = J[J_index+j];
              J_max_i = b1*6 + j;
            }
            //printf("%f, ",J[J_index+j]);
          }
          J_index += 6;

          if (b2 >= 0) {

            //printf(" | ");

            invJrhs_ptr = invJrhs + b2*6;
            for (int j=0; j<6; j++) {
              delta += J[J_index+j] * invJrhs_ptr[j];
              if (dFabs(J[J_index+j]) > dFabs(J_max)) {
                J_max = J[J_index+j];
                J_max_i = b2*6 + j;
              }
              //printf("%f, ",J[J_index+j]);
            }
          }
          J_index += 6;

          //printf("] b1[%d] b2[%d] J_max[%f] J_max_i[%d] rhs[%f]\n",b1,b2,J_max,J_max_i,rhs[i]);
          // update invJrhs where corresponding J element is largest for stability
          delta = (rhs[i] - delta) / J_max;
          //printf("%f, ",delta);
          invJrhs[J_max_i] = invJrhs[J_max_i] + delta;

        }


        //printf("   invJrhs: ");
        //for (int i=0; i<m;i++) printf("%f, ",invJrhs[i]);
        //printf("\n");
        //printf("\n");
      }



      // next, new rhs = J*M*invJrhs
      //   first, tmpz = J*M for one row of J
      //   then, rhs[some row] = tmpz * invJrhs
      {
        dRealPtr J_ptr = J;
        dReal tmpz[12];
        dSetZero (tmpz, 12);
        for (int i=0; i<m;J_ptr+=12,i++) {

          // compute rhs = J * M * tmpz
          int b1 = jb[i*2];
          int b2 = jb[i*2+1];
          dReal k1 = body[b1]->mass.mass;

          for (int j=0; j<3 ; j++) tmpz[j] = J_ptr[j]*k1;

          const dReal *I_ptr1 = I + 12*b1;
          for (int j=0;j<3;j++) {
            tmpz[3+j] = 0;
            for (int k=0;k<3;k++) tmpz[3+j] += J_ptr[3+k]*I_ptr1[k*4+j];
          }

          if (b2 >= 0){
            dReal k2 = body[b2]->mass.mass;
            for (int j=0; j<3 ; j++) tmpz[6+j] = k2*J_ptr[j+6];
            const dReal *I_ptr2 = I + 12*b2;
            for (int j=0;j<3;j++) {
              tmpz[9+j] = 0;
              for (int k=0;k<3;k++) tmpz[9+j] += J_ptr[9+k]*I_ptr2[k*4+j];
            }
          }
          // now calculate rhs = tmpz * invJrhs
          rhs[i] = 0;
          for (int j=0; j<6; j++) rhs[i] += tmpz[j] * invJrhs[b1*6+j];
          if (b2 >= 0)
            for (int j=0; j<6; j++) rhs[i] += tmpz[j+6] * invJrhs[b2*6+j];
        }
      }
      


















      // scale CFM
      for (int j=0; j<m; j++) cfm[j] *= stepsize1;

    } END_STATE_SAVE(context, cstate);


#ifdef USE_JOINT_DAMPING
    /***************************************************************************/
    /* create a nb*6 by 1 vector (v_damp) to store estimated implicit velocity */
    /*  as it is updated in the iterative loop                                 */
    /***************************************************************************/
    {
      // allocate v_damp
      dRealMutablePtr v_damp_ptr = v_damp;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; v_damp_ptr+=6, bodycurr++) {
        dxBody *b = *bodycurr;
        v_damp_ptr[0] = b->lvel[0];
        v_damp_ptr[1] = b->lvel[1];
        v_damp_ptr[2] = b->lvel[2];
        v_damp_ptr[3] = b->avel[0];
        v_damp_ptr[4] = b->avel[1];
        v_damp_ptr[5] = b->avel[2];
      }
    }
#endif


    // load lambda from the value saved on the previous iteration
    dReal *lambda = context->AllocateArray<dReal> (m);

#ifdef WARM_STARTING
    {
      dReal *lambdscurr = lambda;
      const dJointWithInfo1 *jicurr = jointiinfos;
      const dJointWithInfo1 *const jiend = jicurr + nj;
      for (; jicurr != jiend; jicurr++) {
        int infom = jicurr->info.m;
        memcpy (lambdscurr, jicurr->joint->lambda, infom * sizeof(dReal));
        lambdscurr += infom;
      }
    }
#endif



    dReal *cforce = context->AllocateArray<dReal> (nb*6);


    BEGIN_STATE_SAVE(context, lcpstate) {
      IFTIMING (dTimerNow ("solving LCP problem"));
      // solve the LCP problem and get lambda and invM*constraint_force
      SOR_LCP (context,m,nb,J,jb,body,invI,lambda,cforce,rhs,lo,hi,cfm,findex,&world->qs,
#ifdef USE_JOINT_DAMPING
               m_damp,J_damp,coeff_damp,jb_damp,v_damp,f_damp,v_joint_damp,JiM,
#endif
#ifdef USE_TPROW
               world->row_threadpool,
#endif
               stepsize);

    } END_STATE_SAVE(context, lcpstate);

#ifdef WARM_STARTING
    {
      // save lambda for the next iteration
      //@@@ note that this doesn't work for contact joints yet, as they are
      // recreated every iteration
      const dReal *lambdacurr = lambda;
      const dJointWithInfo1 *jicurr = jointiinfos;
      const dJointWithInfo1 *const jiend = jicurr + nj;
      for (; jicurr != jiend; jicurr++) {
        int infom = jicurr->info.m;
        memcpy (jicurr->joint->lambda, lambdacurr, infom * sizeof(dReal));
        lambdacurr += infom;
      }
    }
#endif

#ifdef USE_JOINT_DAMPING
    /****************************************************************/
    /* perform velocity update due to damping force                 */
    /*  v_new = n_old + stepsize * invM * f_damp                    */
    /****************************************************************/
    {
      const dReal *invIrow = invI;

      dRealMutablePtr f_damp_ptr = f_damp;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; invIrow += 12, f_damp_ptr+=6, bodycurr++) {
        // f_damp should be updated in SOR LCP

        // compute the velocity update:
        // add stepsize * invM * f_damp to the body velocity
        dxBody *b = *bodycurr;
        dReal body_invMass_mul_stepsize = stepsize * b->invMass;
        for (int j=0; j<3; j++) {
          b->lvel[j] += body_invMass_mul_stepsize * f_damp_ptr[j];
          f_damp_ptr[3+j] *= stepsize; // multiply torque part by step size
        }
        dMultiplyAdd0_331 (b->avel, invIrow, f_damp_ptr+3);
      }

    }
#endif


    // note that the SOR method overwrites rhs and J at this point, so
    // they should not be used again.
    {
      IFTIMING (dTimerNow ("velocity update due to constraint forces"));
      // note that cforce is really not a force but an acceleration, hence there is
      // no premultiplying of invM here (compare to update due to external force 'facc' below)
      //
      // add stepsize * cforce to the body velocity
      const dReal *cforcecurr = cforce;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; cforcecurr+=6, bodycurr++) {
        dxBody *b = *bodycurr;
        for (int j=0; j<3; j++) {
          b->lvel[j] += stepsize * cforcecurr[j];
          b->avel[j] += stepsize * cforcecurr[3+j];
        }
      }
    }

    if (mfb > 0) {
      // straightforward computation of joint constraint forces:
      // multiply related lambdas with respective J' block for joints
      // where feedback was requested
      dReal data[6];
      const dReal *lambdacurr = lambda;
      const dReal *Jcopyrow = Jcopy;
      const dJointWithInfo1 *jicurr = jointiinfos;
      const dJointWithInfo1 *const jiend = jicurr + nj;
      for (; jicurr != jiend; jicurr++) {
        dxJoint *joint = jicurr->joint;
        const int infom = jicurr->info.m;

        if (joint->feedback) {
          dJointFeedback *fb = joint->feedback;
          Multiply1_12q1 (data, Jcopyrow, lambdacurr, infom);
          fb->f1[0] = data[0];
          fb->f1[1] = data[1];
          fb->f1[2] = data[2];
          fb->t1[0] = data[3];
          fb->t1[1] = data[4];
          fb->t1[2] = data[5];

          if (joint->node[1].body)
          {
            Multiply1_12q1 (data, Jcopyrow+6, lambdacurr, infom);
            fb->f2[0] = data[0];
            fb->f2[1] = data[1];
            fb->f2[2] = data[2];
            fb->t2[0] = data[3];
            fb->t2[1] = data[4];
            fb->t2[2] = data[5];
          }
          
          Jcopyrow += infom * 12;
        }
      
        lambdacurr += infom;
      }
    }
  }

  {
    IFTIMING (dTimerNow ("compute velocity update"));
    // compute the velocity update:
    // add stepsize * invM * fe to the body velocity
    const dReal *invIrow = invI;
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; invIrow += 12, bodycurr++) {
      dxBody *b = *bodycurr;
      dReal body_invMass_mul_stepsize = stepsize * b->invMass;
      for (int j=0; j<3; j++) {
        b->lvel[j] += body_invMass_mul_stepsize * b->facc[j];
        b->tacc[j] *= stepsize;
      }
      dMultiplyAdd0_331 (b->avel, invIrow, b->tacc);
    }
  }

#ifdef CHECK_VELOCITY_OBEYS_CONSTRAINT
  if (m > 0) {
    BEGIN_STATE_SAVE(context, velstate) {
      dReal *vel = context->AllocateArray<dReal>(nb*6);

      // check that the updated velocity obeys the constraint (this check needs unmodified J)
      dReal *velcurr = vel;
      dxBody *bodycurr = body, *const bodyend = body + nb;
      for (; bodycurr != bodyend; velcurr += 6, bodycurr++) {
        for (int j=0; j<3; j++) {
          velcurr[j] = bodycurr->lvel[j];
          velcurr[3+j] = bodycurr->avel[j];
        }
      }
      dReal *tmp = context->AllocateArray<dReal> (m);
      multiply_J (m,J,jb,vel,tmp);
      dReal error = 0;
      for (int i=0; i<m; i++) error += dFabs(tmp[i]);
      printf ("velocity error = %10.6e\n",error);
    
    } END_STATE_SAVE(context, velstate)
  }
#endif

  {
    // update the position and orientation from the new linear/angular velocity
    // (over the given timestep)
    IFTIMING (dTimerNow ("update position"));
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
      dxBody *b = *bodycurr;
      dxStepBody (b,stepsize);
    }
  }

  {
    IFTIMING (dTimerNow ("tidy up"));
    // zero all force accumulators
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
      dxBody *b = *bodycurr;
      dSetZero (b->facc,3);
      dSetZero (b->tacc,3);
    }
  }

  IFTIMING (dTimerEnd());
  IFTIMING (if (m > 0) dTimerReport (stdout,1));

}

#ifdef USE_CG_LCP
static size_t EstimateGR_LCPMemoryRequirements(int m)
{
  size_t res = dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for iMJ
  res += 5 * dEFFICIENT_SIZE(sizeof(dReal) * m); // for r, z, p, q, Ad
  return res;
}
#endif

static size_t EstimateSOR_LCPMemoryRequirements(int m
#ifdef USE_JOINT_DAMPING
                                               ,int m_damp
#endif
                                               )
{
  size_t res = dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for iMJ
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for Ad
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for Adcfm
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for delta_error
  res += dEFFICIENT_SIZE(sizeof(IndexError) * m); // for order
#ifdef REORDER_CONSTRAINTS
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for last_lambda
#endif
#ifdef USE_JOINT_DAMPING
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for b_damp
#endif
  return res;
}

size_t dxEstimateQuickStepMemoryRequirements (
  dxBody * const *body, int nb, dxJoint * const *_joint, int _nj)
{
  int nj, m, mfb;

  {
    int njcurr = 0, mcurr = 0, mfbcurr = 0;
    dxJoint::SureMaxInfo info;
    dxJoint *const *const _jend = _joint + _nj;
    for (dxJoint *const *_jcurr = _joint; _jcurr != _jend; _jcurr++) {	
      dxJoint *j = *_jcurr;
      j->getSureMaxInfo (&info);
      
      int jm = info.max_m;
      if (jm > 0) {
        njcurr++;

        mcurr += jm;
        if (j->feedback)
          mfbcurr += jm;
      }
    }
    nj = njcurr; m = mcurr; mfb = mfbcurr;
  }

#ifdef USE_JOINT_DAMPING
  int m_damp;
  {
    int m_dampcurr = 0;
    dxJoint::SureMaxInfo info;
    dxJoint *const *const _jend = _joint + _nj;
    for (dxJoint *const *_jcurr = _joint; _jcurr != _jend; _jcurr++) {
      dxJoint *j = *_jcurr;
      /***************************/
      /* size for damping joints */
      /***************************/
      if (j->use_damping)
        m_dampcurr ++;
    }
    m_damp = m_dampcurr;
  }
#endif

  size_t res = 0;

  res += dEFFICIENT_SIZE(sizeof(dReal) * 3 * 4 * nb); // for invI
  res += dEFFICIENT_SIZE(sizeof(dReal) * 3 * 4 * nb); // for I needed by preconditioner
  res += dEFFICIENT_SIZE(sizeof(dReal) * nb); // for invM

  {
    size_t sub1_res1 = dEFFICIENT_SIZE(sizeof(dJointWithInfo1) * _nj); // for initial jointiinfos

    size_t sub1_res2 = dEFFICIENT_SIZE(sizeof(dJointWithInfo1) * nj); // for shrunk jointiinfos
    if (m > 0) {
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for J
      sub1_res2 += 4 * dEFFICIENT_SIZE(sizeof(dReal) * m); // for cfm, lo, hi, rhs
      sub1_res2 += dEFFICIENT_SIZE(sizeof(int) * 2 * m); // for jb            FIXME: shoulbe be 2 not 12?
      sub1_res2 += dEFFICIENT_SIZE(sizeof(int) * m); // for findex
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * mfb); // for Jcopy

#ifdef USE_JOINT_DAMPING
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m_damp); // for J_damp
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * m_damp ); // for v_joint_damp
      sub1_res2 += dEFFICIENT_SIZE(sizeof(int) * 2 * m_damp); // for jb_damp            FIXME: shoulbe be 2 not 12?
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for f_damp
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 12*m); // for JiM
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for v_damp
      sub1_res2 += dEFFICIENT_SIZE(sizeof(dReal) * m_damp); // for coeff_damp
#endif
      {
        size_t sub2_res1 = dEFFICIENT_SIZE(sizeof(dReal) * m); // for c
        sub2_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for invJrhs
        {
          size_t sub3_res1 = dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for tmp1
    
          size_t sub3_res2 = 0;

          sub2_res1 += (sub3_res1 >= sub3_res2) ? sub3_res1 : sub3_res2;
        }

        size_t sub2_res2 = dEFFICIENT_SIZE(sizeof(dReal) * m); // for lambda
        sub2_res2 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for cforce
        {
          size_t sub3_res1 = EstimateSOR_LCPMemoryRequirements(m
#ifdef USE_JOINT_DAMPING
                                                              ,m_damp
#endif
                                                              ); // for SOR_LCP

          size_t sub3_res2 = 0;
#ifdef CHECK_VELOCITY_OBEYS_CONSTRAINT
          {
            size_t sub4_res1 = dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for vel
            sub4_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m); // for tmp

            size_t sub4_res2 = 0;

            sub3_res2 += (sub4_res1 >= sub4_res2) ? sub4_res1 : sub4_res2;
          }
#endif
          sub2_res2 += (sub3_res1 >= sub3_res2) ? sub3_res1 : sub3_res2;
        }

        sub1_res2 += (sub2_res1 >= sub2_res2) ? sub2_res1 : sub2_res2;
      }
    }
    
    res += (sub1_res1 >= sub1_res2) ? sub1_res1 : sub1_res2;
  }

  return res;
}


