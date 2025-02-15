

/* 
A* -------------------------------------------------------------------
B* This file contains source code for the PyMOL computer program
C* Copyright (c) Schrodinger, LLC. 
D* -------------------------------------------------------------------
E* It is unlawful to modify or remove this copyright notice.
F* -------------------------------------------------------------------
G* Please see the accompanying LICENSE file for further information. 
H* -------------------------------------------------------------------
I* Additional authors of this source file include:
-* 
-* 
-*
Z* -------------------------------------------------------------------
*/
#ifndef _H_os_predef
#define _H_os_predef

/* Macros used by Fortify source in GCC 4.1.x are incompatible with
   PyMOL's Feedback system... */

#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif


/* BEGIN PROPRIETARY CODE SEGMENT (see disclaimer in "os_proprietary.h") */

#ifdef WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif


/* commercial product */

#ifdef PYMOL_COMM
#ifndef _PYMOL_IP_SPLASH
#define _PYMOL_IP_SPLASH
#endif
#ifndef _PYMOL_IP_EXTRAS
#define _PYMOL_IP_EXTRAS
#endif
#endif


/* collaboration product (placarded) */

#ifdef PYMOL_COLL
#ifndef _PYMOL_IP_SPLASH
#define _PYMOL_IP_SPLASH
#endif
#ifndef _PYMOL_IP_EXTRAS
#define _PYMOL_IP_EXTRAS
#endif
#endif


/* educational product (placarded) */

#ifdef PYMOL_EDU
#ifndef _PYMOL_IP_SPLASH
#define _PYMOL_IP_SPLASH
#endif
#ifndef _PYMOL_IP_EXTRAS
#define _PYMOL_IP_EXTRAS
#endif
#endif


/* evaluation product (placarded) */

#ifdef PYMOL_EVAL
#ifndef _PYMOL_IP_SPLASH
#define _PYMOL_IP_SPLASH
#endif
#endif


/* END PROPRIETARY CODE SEGMENT */

#ifdef __linux__
#include <malloc.h>
#else
#include <stddef.h>
#endif

#if defined(_MSC_VER) && (_MSC_VER < 1900)
#pragma warning (disable:4996)
#if !defined(snprintf) && (_MSC_VER < 1900)
#define snprintf sprintf_s
#endif
#endif

#include "ov_types.h"

// alternative to std::swap if references are not allowed (e.g. bit fields)
#define SWAP_NOREF(a, b) {auto _t=(a);(a)=(b);(b)=_t;}

#endif
