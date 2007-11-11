/* This file was autogenerated by Premake */
#ifndef _ODE_CONFIG_H_
#define _ODE_CONFIG_H_


/******************************************************************
 * CONFIGURATON SETTINGS - you can change these, and then rebuild
 *   ODE to modify the behavior of the library.
 *
 *   dSINGLE/dDOUBLE   - force ODE to use single-precision (float)
 *                       or double-precision (double) for numbers.
 *                       Only one should be defined.
 *
 *   dTRIMESH_ENABLED  - enable/disable trimesh support
 *   dTRIMESH_OPCODE   - use the OPCODE trimesh engine
 *   dTRIMESH_GIMPACT  - use the GIMPACT trimesh engine
 *                       Only one trimesh engine should be enabled.
 *
 *   dTRIMESH_16BIT_INDICES (todo: opcode only)
 *                       Setup the trimesh engine to use 16 bit
 *                       triangle indices. The default is to use
 *                       32 bit indices. Use the dTriIndex type to
 *                       detect the correct index size.
 *
 *   dUSE_MALLOC_FOR_ALLOCA (experimental)-
 *                       Use malloc() instead of alloca(). Slower,
 *                       but allows for larger systems and more
 *                       graceful out-of-memory handling.
 *
 *   dTRIMESH_OPCODE_USE_NEW_TRIMESH_TRIMESH_COLLIDER (experimental)-
 *                       Use an alternative trimesh-trimesh collider
 *                       which should yield better results.
 *
 ******************************************************************/

#define dSINGLE
/* #define dDOUBLE */

#define dTRIMESH_ENABLED 1
#define dTRIMESH_OPCODE 1
#define dTRIMESH_16BIT_INDICES 0

#define dTRIMESH_OPCODE_USE_NEW_TRIMESH_TRIMESH_COLLIDER 0

/* #define dUSE_MALLOC_FOR_ALLOCA */


/******************************************************************
 * SYSTEM SETTINGS - you shouldn't need to change these. If you
 *   run into an issue with these settings, please report it to
 *   the ODE bug tracker at:
 *      http://sf.net/tracker/?group_id=24884&atid=382799
 ******************************************************************/

/* Try to identify the platform */
#if defined(_XENON)
  #define ODE_PLATFORM_XBOX360
#elif defined(SN_TARGET_PSP_HW)
  #define ODE_PLATFORM_PSP
#elif defined(SN_TARGET_PS3)
  #define ODE_PLATFORM_PS3
#elif defined(_MSC_VER) || defined(__CYGWIN32__) || defined(__MINGW32__)
  #define ODE_PLATFORM_WINDOWS
#elif defined(__linux__)
  #define ODE_PLATFORM_LINUX
#elif defined(__APPLE__) && defined(__MACH__)
  #define ODE_PLATFORM_OSX
#else
  #error "Need some help identifying the platform!"
#endif

/* Additional platform defines used in the code */
#if defined(ODE_PLATFORM_WINDOWS) && !defined(WIN32)
  #define WIN32
#endif

#if defined(__CYGWIN32__) || defined(__MINGW32__)
  #define CYGWIN
#endif

#if defined(ODE_PLATFORM_OSX)
  #define macintosh
#endif


/* Define a DLL export symbol for those platforms that need it */
#if defined(ODE_PLATFORM_WINDOWS)
  #if defined(ODE_DLL)
    #define ODE_API __declspec(dllexport)
  #elif !defined(ODE_LIB)
    #define ODE_DLL_API __declspec(dllimport)
  #endif
#endif

#if !defined(ODE_API)
  #define ODE_API
#endif


/* Pull in the standard headers */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <float.h>

#if !defined(ODE_PLATFORM_PS3)
  #include <malloc.h>
#endif

#if !defined(ODE_PLATFORM_WINDOWS)
  #include <alloca.h>
#endif


/* Visual C does not define these functions */
#if defined(_MSC_VER)
  #define copysignf _copysign
  #define copysign _copysign
#endif


/* Define a value for infinity */
#if defined(HUGE_VALF)
	#define ODE_INFINITY4 HUGE_VALF
	#define ODE_INFINITY8 HUGE_VAL
#elif defined(FLT_MAX)
	#define ODE_INFINITY4 FLT_MAX
	#define ODE_INFINITY8 DBL_MAX
#else
	static union { unsigned char __c[4]; float  __f; }  __ode_huge_valf = {{0,0,0x80,0x7f}};
	static union { unsigned char __c[8]; double __d; }  __ode_huge_val  = {{0,0,0,0,0,0,0xf0,0x7f}};
	#define ODE_INFINITY4 (__ode_huge_valf.__f)
	#define ODE_INFINITY8 (__ode_huge_val.__d)
#endif

#ifdef dSINGLE
	#define dInfinity ODE_INFINITY4
	#define dEpsilon  FLT_EPSILON
#else
	#define dInfinity ODE_INFINITY8
	#define dEpsilon  DBL_EPSILON
#endif


/* Well-defined common data types...need to define for 64 bit systems */
#if defined(_M_IA64) || defined(__ia64__) || defined(_M_AMD64) || defined(__x86_64__)
  #define X86_64_SYSTEM   1
  typedef int             int32;
  typedef unsigned int    uint32;
  typedef short           int16;
  typedef unsigned short  uint16;
  typedef char            int8;
  typedef unsigned char   uint8;
#else
  typedef int             int32;
  typedef unsigned int    uint32;
  typedef short           int16;
  typedef unsigned short  uint16;
  typedef char            int8;
  typedef unsigned char   uint8;
#endif

/* An integer type that can be safely cast to a pointer. This definition
 * should be safe even on 64-bit systems */
typedef size_t intP;


/* The efficient alignment. most platforms align data structures to some
 * number of bytes, but this is not always the most efficient alignment.
 * for example, many x86 compilers align to 4 bytes, but on a pentium it is
 * important to align doubles to 8 byte boundaries (for speed), and the 4
 * floats in a SIMD register to 16 byte boundaries. many other platforms have
 * similar behavior. setting a larger alignment can waste a (very) small
 * amount of memory. NOTE: this number must be a power of two. */
#define EFFICIENT_ALIGNMENT 16


/* Define this if your system supports anonymous memory maps (linux does) */
#define MMAP_ANONYMOUS

#endif
