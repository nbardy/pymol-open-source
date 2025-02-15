
/* 
A* -------------------------------------------------------------------
B* This file contains source code for the PyMOL computer program
C* copyright 1998-2000 by Warren Lyford Delano of DeLano Scientific. 
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
#ifndef _H_ObjectSurface
#define _H_ObjectSurface

#include"os_gl.h"
#include"ObjectMap.h"

typedef struct {
  CObjectState State;
  ObjectNameType MapName;
  int MapState;
  CCrystal Crystal;
  int Active;
  int *N, nT, base_n_V;
  float *V;
  float *VC;
  int *RC;
  int OneColor;
  int VCsize;
  int Range[6];
  float ExtentMin[3], ExtentMax[3];
  int ExtentFlag;
  float Level, Radius;
  int RefreshFlag;
  int ResurfaceFlag;
  int RecolorFlag;
  int quiet;
  float *AtomVertex;
  int CarveFlag;
  float CarveBuffer;
  int Mode;                     /* 0 dots, 1 lines, 2 triangles */
  int DotFlag;
  CGO *UnitCellCGO;
  int Side;
  CGO *shaderCGO;

  /* for immediate mode, holds vertices and colors, used temporarily to generate CGOs */
  float **t_buf; // vertices
  float **c_buf; // colors
} ObjectSurfaceState;

struct ObjectSurface : public CObject {
  ObjectSurfaceState *State;
  int NState = 0;
  ObjectSurface(PyMOLGlobals* G);
  ~ObjectSurface();

  // virtual methods
  void update() override;
  void render(RenderInfo* info) override;
  void invalidate(int rep, int level, int state) override;
  int getNFrame() const override;
};

ObjectSurface *ObjectSurfaceFromBox(PyMOLGlobals * G, ObjectSurface * obj,
                                    ObjectMap * map, int map_state, int state, float *mn,
                                    float *mx, float level, int mode, float carve,
                                    float *vert_vla, int side, int quiet);
void ObjectSurfaceDump(ObjectSurface * I, const char *fname, int state);

int ObjectSurfaceNewFromPyList(PyMOLGlobals * G, PyObject * list,
                               ObjectSurface ** result);
PyObject *ObjectSurfaceAsPyList(ObjectSurface * I);
int ObjectSurfaceSetLevel(ObjectSurface * I, float level, int state, int quiet);
int ObjectSurfaceGetLevel(ObjectSurface * I, int state, float *result);
int ObjectSurfaceInvalidateMapName(ObjectSurface * I, const char *name, const char * new_name);

#endif
