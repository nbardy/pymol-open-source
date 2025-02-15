
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
#ifndef _H_Movie
#define _H_Movie

#include <memory>
#include <array>
#include"os_python.h"
#include"Ortho.h"
#include"Scene.h"
#include"View.h"

using MovieCmdType = std::array<char, 1024>;

struct CMovieModal {
  int stage;

  /* input parameters */
  OrthoLineType prefix;
  int save, start, stop, missing_only;
  int modal, mode;

  int width;
  int height;

  /* job / local parameters */
  int frame;
  int image;
  int nFrame;
  double accumTiming;
  double timing;
  int complete;
  int file_missing;
  int format;
  int quiet;
  OrthoLineType fname;
};

struct CMovie : public Block {
  std::vector<std::shared_ptr<pymol::Image>> Image;
  pymol::vla<int> Sequence;
  pymol::vla<MovieCmdType> Cmd;
  int NImage { 0 }, NFrame { 0 };
  int MatrixFlag { false };
  SceneViewType Matrix {};
  int Playing { false };
  int Locked {};
  int CacheSave {};
  int OverlaySave {};
  pymol::vla<CViewElem> ViewElem;
  bool RecursionFlag { false };
  bool RealtimeFlag { true };
  CMovieModal Modal {};
  int Width {}, Height {};
  ScrollBar m_ScrollBar;
  int DragMode {};
  int Dragging {};
  CObject *DragObj {}; /* if not dragging all */
  BlockRect DragRect {};
  int DragX {}, DragY {}, DragMenu {};
  int DragStartFrame {}, DragCurFrame {}, DragNearest {}, DragDraw {};
  int DragColumn {};
  int LabelIndent {};
  int PanelActive {};

  CMovie(PyMOLGlobals* G);
  ~CMovie();

  int release(int button, int x, int y, int mod) override;
  int click(int button, int x, int y, int mod) override;
  int drag(int x, int y, int mod) override;
  void draw(CGO* orthoCGO) override;
  bool fastDraw(CGO* orthoCGO) override;
  void reshape(int width, int height) override;
};

int MovieFromPyList(PyMOLGlobals * G, PyObject * list, int *warning);
PyObject *MovieAsPyList(PyMOLGlobals * G);
int MovieGetSpecLevel(PyMOLGlobals *G,int frame);
void MovieDrawViewElem(PyMOLGlobals *G, BlockRect *rect,int frames ORTHOCGOARG);

Block *MovieGetBlock(PyMOLGlobals * G);
void MovieFree(PyMOLGlobals * G);
void MovieReset(PyMOLGlobals * G);
void MovieDump(PyMOLGlobals * G);
void MovieAppendSequence(PyMOLGlobals * G, const char *seq, int start_from,int freeze);
int MovieSeekScene(PyMOLGlobals * G, int loop);
int MoviePNG(PyMOLGlobals * G, char *prefix, int save, int start, int stop,
             int missing_only, int modal, int format, int mode, int quiet,
             int width=0, int height=0);
void MovieSetScrollBarFrame(PyMOLGlobals * G, int frame);
void MovieSetCommand(PyMOLGlobals * G, int frame, char *command);
void MovieAppendCommand(PyMOLGlobals * G, int frame, char *command);

void MovieDoFrameCommand(PyMOLGlobals * G, int frame);

void MovieCopyPrepare(PyMOLGlobals * G, int *width, int *height, int *length);
int MovieCopyFrame(PyMOLGlobals * G, int frame, int width, int height, int rowbytes,
                   void *ptr);
int MoviePurgeFrame(PyMOLGlobals * G, int frame);
void MovieCopyFinish(PyMOLGlobals * G);

#define cMovieStop 0
#define cMoviePlay 1
#define cMovieToggle -1

void MoviePlay(PyMOLGlobals * G, int cmd);
int MoviePlaying(PyMOLGlobals * G);
void MovieSetSize(PyMOLGlobals * G, unsigned int width, unsigned int height);

void MovieClearImages(PyMOLGlobals * G);
//Return copy of Image
std::shared_ptr<pymol::Image> MovieGetImage(PyMOLGlobals * G, int index);
void MovieSetImage(PyMOLGlobals * G, int index, std::shared_ptr<pymol::Image> image);

int MovieGetLength(PyMOLGlobals * G);
int MovieGetPanelHeight(PyMOLGlobals * G);
int MovieFrameToImage(PyMOLGlobals * G, int frame);
int MovieFrameToIndex(PyMOLGlobals * G, int frame);
int MovieLocked(PyMOLGlobals * G);
void MovieSetLock(PyMOLGlobals * G, int);
int MovieDefined(PyMOLGlobals * G);
int MovieView(PyMOLGlobals * G, int action, int first,
              int last, float power, float bias,
              int simple, float linear, int wrap,
              int hand, int window, int cycles,
              const char *scene_name, float scene_cut, int state, int quiet);
void MovieFlushCommands(PyMOLGlobals * G);
void MovieSetRealtime(PyMOLGlobals * G, int realtime);
int MovieGetRealtime(PyMOLGlobals * G);

#define cMovieMatrixClear  0
#define cMovieMatrixStore  1
#define cMovieMatrixRecall 2
#define cMovieMatrixCheck  3

int MovieMatrix(PyMOLGlobals * G, int action);  /* 0 clear, 1 remember, 2 recall */

int MovieViewModify(PyMOLGlobals *G,int action, int index, int count, int target, int freeze, int localize);
void MovieViewReinterpolate(PyMOLGlobals *G);
void MovieViewTrim(PyMOLGlobals *G,int n_frame);

void MoviePrepareDrag(PyMOLGlobals *G, BlockRect * rect, CObject * obj, int mode, int x, int y, int nearest);
int MovieXtoFrame(PyMOLGlobals *G, BlockRect *rect, int frames, int x, int nearest);

/*void MovieSave(char *fname);
  void MovieLoad(char *fname);*/

#endif
