/* 
A* -------------------------------------------------------------------
B* This file contains source code for the PyMOL computer program
C* copyright 1998-2004 by Warren Lyford Delano of DeLano Scientific. 
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

#include <algorithm>
#include <string>
#include <vector>

#include"os_python.h"
#include"os_numpy.h"
#include"os_std.h"

#include"Base.h"
#include"Map.h"
#include"Vector.h"
#include"Err.h"
#include"Word.h"
#include"Util.h"
#include"PConv.h"
#include"P.h"

#include"MemoryDebug.h"
#include"Selector.h"
#include"Executive.h"
#include"ObjectMolecule.h"
#include"CoordSet.h"
#include"DistSet.h"
#include"Word.h"
#include"Scene.h"
#include"CGO.h"
#include"Seq.h"
#include"Editor.h"
#include"Seeker.h"
#include "Lex.h"
#include "Mol2Typing.h"

#include"OVContext.h"
#include"OVLexicon.h"
#include"OVOneToAny.h"
#include"Parse.h"

#include"ListMacros.h"

#include "SelectorDef.h"

#define cSelectorTmpPrefix "_sel_tmp_"
#define cSelectorTmpPrefixLen 9 /* strlen(cSelectorTmpPrefix) */
#define cSelectorTmpPattern "_sel_tmp_*"

#define cDummyOrigin 0
#define cDummyCenter 1


/* special selections, unknown to executive */
#define cColorectionFormat "_!c_%s_%d"

static WordKeyValue rep_names[] = {
  {"spheres", cRepSphereBit},
  {"sticks", cRepCylBit},
  {"surface", cRepSurfaceBit},
  {"labels", cRepLabelBit},
  {"nb_spheres", cRepNonbondedSphereBit},
  {"cartoon", cRepCartoonBit},
  {"ribbon", cRepRibbonBit},
  {"lines", cRepLineBit},
  {"dots", cRepDotBit},
  {"mesh", cRepMeshBit},
  {"nonbonded", cRepNonbondedBit},
  {"ellipsoid", cRepEllipsoidBit},
  {"",},
};

static const char *backbone_names[] = {
  // protein
  "CA", "C", "O", "N", "OXT", "H",
  // nucleic acid
  "P", "OP1", "OP2", "OP3", "C1'", "C2'", "O2'",
  "C3'", "O3'", "C4'", "O4'", "C5'", "O5'",
  "H1'", "H3'", "H4'",
  "H2'", "H2''", "H12'", "H22'",
  "H5'", "H5''", "H15'", "H25'",
  "HO2'", "HO3'", "HO5'",
  ""
};

/// Helper type which replaces `int*` return types
typedef std::unique_ptr<int[]> sele_array_t;
inline void sele_array_calloc(sele_array_t& sele, size_t count)
{
  sele.reset(new int[count]());
}

struct EvalElem {
  int level, imp_op_level;
  int type;                     /* 0 = value 1 = operation 2 = pre-operation */
  unsigned int code;
  std::string m_text;
  sele_array_t sele;

  // Helpers for refactoring `sele` type
  int* sele_data() { return sele.get(); }
  void sele_free() { sele.reset(); }
  void sele_calloc(size_t count) { sele_array_calloc(sele, count); }

  // TODO replace with pymol::Error handling
  void sele_check_ok(int& ok) { CHECKOK(ok, sele_data()); }
  void sele_err_chk_ptr(PyMOLGlobals* G) { ErrChkPtr(G, sele_data()); }

  /// read-only access to text
  const char* text() const { return m_text.c_str(); }
};

typedef struct {
  int depth1;
  int depth2;
  int depth3;
  int depth4;
  int sum;
  int frag;
} WalkDepthRec;

static sele_array_t SelectorSelect(PyMOLGlobals * G, const char *sele, int state, int domain, int quiet);
static int SelectorGetInterstateVLA(PyMOLGlobals * G, int sele1, int state1, int sele2,
                                    int state2, float cutoff, int **vla);

static int SelectorModulate1(PyMOLGlobals * G, EvalElem * base, int state);
static int SelectorSelect0(PyMOLGlobals * G, EvalElem * base);
static int SelectorSelect1(PyMOLGlobals * G, EvalElem * base, int quiet);
static int SelectorSelect2(PyMOLGlobals * G, EvalElem * base, int state);
static int SelectorLogic1(PyMOLGlobals * G, EvalElem * base, int state);
static int SelectorLogic2(PyMOLGlobals * G, EvalElem * base);
static int SelectorOperator22(PyMOLGlobals * G, EvalElem * base, int state);
static sele_array_t SelectorEvaluate(PyMOLGlobals* G, std::vector<std::string>& word, int state, int quiet);
static std::vector<std::string> SelectorParse(PyMOLGlobals * G, const char *s);
static void SelectorPurgeMembers(PyMOLGlobals * G, int sele);
static int SelectorEmbedSelection(PyMOLGlobals * G, const int *atom, const char *name,
                                  ObjectMolecule * obj, int no_dummies, int exec_manage);
static int *SelectorGetIndexVLA(PyMOLGlobals * G, int sele);
static int *SelectorGetIndexVLAImpl(PyMOLGlobals * G, CSelector *I, int sele);
static void SelectorClean(PyMOLGlobals * G);
static void SelectorCleanImpl(PyMOLGlobals * G, CSelector *I);
static int SelectorCheckNeighbors(PyMOLGlobals * G, int maxDepth, ObjectMolecule * obj,
                                  int at1, int at2, int *zero, int *scratch);

static sele_array_t SelectorUpdateTableSingleObject(PyMOLGlobals * G, ObjectMolecule * obj,
                                            int req_state,
                                            int no_dummies, int *idx,
                                            int n_idx, int numbered_tags);


/*========================================================================*/
/*
 * Iterator over the selector table (all atoms in universe)
 *
 * Selector table must be up-to-date!
 * Does NOT provide coord or coordset access
 *
 * (Implementation in Selector.cpp)
 */
class SelectorAtomIterator : public AbstractAtomIterator {
  CSelector * selector;

public:
  int a;        // index in selector

  SelectorAtomIterator(CSelector * I) {
    selector = I;

    // no coord set
    cs = NULL;
    idx = -1;

    reset();
  }

  void reset() override {
    a = cNDummyAtoms - 1;
  }

  bool next() override;
};

bool SelectorAtomIterator::next() {
  if ((++a) >= selector->NAtom)
    return false;

  TableRec *table_a = selector->Table + a;

  atm = table_a->atom;
  obj = selector->Obj[table_a->model];

  return true;
}


/*========================================================================*/
static int SelectorGetObjAtmOffset(CSelector * I, ObjectMolecule * obj, int offset)
{
  if(I->SeleBaseOffsetsValid) {
    return obj->SeleBase + offset;
  } else {
    ov_diff stop_below = obj->SeleBase;
    ov_diff stop_above = I->NAtom - 1;
    int result = stop_below;
    TableRec *i_table = I->Table;
    ObjectMolecule **i_obj = I->Obj;
    int step = offset;
    int cur;
    int proposed;
    int prior1 = -1, prior2 = -1;

    /* non-linear hunt to find atom */

    result = stop_below;
    cur = i_table[result].atom;
    while(step > 1) {
      if(cur < offset) {
        stop_below = result + 1;
        while(step > 1) {
          proposed = result + step;
          if(proposed <= stop_above) {
            if(i_obj[i_table[proposed].model] == obj) {
              if(proposed == prior1) {
                proposed--;
                step--;         /* guarantee progress (avoid flip flop) */
              }
              result = prior1 = proposed;
              break;
            } else if(stop_above > proposed) {
              stop_above = proposed - 1;
            }
          }
          step = (step >> 1);
        }
      } else if(cur > offset) {
        stop_above = result - 1;
        while(step > 1) {
          proposed = result - step;
          if(proposed >= stop_below) {
            if(i_obj[i_table[proposed].model] == obj) {
              if(proposed == prior2) {
                proposed++;
                step--;         /* guarantee progress (avoid flip flop) */
              }
              result = prior2 = proposed;
              break;
            }
          }
          step = (step >> 1);
        }
      } else
        return result;
      cur = i_table[result].atom;
      if(cur == offset)
        return result;
    }

    {
      /* failsafe / linear search */
      int dir = 1;
      if(cur > offset)
        dir = -1;
      while(1) {                /* TODO: optimize this search algorithm! */
        if(cur == offset)
          return result;
        if(dir > 0) {
          if(result >= stop_above)
            break;
          result++;
        } else {
          if(result <= stop_below)
            break;
          result--;
        }
        if(i_obj[i_table[result].model] != obj)
          break;
        cur = i_table[result].atom;
      }
    }
  }
  return -1;
}

#define STYP_VALU 0
#define STYP_OPR1 1
#define STYP_OPR2 2
#define STYP_SEL0 3
#define STYP_SEL1 4
#define STYP_SEL2 5
#define STYP_LIST 6
#define STYP_PRP1 7
#define STYP_SEL3 8
#define STYP_PVAL 0
#define STYP_OP22 9             /* sele oper arg1 arg2 sele */


/*                  code   |   type    | priority */

#define SELE_NOT1 ( 0x0100 | STYP_OPR1 | 0x70 )
#define SELE_BYR1 ( 0x0200 | STYP_OPR1 | 0x20 )
#define SELE_AND2 ( 0x0300 | STYP_OPR2 | 0x60 )
#define SELE_OR_2 ( 0x0400 | STYP_OPR2 | 0x40 )
#define SELE_IN_2 ( 0x0500 | STYP_OPR2 | 0x40 )
#define SELE_ALLz ( 0x0600 | STYP_SEL0 | 0x90 )
#define SELE_NONz ( 0x0700 | STYP_SEL0 | 0x90 )
#define SELE_HETz ( 0x0800 | STYP_SEL0 | 0x80 )
#define SELE_HYDz ( 0x0900 | STYP_SEL0 | 0x90 )
#define SELE_VISz ( 0x0A00 | STYP_SEL0 | 0x90 )
#define SELE_ARD_ ( 0x0B00 | STYP_PRP1 | 0x30 )
#define SELE_EXP_ ( 0x0C00 | STYP_PRP1 | 0x30 )
#define SELE_NAMs ( 0x0D00 | STYP_SEL1 | 0x80 )
#define SELE_ELEs ( 0x0E00 | STYP_SEL1 | 0x80 )
#define SELE_RSIs ( 0x0F00 | STYP_SEL1 | 0x80 )
#define SELE_CHNs ( 0x1000 | STYP_SEL1 | 0x80 )
#define SELE_SEGs ( 0x1100 | STYP_SEL1 | 0x80 )
#define SELE_MODs ( 0x1200 | STYP_SEL1 | 0x80 )
#define SELE_IDXs ( 0x1300 | STYP_SEL1 | 0x80 )
#define SELE_RSNs ( 0x1400 | STYP_SEL1 | 0x80 )
#define SELE_SELs ( 0x1500 | STYP_SEL1 | 0x80 )
#define SELE_BVLx ( 0x1600 | STYP_SEL2 | 0x80 )
#define SELE_ALTs ( 0x1700 | STYP_SEL1 | 0x80 )
#define SELE_FLGs ( 0x1800 | STYP_SEL1 | 0x80 )
#define SELE_GAP_ ( 0x1900 | STYP_PRP1 | 0x80 )
#define SELE_TTYs ( 0x1A00 | STYP_SEL1 | 0x80 )
#define SELE_NTYs ( 0x1B00 | STYP_SEL1 | 0x80 )
#define SELE_PCHx ( 0x1C00 | STYP_SEL2 | 0x80 )
#define SELE_FCHx ( 0x1D00 | STYP_SEL2 | 0x80 )
#define SELE_ID_s ( 0x1E00 | STYP_SEL1 | 0x80 )
#define SELE_BNDz ( 0x1F00 | STYP_SEL0 | 0x80 )
#define SELE_LIK2 ( 0x2000 | STYP_OPR2 | 0x40 )
#define SELE_NGH1 ( 0x2100 | STYP_OPR1 | 0x20 )
#define SELE_QVLx ( 0x2200 | STYP_SEL2 | 0x80 )
#define SELE_BYO1 ( 0x2300 | STYP_OPR1 | 0x20 )
#define SELE_SSTs ( 0x2400 | STYP_SEL1 | 0x80 )
#define SELE_STAs ( 0x2500 | STYP_SEL1 | 0x80 )
#define SELE_PREz ( 0x2500 | STYP_SEL0 | 0x80 )
#define SELE_WIT_ ( 0x2600 | STYP_OP22 | 0x30 )
#define SELE_ORIz ( 0x2700 | STYP_SEL0 | 0x90 )
#define SELE_CENz ( 0x2800 | STYP_SEL0 | 0x90 )
#define SELE_ENAz ( 0x2900 | STYP_SEL0 | 0x90 )
#define SELE_REPs ( 0x2A00 | STYP_SEL1 | 0x80 )
#define SELE_COLs ( 0x2B00 | STYP_SEL1 | 0x80 )
#define SELE_HBDs ( 0x2C00 | STYP_SEL0 | 0x80 )
#define SELE_HBAs ( 0x2D00 | STYP_SEL0 | 0x80 )
#define SELE_BYC1 ( 0x2E00 | STYP_OPR1 | 0x20 )
#define SELE_BYS1 ( 0x2F00 | STYP_OPR1 | 0x20 )
#define SELE_BYM1 ( 0x3000 | STYP_OPR1 | 0x20 )
#define SELE_BYF1 ( 0x3100 | STYP_OPR1 | 0x20 )
#define SELE_EXT_ ( 0x3200 | STYP_PRP1 | 0x30 )
#define SELE_BON1 ( 0x3300 | STYP_OPR1 | 0x50 )
#define SELE_FST1 ( 0x3400 | STYP_OPR1 | 0x30 )
#define SELE_CAS1 ( 0x3500 | STYP_OPR1 | 0x30 )
#define SELE_BEY_ ( 0x3600 | STYP_OP22 | 0x30 )
#define SELE_POLz ( 0x3700 | STYP_SEL0 | 0x90 )
#define SELE_SOLz ( 0x3800 | STYP_SEL0 | 0x90 )
#define SELE_ORGz ( 0x3900 | STYP_SEL0 | 0x90 )
#define SELE_INOz ( 0x3A00 | STYP_SEL0 | 0x90 )
#define SELE_GIDz ( 0x3B00 | STYP_SEL0 | 0x90 )
#define SELE_RNKs ( 0x3C00 | STYP_SEL1 | 0x80 )
#define SELE_PEPs ( 0x3D00 | STYP_SEL1 | 0x80 )
#define SELE_ACCz ( 0x3E00 | STYP_SEL0 | 0x90 )
#define SELE_DONz ( 0x3F00 | STYP_SEL0 | 0x90 )
#define SELE_LST1 ( 0x4000 | STYP_OPR1 | 0x30 )
#define SELE_NTO_ ( 0x4100 | STYP_OP22 | 0x30 )
#define SELE_CCLs ( 0x4200 | STYP_SEL1 | 0x80 )
#define SELE_RCLs ( 0x4300 | STYP_SEL1 | 0x80 )
#define SELE_PTDz ( 0x4400 | STYP_SEL0 | 0x90 )
#define SELE_MSKz ( 0x4500 | STYP_SEL0 | 0x90 )
#define SELE_IOR2 ( 0x4600 | STYP_OPR2 | 0x10 )
#define SELE_FXDz ( 0x4700 | STYP_SEL0 | 0x90 )
#define SELE_RSTz ( 0x4800 | STYP_SEL0 | 0x90 )
#define SELE_ANT2 ( 0x4900 | STYP_OPR2 | 0x60 )
#define SELE_BYX1 ( 0x4A00 | STYP_OPR1 | 0x20 )
#define SELE_STRO ( 0x4B00 | STYP_SEL1 | 0x80 )
#define SELE_METz ( 0x4C00 | STYP_SEL0 | 0x90 )
#define SELE_BB_z ( 0x4D00 | STYP_SEL0 | 0x90 )
#define SELE_SC_z ( 0x4E00 | STYP_SEL0 | 0x90 )
#define SELE_PROP ( 0x4F00 | STYP_SEL3 | 0x80 )
#define SELE_XVLx ( 0x5000 | STYP_SEL2 | 0x80 )
#define SELE_YVLx ( 0x5100 | STYP_SEL2 | 0x80 )
#define SELE_ZVLx ( 0x5200 | STYP_SEL2 | 0x80 )
#define SELE_CUST ( 0x5300 | STYP_SEL1 | 0x80 )
#define SELE_RING ( 0x5400 | STYP_OPR1 | 0x20 )
#define SELE_LABs ( 0x5500 | STYP_SEL1 | 0x80 )
#define SELE_PROz ( 0x5600 | STYP_SEL0 | 0x90 )
#define SELE_NUCz ( 0x5700 | STYP_SEL0 | 0x90 )

#define SEL_PREMAX 0x8

static WordKeyValue Keyword[] = {
  {"not", SELE_NOT1},
  {"!", SELE_NOT1},

  {"neighbor", SELE_NGH1},
  {"nbr;", SELE_NGH1},          /* deprecated */
  {"nbr.", SELE_NGH1},

  {"byfragment", SELE_BYF1},
  {"byfrag", SELE_BYF1},
  {"bf.", SELE_BYF1},

  {"byresidue", SELE_BYR1},
  {"byresi", SELE_BYR1},        /* unofficial */
  {"byres", SELE_BYR1},
  {"br;", SELE_BYR1},           /* deprecated */
  {"br.", SELE_BYR1},
  {"b;", SELE_BYR1},            /* deprecated */

  {"bychain", SELE_BYC1},
  {"bc.", SELE_BYC1},

  {"byobject", SELE_BYO1},
  {"byobj", SELE_BYO1},
  {"bo;", SELE_BYO1},           /* deprecated */
  {"bo.", SELE_BYO1},

  {"bound_to", SELE_BON1},
  {"bto.", SELE_BON1},

  {"bymolecule", SELE_BYM1},
  {"bymol", SELE_BYM1},
  {"bm.", SELE_BYM1},

  {"bysegment", SELE_BYS1},
  {"byseg", SELE_BYS1},
  {"bysegi", SELE_BYS1},        /* unofficial */
  {"bs.", SELE_BYS1},

  {"bycalpha", SELE_CAS1},
  {"bca.", SELE_CAS1},

  {"first", SELE_FST1},
  {"last", SELE_LST1},

  {"and", SELE_AND2},
  {"&", SELE_AND2},
  {"or", SELE_OR_2},
  {"+", SELE_OR_2},             /* added to mitigate damage caused by the obj1+obj2 parser bug */
  {"-", SELE_ANT2},             /* added to provide natural complement to the above: an AND NOT or SUBTRACT operation */
  {"|", SELE_OR_2},
  {"in", SELE_IN_2},

  {"like", SELE_LIK2},
  {"l;", SELE_LIK2},
  {"l.", SELE_LIK2},

  {cKeywordAll, SELE_ALLz},     /* 0 parameter */
  {"*", SELE_ALLz},             /* 0 parameter */

  {cKeywordNone, SELE_NONz},    /* 0 parameter */
  {"hetatm", SELE_HETz},        /* 0 parameter */
  {"het", SELE_HETz},           /* 0 parameter */

  {"hydrogens", SELE_HYDz},     /* 0 parameter */
  {"hydro", SELE_HYDz},         /* 0 parameter */
  {"h;", SELE_HYDz},            /* deprecated */
  {"h.", SELE_HYDz},            /* 0 parameter */

  {"hba.", SELE_HBAs},
  {"hbd.", SELE_HBDs},

  {"visible", SELE_VISz},       /* 0 parameter */
  {"v;", SELE_VISz},            /* 0 parameter */
  {"v.", SELE_VISz},            /* 0 parameter */

  {"around", SELE_ARD_},        /* 1 parameter */
  {"a;", SELE_ARD_},            /* deprecated */
  {"a.", SELE_ARD_},            /* 1 parameter */

  {"expand", SELE_EXP_},        /* 1 parameter */
  {"x;", SELE_EXP_},            /* 1 parameter */
  {"x.", SELE_EXP_},            /* 1 parameter */

  {"extend", SELE_EXT_},        /* 1 parameter */
  {"xt.", SELE_EXT_},           /* 1 parameter */

  {"name", SELE_NAMs},
  {"n;", SELE_NAMs},            /* deprecated */
  {"n.", SELE_NAMs},

  {"symbol", SELE_ELEs},
  {"element", SELE_ELEs},
  {"elem", SELE_ELEs},
  {"e;", SELE_ELEs},            /* deprecated */
  {"e.", SELE_ELEs},

  {"enabled", SELE_ENAz},

  {"residue", SELE_RSIs},
  {"resi", SELE_RSIs},
  {"resident", SELE_RSIs},
  {"resid", SELE_RSIs},
  {"i;", SELE_RSIs},            /* deprecated */
  {"i.", SELE_RSIs},

  {"rep", SELE_REPs},

  {"color", SELE_COLs},
  {"cartoon_color", SELE_CCLs},
  {"ribbon_color", SELE_RCLs},

  {"altloc", SELE_ALTs},
  {"alt", SELE_ALTs},

  {"flag", SELE_FLGs},
  {"f;", SELE_FLGs},            /* deprecated */
  {"f.", SELE_FLGs},

  {"gap", SELE_GAP_},

  {"partial_charge", SELE_PCHx},
  {"pc;", SELE_PCHx},           /* deprecated */
  {"pc.", SELE_PCHx},

  {"masked", SELE_MSKz},
  {"msk.", SELE_MSKz},

  {"protected", SELE_PTDz},

  {"formal_charge", SELE_FCHx},
  {"fc;", SELE_FCHx},           /* deprecated */
  {"fc.", SELE_FCHx},

  {"numeric_type", SELE_NTYs},
  {"nt;", SELE_NTYs},           /* deprecated */
  {"nt.", SELE_NTYs},

  {"text_type", SELE_TTYs},
  {"custom", SELE_CUST},
  {"tt;", SELE_TTYs},           /* deprecated */
  {"tt.", SELE_TTYs},

  {"chain", SELE_CHNs},
  {"c;", SELE_CHNs},            /* deprecated */
  {"c.", SELE_CHNs},

  {cKeywordCenter, SELE_CENz},
  {"bonded", SELE_BNDz},

  {"segment", SELE_SEGs},
  {"segid", SELE_SEGs},
  {"segi", SELE_SEGs},
  {"s;", SELE_SEGs},            /* deprecated */
  {"s.", SELE_SEGs},

  {"ss", SELE_SSTs},

  {"state", SELE_STAs},

  {"object", SELE_MODs},
  {"o.", SELE_MODs},

  {cKeywordOrigin, SELE_ORIz},

  {"model", SELE_MODs},
  {"m;", SELE_MODs},            /* deprecated */
  {"m.", SELE_MODs},

  {"index", SELE_IDXs},
  {"idx.", SELE_IDXs},

  {"id", SELE_ID_s},
  {"ID", SELE_ID_s},
  {"rank", SELE_RNKs},

  {"within", SELE_WIT_},
  {"w.", SELE_WIT_},

  {"near_to", SELE_NTO_},
  {"nto.", SELE_NTO_},

  {"beyond", SELE_BEY_},
  {"be.", SELE_BEY_},

  {"donors", SELE_DONz},
  {"don.", SELE_DONz},

  {"acceptors", SELE_ACCz},
  {"acc.", SELE_ACCz},

  {"pepseq", SELE_PEPs},
  {"ps.", SELE_PEPs},

  /*
     {  "nucseq",  SELE_NUCs },
     {  "ns.",      SELE_NUCs },
    */

  {"fixed", SELE_FXDz},
  {"fxd.", SELE_FXDz},

  {"restrained", SELE_RSTz},
  {"rst.", SELE_RSTz},

  {"polymer", SELE_POLz},
  {"pol.", SELE_POLz},

  {"polymer.protein", SELE_PROz},
  {"polymer.nucleic", SELE_NUCz},

#if 0
  // User survey winners. Not activated (yet) but ObjectMakeValidName
  // prints a deprecation warning if these names are used to name
  // objects or selections.
  {"protein", SELE_PROz},
  {"nucleic", SELE_NUCz},

  {"pro.", SELE_PROz},
  {"nuc.", SELE_NUCz},
#endif

  {"organic", SELE_ORGz},
  {"org.", SELE_ORGz},

  {"inorganic", SELE_INOz},
  {"ino.", SELE_INOz},

  {"solvent", SELE_SOLz},
  {"sol.", SELE_SOLz},

  {"guide", SELE_GIDz},

  {"present", SELE_PREz},
  {"pr.", SELE_PREz},

  {"resname", SELE_RSNs},
  {"resn", SELE_RSNs},
  {"r;", SELE_RSNs},            /* deprecated */
  {"r.", SELE_RSNs},

  {"%", SELE_SELs},
  {"b", SELE_BVLx},             /* 2 operand selection operator */
  {"q", SELE_QVLx},             /* 2 operand selection operator */

  {"stereo", SELE_STRO},

  {"bycell", SELE_BYX1},

  {"metals", SELE_METz},        /* 0 parameter */

  {"backbone", SELE_BB_z},
  {"bb.", SELE_BB_z},

  {"sidechain", SELE_SC_z},
  {"sc.", SELE_SC_z},

  {"p.", SELE_PROP},

  {"x", SELE_XVLx},
  {"y", SELE_YVLx},
  {"z", SELE_ZVLx},

  {"byring", SELE_RING},
  {"label", SELE_LABs},

  {"", 0}
};

#define SCMP_GTHN 0x01
#define SCMP_LTHN 0x02
#define SCMP_RANG 0x03
#define SCMP_EQAL 0x04

static WordKeyValue AtOper[] = {
  {">", SCMP_GTHN},
  {"<", SCMP_LTHN},
  {"in", SCMP_RANG},
  {"=", SCMP_EQAL},
  {"", 0}
};

static short fcmp(float a, float b, int oper) {
  switch (oper) {
  case SCMP_GTHN:
    return (a > b);
  case SCMP_LTHN:
    return (a < b);
  case SCMP_EQAL:
    return fabs(a - b) < R_SMALL4;
  }
  printf("ERROR: invalid operator %d\n", oper);
  return false;
}

#define cINTER_ENTRIES 11

int SelectorRenameObjectAtoms(PyMOLGlobals * G, ObjectMolecule * obj, int sele, int force,
                              int update_table)
{
  int result = 0;
  int obj_nAtom = obj->NAtom;

  if(update_table) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  }
  if(obj_nAtom) {
    int *flag = pymol::calloc<int>(obj_nAtom);
    if(!flag) {
      result = -1;
    } else {
      const AtomInfoType *ai = obj->AtomInfo.data();
      int a;
      for(a = 0; a < obj_nAtom; a++) {
        if(SelectorIsMember(G, ai->selEntry, sele)) {
          flag[a] = true;
          result = true;
        }
        ai++;
      }
      if (!result && !force) {
        // nothing selected, no need to continue
        return 0;
      }
      result = ObjectMoleculeRenameAtoms(obj, flag, force);
    }
    FreeP(flag);
  }
  return result;
}

int SelectorResidueVLAsTo3DMatchScores(PyMOLGlobals * G, CMatch * match,
                                       int *vla1, int n1, int state1,
                                       int *vla2, int n2, int state2,
                                       float seq_wt,
                                       float radius, float scale, float base,
                                       float coord_wt, float rms_exp)
{
  CSelector *I = G->Selector;
  int a, b, *vla;
  int n_max = (n1 > n2) ? n1 : n2;
  float *inter1 = pymol::calloc<float>(cINTER_ENTRIES * n1);
  float *inter2 = pymol::calloc<float>(cINTER_ENTRIES * n2);
  float *v_ca = pymol::calloc<float>(3 * n_max);
  if(inter1 && inter2 && v_ca) {
    int pass;

    for(pass = 0; pass < 2; pass++) {
      ObjectMolecule *obj;
      const CoordSet *cs;
      const int *neighbor = NULL;
      const AtomInfoType *atomInfo = NULL;
      const ObjectMolecule *last_obj = NULL;
      float **dist_mat;
      float *inter;
      int state;
      int n;
      if(!pass) {
        vla = vla1;
        state = state1;
        inter = inter1;
        n = n1;
        dist_mat = match->da;
      } else {
        vla = vla2;
        state = state2;
        inter = inter2;
        n = n2;
        dist_mat = match->db;
      }

      if(state < 0)
        state = 0;
      for(a = 0; a < n; a++) {
        int at_ca1;
        float *vv_ca = v_ca + a * 3;

        obj = I->Obj[vla[0]];
        at_ca1 = vla[1];
        if(obj != last_obj) {
          ObjectMoleculeUpdateNeighbors(obj);
          last_obj = obj;
          neighbor = obj->Neighbor;
          atomInfo = obj->AtomInfo;
        }

        if(state < obj->NCSet)
          cs = obj->CSet[state];
        else
          cs = NULL;
        if(cs && neighbor && atomInfo) {
          int idx_ca1 = -1;
          if(obj->DiscreteFlag) {
            if(cs == obj->DiscreteCSet[at_ca1])
              idx_ca1 = obj->DiscreteAtmToIdx[at_ca1];
            else
              idx_ca1 = -1;
          } else
            idx_ca1 = cs->AtmToIdx[at_ca1];

          if(idx_ca1 >= 0) {
            int mem0, mem1, mem2, mem3, mem4;
            int nbr0, nbr1, nbr2, nbr3;
            const float *v_ca1 = cs->Coord + 3 * idx_ca1;
            int idx_cb1 = -1;
            int cnt = 0;

            copy3f(v_ca1, vv_ca);
            copy3f(v_ca1, inter + 8);

            /* find attached CB */

            mem0 = at_ca1;
            nbr0 = neighbor[mem0] + 1;
            while((mem1 = neighbor[nbr0]) >= 0) {
              if((atomInfo[mem1].protons == cAN_C) &&
                 (atomInfo[mem1].name == G->lex_const.CB)) {
                idx_cb1 = cs->atmToIdx(mem1);
                break;
              }
              nbr0 += 2;
            }

            /* find remote CA, CB */

            if(idx_cb1 >= 0) {
              const float *v_cb1 = cs->Coord + 3 * idx_cb1;

              mem0 = at_ca1;
              nbr0 = neighbor[mem0] + 1;
              while((mem1 = neighbor[nbr0]) >= 0) {

                nbr1 = neighbor[mem1] + 1;
                while((mem2 = neighbor[nbr1]) >= 0) {
                  if(mem2 != mem0) {
                    int idx_ca2 = -1;

                    nbr2 = neighbor[mem2] + 1;
                    while((mem3 = neighbor[nbr2]) >= 0) {
                      if((mem3 != mem1) && (mem3 != mem0)) {
                        if((atomInfo[mem3].protons == cAN_C) &&
                           (atomInfo[mem3].name == G->lex_const.CA)) {
                          idx_ca2 = cs->atmToIdx(mem3);
                          break;
                        }
                      }
                      nbr2 += 2;
                    }
                    if(idx_ca2 >= 0) {
                      const float *v_ca2 = cs->Coord + 3 * idx_ca2;

                      nbr2 = neighbor[mem2] + 1;
                      while((mem3 = neighbor[nbr2]) >= 0) {
                        if((mem3 != mem1) && (mem3 != mem0)) {
                          int idx_cb2 = -1;
                          nbr3 = neighbor[mem3] + 1;
                          while((mem4 = neighbor[nbr3]) >= 0) {
                            if((mem4 != mem2) && (mem4 != mem1) && (mem4 != mem0)) {
                              if((atomInfo[mem4].protons == cAN_C) &&
                                 (atomInfo[mem4].name == G->lex_const.CB)) {
                                idx_cb2 = cs->atmToIdx(mem4);
                                break;
                              }
                            }
                            nbr3 += 2;
                          }

                          if(idx_cb2 >= 0) {
                            const float *v_cb2 = NULL;
                            v_cb2 = cs->Coord + 3 * idx_cb2;
                            {
                              float angle = get_dihedral3f(v_cb1, v_ca1, v_ca2, v_cb2);
                              if(idx_cb1 < idx_cb2) {
                                inter[0] = (float) cos(angle);
                                inter[1] = (float) sin(angle);
                              } else {
                                inter[2] = (float) cos(angle);
                                inter[3] = (float) sin(angle);
                              }
                            }
                            cnt++;
                          }
                        }
                        nbr2 += 2;
                      }
                    }
                  }
                  nbr1 += 2;
                }
                nbr0 += 2;
              }
            }
          }
        }
        vla += 3;
        inter += cINTER_ENTRIES;
      }
      if(dist_mat) {
        for(a = 0; a < n; a++) {        /* optimize this later */
          float *vv_ca = v_ca + a * 3;
          for(b = 0; b < n; b++) {
            float *vv_cb = v_ca + b * 3;
            float diff = (float) diff3f(vv_ca, vv_cb);
            dist_mat[a][b] = diff;
            dist_mat[b][a] = diff;
          }
        }
      }
      {
        MapType *map = MapNew(G, radius, v_ca, n, NULL);
        if(!pass) {
          inter = inter1;
        } else {
          inter = inter2;
        }
        if(map) {
          int i, h, k, l;
          MapSetupExpress(map);

          for(a = 0; a < n; a++) {
            float *v_ca1 = v_ca + 3 * a;
            float *i_ca1 = inter + cINTER_ENTRIES * a;
            if(MapExclLocus(map, v_ca1, &h, &k, &l)) {
              i = *(MapEStart(map, h, k, l));
              if(i) {
                b = map->EList[i++];
                while(b >= 0) {
                  float *v_ca2 = v_ca + 3 * b;
                  if(a != b) {
                    if(within3f(v_ca1, v_ca2, radius)) {
                      float *i_ca2 = inter + cINTER_ENTRIES * b;
                      i_ca1[4] += i_ca2[0];     /* add dihedral vectors head-to-tail */
                      i_ca1[5] += i_ca2[1];
                      i_ca1[6] += i_ca2[2];
                      i_ca1[7] += i_ca2[3];
                    }
                  }
                  b = map->EList[i++];
                }
              }
            }
          }
          MapFree(map);
          for(a = 0; a < n; a++) {
            float nf = (float) sqrt(inter[4] * inter[4] + inter[5] * inter[5]);
            if(nf > 0.0001F) {
              inter[4] = inter[4] / nf;
              inter[5] = inter[5] / nf;
            }
            nf = (float) sqrt(inter[6] * inter[6] + inter[7] * inter[7]);
            if(nf > 0.0001F) {

              inter[6] = inter[6] / nf;
              inter[7] = inter[7] / nf;
            }
            inter += cINTER_ENTRIES;
          }
        }
      }
    }
    {
      const float _0F = 0.0F;

      if((scale != 0.0F) || (seq_wt != 0.0F)) {
        for(a = 0; a < n1; a++) {
          float *i1 = inter1 + cINTER_ENTRIES * a;
          for(b = 0; b < n2; b++) {
            float *i2 = inter2 + cINTER_ENTRIES * b;
            float sm[cINTER_ENTRIES], comp1, comp2, comp3 = 1.0F;
            float score;
            int c;
            for(c = 0; c < (cINTER_ENTRIES - 1); c += 2) {
              if(((i1[c] == _0F) && (i1[c + 1] == _0F))
                 || ((i2[c] == _0F) && (i2[c + 1] == _0F))) {
                /* handle glycine case */
                sm[c] = 1.0F;
                sm[c + 1] = 1.0F;
              } else {
                sm[c] = i1[c] + i2[c];
                sm[c + 1] = i1[c + 1] + i2[c + 1];
              }
            }
            comp1 = (float)
              ((sqrt(sm[0] * sm[0] + sm[1] * sm[1]) +
                sqrt(sm[2] * sm[2] + sm[3] * sm[3])) * 0.25);
            comp2 = (float)
              ((sqrt(sm[4] * sm[4] + sm[5] * sm[5]) +
                sqrt(sm[6] * sm[6] + sm[7] * sm[7])) * 0.25);
            score = scale * (comp1 * comp2 - base);
            if(coord_wt != 0.0) {
              float diff = (float) diff3f(i1 + 8, i2 + 8);
              comp3 = (float) -log(diff / rms_exp);
              score = (1 - coord_wt) * score + coord_wt * comp3 * scale;
            }
            match->mat[a][b] = seq_wt * match->mat[a][b] + score;
          }
        }
      }
    }
  }
  FreeP(inter1);
  FreeP(inter2);
  FreeP(v_ca);
  return 1;
}

int SelectorNameIsKeyword(PyMOLGlobals * G, const char *name)
{
  CSelector *I = G->Selector;
  WordType lower_name;
  OVreturn_word result;
  UtilNCopyToLower(lower_name, name, sizeof(WordType));
  if(OVreturn_IS_OK((result = OVLexicon_BorrowFromCString(I->Lex, lower_name)))) {
    if(OVreturn_IS_OK((result = OVOneToAny_GetKey(I->Key, result.word)))) {
      return 1;
    }
  }
  return 0;
}


/*========================================================================*/
static int SelectorIsSelectionDiscrete(PyMOLGlobals * G, int sele, int update_table)
{
  CSelector *I = G->Selector;
  ObjectMolecule **i_obj = I->Obj, *obj;
  AtomInfoType *ai;
  int result = false;
  TableRec *i_table = I->Table, *table_a;
  ov_size a;

  if(update_table) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  }

  table_a = i_table + cNDummyAtoms;
  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    obj = i_obj[table_a->model];
    ai = obj->AtomInfo + table_a->atom;
    if(SelectorIsMember(G, ai->selEntry, sele)) {
      if(obj->DiscreteFlag) {
        result = true;
        break;
      }
    }
    table_a++;
  }
  return (result);
}

static sele_array_t SelectorUpdateTableMultiObjectIdxTag(PyMOLGlobals * G,
                                                 ObjectMolecule ** obj_list,
                                                 int no_dummies,
                                                 int **idx_list, int *n_idx_list,
                                                 int n_obj)
{
  int a = 0;
  int b;
  int c = 0;
  int modelCnt;
  sele_array_t result{};
  CSelector *I = G->Selector;
  ObjectMolecule *obj = NULL;
  int *idx, n_idx;

  PRINTFD(G, FB_Selector)
    "SelectorUpdateTableMultiObject-Debug: entered ...\n" ENDFD;

  SelectorClean(G);

  I->SeleBaseOffsetsValid = true;       /* all states -> all atoms -> offsets valid */
  I->NCSet = 0;
  if(no_dummies) {
    modelCnt = 0;
    c = 0;
  } else {
    modelCnt = cNDummyModels;
    c = cNDummyAtoms;
  }
  for(b = 0; b < n_obj; b++) {
    obj = obj_list[b];
    c += obj->NAtom;
    if(I->NCSet < obj->NCSet)
      I->NCSet = obj->NCSet;
    modelCnt++;
  }
  sele_array_calloc(result, c);
  I->Table = pymol::calloc<TableRec>(c);
  ErrChkPtr(G, I->Table);
  I->Obj = pymol::calloc<ObjectMolecule *>(modelCnt);
  ErrChkPtr(G, I->Obj);
  if(no_dummies) {
    modelCnt = 0;
    c = 0;
  } else {
    c = cNDummyAtoms;
    modelCnt = cNDummyModels;
  }
  for(b = 0; b < n_obj; b++) {
    obj = obj_list[b];
    idx = idx_list[b];
    n_idx = n_idx_list[b];

    I->Obj[modelCnt] = obj;
    obj->SeleBase = c;
    for(a = 0; a < obj->NAtom; a++) {
      I->Table[c].model = modelCnt;
      I->Table[c].atom = a;
      c++;
    }
    if(idx && n_idx) {
      if(n_idx > 0) {
        for(a = 0; a < n_idx; a++) {
          int at = idx[2 * a];  /* index first */
          int pri = idx[2 * a + 1];     /* then priority */
          if((at >= 0) && (at < obj->NAtom)) {
            result[obj->SeleBase + at] = pri;
          }
        }
      }
    }
    modelCnt++;
    I->NModel = modelCnt;
  }
  I->NAtom = c;
  I->Flag1 = pymol::malloc<int>(c);
  ErrChkPtr(G, I->Flag1);
  I->Flag2 = pymol::malloc<int>(c);
  ErrChkPtr(G, I->Flag2);
  I->Vertex = pymol::malloc<float>(c * 3);
  ErrChkPtr(G, I->Vertex);

  PRINTFD(G, FB_Selector)
    "SelectorUpdateTableMultiObject-Debug: leaving...\n" ENDFD;

  return (result);
}

static int IntInOrder(const int *list, int a, int b)
{
  return (list[a] <= list[b]);
}

static int SelectorAddName(PyMOLGlobals * G, int index)
{
  CSelector *I = G->Selector;
  int ok = false;
  OVreturn_word result;
  OVstatus status;
  if(OVreturn_IS_OK((result = OVLexicon_GetFromCString(I->Lex, I->Name[index])))) {
    if(OVreturn_IS_OK((status = OVOneToOne_Set(I->NameOffset, result.word, index)))) {
      ok = true;
    }
  }
  return ok;
}

static int SelectorDelName(PyMOLGlobals * G, int index)
{
  CSelector *I = G->Selector;
  int ok = false;
  OVreturn_word result;
  if(OVreturn_IS_OK((result = OVLexicon_BorrowFromCString(I->Lex, I->Name[index])))) {
    if(OVreturn_IS_OK(OVLexicon_DecRef(I->Lex, result.word)) &&
       OVreturn_IS_OK(OVOneToOne_DelForward(I->NameOffset, result.word))) {
      ok = true;
    }
  }
  return ok;
}

int SelectorClassifyAtoms(PyMOLGlobals * G, int sele, int preserve,
                          ObjectMolecule * only_object)
{
  CSelector *I = G->Selector;
  ObjectMolecule *obj, *last_obj = NULL, *obj0, *obj1 = NULL;
  int a, aa, at, a0, a1;
  AtomInfoType *ai, *last_ai = NULL, *ai0, *ai1;
  unsigned int mask;
  int n_dummies = 0;

  int auto_show_classified = SettingGetGlobal_i(G, cSetting_auto_show_classified);
  int auto_show_mask = (auto_show_classified != 2) ? 0 : cRepBitmask;

  int visRep_organic = cRepCylBit | cRepNonbondedSphereBit;
  int visRep_inorganic = cRepSphereBit;
  int visRep_polymer = cRepCartoonBit;

  // detect large systems
  if (auto_show_classified == -1 &&
      only_object &&
      only_object->NAtom * (only_object->DiscreteFlag ?
        1 : only_object->NCSet) > 5e5) {
    auto_show_classified = 3;
  }

  if (auto_show_classified == 3) {
    visRep_organic = visRep_inorganic = cRepLineBit | cRepNonbondedBit;
    visRep_polymer = cRepRibbonBit;
  }

  if(only_object) {
    SelectorUpdateTableSingleObject(G, only_object, cSelectorUpdateTableAllStates,
                                    true, NULL, 0, false);
    n_dummies = 0;
  } else {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
    n_dummies = cNDummyAtoms;
  }
  a = 0;
  while(a < I->NAtom) {
    obj = I->Obj[I->Table[a].model];
    at = I->Table[a].atom;
    ai = obj->AtomInfo + at;

    if(SelectorIsMember(G, ai->selEntry, sele) &&
       ((!AtomInfoSameResidueP(G, ai, last_ai)))) {

      AtomInfoType *guide_atom = NULL;

      /* delimit residue */

      a0 = a - 1;
      while(a0 >= n_dummies) {
        obj0 = I->Obj[I->Table[a0].model];
        if(obj0 != obj)
          break;
        ai0 = obj0->AtomInfo + I->Table[a0].atom;
        if(!AtomInfoSameResidue(G, ai0, ai))
          break;
        a0--;
      }

      a1 = a + 1;
      while(a1 < I->NAtom) {
        obj1 = I->Obj[I->Table[a1].model];
        if(obj1 != obj)
          break;
        ai1 = obj1->AtomInfo + I->Table[a1].atom;
        if(!AtomInfoSameResidue(G, ai1, ai))
          break;
        a1++;
      }

      a0++;
      a1--;

      mask = 0;
      if(!ai->hetatm && AtomInfoKnownProteinResName(LexStr(G, ai->resn)))
        mask = cAtomFlag_polymer | cAtomFlag_protein;
      else if(!ai->hetatm && AtomInfoKnownNucleicResName(LexStr(G, ai->resn)))
        mask = cAtomFlag_polymer | cAtomFlag_nucleic;
      else if(AtomInfoKnownWaterResName(G, LexStr(G, ai->resn)))
        mask = cAtomFlag_solvent;
      else {

        /* does this residue have a canonical atoms? */

        int found_ca = false;
        int found_n = false;
        int found_c = false;
        int found_o = false;
        int found_oh2 = false;
        int found_carbon = false;
        int found_cn_bond = false;
        int found_nc_bond = false;
        int found_o3_bond = false;
        int found_o3star = false;
        int found_c3star = false;
        int found_c4star = false;
        int found_c5star = false;
        int found_o5star = false;
        int found_p_bond = false;
        if(obj != last_obj) {
          ObjectMoleculeUpdateNeighbors(obj);
          last_obj = obj;
        }

        ai0 = obj->AtomInfo + I->Table[a0].atom;
        for(aa = a0; aa <= a1; aa++) {
          if(ai0->protons == cAN_C) {
            const char *name = LexStr(G, ai0->name);
            found_carbon = true;
            switch (name[0]) {
            case 'C':
              switch (name[1]) {
              case 0:
                found_c = true;
                found_cn_bond =
                  ObjectMoleculeIsAtomBondedToName(obj, I->Table[aa].atom, "N", 0);
                break;
              case 'A':
                switch (name[2]) {
                case 0:
                  found_ca = true;
                  guide_atom = ai0;
                  break;
                }
              case '3':
                switch (name[2]) {
                case '*':
                case '\'':
                  guide_atom = ai0;
                  found_c3star = true;
                  break;
                }
                break;
              case '4':
                switch (name[2]) {
                case '*':
                case '\'':
                  found_c4star = true;
                  break;
                }
                break;
              case '5':
                switch (name[2]) {
                case '*':
                case '\'':
                  found_c5star = true;
                  break;
                }
                break;
              }
            }
          } else if(ai0->protons == cAN_N) {
            const char *name = LexStr(G, ai0->name);
            switch (name[0]) {
            case 'N':
              switch (name[1]) {
              case 0:
                found_n = true;
                found_nc_bond =
                  ObjectMoleculeIsAtomBondedToName(obj, I->Table[aa].atom, "C", 0);
                break;
              }
            }
          } else if(ai0->protons == cAN_O) {
            const char *name = LexStr(G, ai0->name);
            switch (name[0]) {
            case 'O':
              switch (name[1]) {
              case 0:
                found_o = true;
                break;
              case 'H':
                switch (name[2]) {
                case '2':
                  found_oh2 = true;
                  break;
                }
              case '3':
                switch (name[2]) {
                case '*':
                case '\'':
                  found_o3star = true;
                  found_o3_bond =
                    ObjectMoleculeIsAtomBondedToName(obj, I->Table[aa].atom, "P", 0);
                  break;
                }
                break;
              case '5':
                switch (name[2]) {
                case '*':
                case '\'':
                  found_o5star = true;
                  break;
                }
                break;
              }
            }
          } else if(ai0->protons == cAN_P) {

            const char *name = LexStr(G, ai0->name);
            switch (name[0]) {
            case 'P':
              switch (name[1]) {
              case 0:
                found_p_bond =
                  (ObjectMoleculeIsAtomBondedToName(obj, I->Table[aa].atom, "O3*", 0)
                   || ObjectMoleculeIsAtomBondedToName(obj, I->Table[aa].atom, "O3'", 0));
                break;
              }
            }
          }
          ai0++;
        }

        if(found_ca && found_n && found_c && found_o && (found_cn_bond || found_nc_bond)) {
          mask = cAtomFlag_polymer | cAtomFlag_protein;
        } else if (found_o3star && found_c3star && found_c4star && found_c5star
               && found_o5star && (found_o3_bond || found_p_bond)) {
          mask = cAtomFlag_polymer | cAtomFlag_nucleic;
        } else if(found_carbon)
          mask = cAtomFlag_organic;
        else if((found_o || found_oh2) && (a1 == a0))
          mask = cAtomFlag_solvent;
        else
          mask = cAtomFlag_inorganic;
      }

      /* mark which atoms we can write to */

      ai0 = obj->AtomInfo + I->Table[a0].atom;
      if(preserve) {
        printf("NOT IMPLEMENTED\n");
      } else {
        auto visRep_polymer_obj = visRep_polymer;
        if (obj->NAtom < 50) {
          // prevent single residue objects from disappearing
          visRep_polymer_obj |= visRep_organic;
        }

        for(aa = a0; aa <= a1; aa++) {
          if(SelectorIsMember(G, ai0->selEntry, sele))
          {
            // apply styles if atom was unclassified
            if (auto_show_classified && !(ai0->flags & cAtomFlag_class)) {
              if (mask & cAtomFlag_organic) {
                ai0->visRep = (ai0->visRep & auto_show_mask) | visRep_organic;
              } else if (mask & cAtomFlag_inorganic) {
                ai0->visRep = (ai0->visRep & auto_show_mask) | visRep_inorganic;
              } else if (mask & cAtomFlag_polymer) {
                ai0->visRep = (ai0->visRep & auto_show_mask) | visRep_polymer_obj;
              }
            }
            ai0->flags = (ai0->flags & cAtomFlag_class_mask) | mask;
          }
          ai0++;
        }
      }

      if((mask & cAtomFlag_polymer)) {
        ai0 = obj->AtomInfo + I->Table[a0].atom;
        for(aa = a0; !guide_atom && aa <= a1; aa++) {
          if(ai0->protons == cAN_C) {
            const char *name = LexStr(G, ai0->name);
            switch (name[0]) {
            case 'C':
              switch (name[1]) {
              case 'A':
                switch (name[2]) {
                case 0:
                  guide_atom = ai0;
                  break;
                }
                break;
              case '4':
                switch (name[2]) {      /* use C4* as guide atom for nucleic acids */
                case '*':
                case '\'':
                  guide_atom = ai0;
                  break;
                }
                break;
              }
            }
          }
          ai0++;
        }
      }

      if(guide_atom)
        guide_atom->flags |= cAtomFlag_guide;

      if(a1 > (a + 1))
        a = a1;
    }
    a++;
  }
  return true;
}

static void SelectionInfoInit(SelectionInfoRec * rec)
{
  rec->justOneObjectFlag = false;
  rec->justOneAtomFlag = false;
}

void SelectorComputeFragPos(PyMOLGlobals * G, ObjectMolecule * obj, int state, int n_frag,
                            char *prefix, float **vla)
{
  CSelector *I = G->Selector;
  WordType name;
  int *sele;
  int *cnt;
  SelectorUpdateTableSingleObject(G, obj, cSelectorUpdateTableAllStates, true, NULL, 0,
                                  false);
  sele = pymol::malloc<int>(n_frag);
  cnt = pymol::calloc<int>(n_frag);
  VLACheck(*vla, float, n_frag * 3 + 2);
  {
    int a;
    for(a = 0; a < n_frag; a++) {
      sprintf(name, "%s%d", prefix, a + 1);
      sele[a] = SelectorIndexByName(G, name);
      zero3f((*vla) + 3 * a);
    }
  }
  {
    int at, a, ati;
    AtomInfoType *ai;
    float v[3], *vp;
    int vert_flag;
    for(at = 0; at < I->NAtom; at++) {

      ati = I->Table[at].atom;
      ai = obj->AtomInfo + ati;

      vert_flag = false;
      for(a = 0; a < n_frag; a++) {
        if(SelectorIsMember(G, ai->selEntry, sele[a])) {
          if(!vert_flag) {
            vert_flag = ObjectMoleculeGetAtomVertex(obj, state, ati, v);
          }
          if(vert_flag) {
            vp = (*vla) + 3 * a;
            add3f(v, vp, vp);
            cnt[a]++;
          }
        }
      }
    }
  }

  {
    int a;
    float div, *vp;
    for(a = 0; a < n_frag; a++) {
      if(cnt[a]) {
        vp = (*vla) + 3 * a;
        div = 1.0F / cnt[a];
        scale3f(vp, div, vp);
      }
    }
  }

  FreeP(sele);
  FreeP(cnt);
}

MapType *SelectorGetSpacialMapFromSeleCoord(PyMOLGlobals * G, int sele, int state,
                                            float cutoff, float **coord_vla)
{
  //  register CSelector *I = G->Selector;

  int *index_vla = NULL;
  float *coord = NULL;
  int n, nc = 0;
  MapType *result = NULL;
  if(sele < 0)
    return NULL;
  else {
    CSelector *I = NULL;
    SelectorInitImpl(G, &I, 0);
    SelectorUpdateTableImpl(G, I, state, -1);
    index_vla = SelectorGetIndexVLAImpl(G, I, sele);

    if(index_vla) {
      n = VLAGetSize(index_vla);
      if(n)
        coord = VLAlloc(float, n * 3);
      if(coord) {
        int i, a;
        int st, sta;
        ObjectMolecule *obj;
        CoordSet *cs;
        int at;
        int idx;
        for(i = 0; i < n; i++) {
          a = index_vla[i];

          obj = I->Obj[I->Table[a].model];
          at = +I->Table[a].atom;
          for(st = 0; st < I->NCSet; st++) {

            if((state < 0) || (st == state)) {

              sta = st;
              if(sta < obj->NCSet)
                cs = obj->CSet[sta];
              else
                cs = NULL;
              if(cs) {
                idx = cs->atmToIdx(at);
              } else {
                idx = -1;
              }
              if(idx >= 0) {
                VLACheck(coord, float, nc * 3 + 2);
                const float* src = cs->Coord + 3 * idx;
                float* dst = coord + 3 * nc;
                copy3f(src, dst);
                nc++;
              }
            }
          }
        }
        if(nc) {
          result = MapNew(G, cutoff, coord, nc, NULL);
        }
      }
    }
    SelectorFreeImpl(G, I, 0);
  }
  VLAFreeP(index_vla);
  if(coord)
    VLASize(coord, float, nc * 3);
  *(coord_vla) = coord;
  return (result);
}

static ov_diff SelectGetNameOffset(PyMOLGlobals * G, const char *name, ov_size minMatch,
                                   int ignCase)
{

  CSelector *I = G->Selector;
  int result = -1;
  while(name[0] == '?')
    name++;
  {                             /* first try for perfect match using the dictionary */
    OVreturn_word res;
    if(OVreturn_IS_OK((res = OVLexicon_BorrowFromCString(I->Lex, name)))) {
      if(OVreturn_IS_OK((res = OVOneToOne_GetForward(I->NameOffset, res.word)))) {
        result = res.word;
      }
    }
  }
  if(result < 0) {              /* not found, so try partial/ignored-case match */

    int offset, wm, best_match, best_offset;
    SelectorWordType *I_Name = I->Name;
    offset = 0;
    best_offset = -1;
    best_match = -1;

    while(I_Name[offset][0]) {
      wm = WordMatch(G, name, I_Name[offset], ignCase);
      if(wm < 0) {              /* exact match is always good */
        best_offset = offset;
        best_match = wm;
        break;
      }
      if(wm > 0) {
        if(best_match < wm) {
          best_match = wm;
          best_offset = offset;
        } else if(best_match == wm) {   /* uh oh -- ambiguous match */
          best_offset = -1;
        }
      }
      offset++;
    }
    if((best_match < 0) || (best_match > (ov_diff) minMatch))
      result = best_offset;
  }
  return (result);
}

void SelectorSelectByID(PyMOLGlobals * G, const char *name, ObjectMolecule * obj, int *id,
                        int n_id)
{
  CSelector *I = G->Selector;
  int min_id, max_id, range, *lookup = NULL;
  int *atom = NULL;
  /* this routine only works if IDs cover a reasonable range --
     should rewrite using a hash table */

  SelectorUpdateTableSingleObject(G, obj, cSelectorUpdateTableAllStates, true, NULL, 0,
                                  false);
  atom = pymol::calloc<int>(I->NAtom);
  if(I->NAtom) {

    /* determine range */

    {
      int a, cur_id;
      cur_id = obj->AtomInfo[0].id;
      min_id = cur_id;
      max_id = cur_id;
      for(a = 1; a < obj->NAtom; a++) {
        cur_id = obj->AtomInfo[a].id;
        if(min_id > cur_id)
          min_id = cur_id;
        if(max_id < cur_id)
          max_id = cur_id;
      }
    }

    /* create cross-reference table */

    {
      int a, offset;

      range = max_id - min_id + 1;
      lookup = pymol::calloc<int>(range);
      for(a = 0; a < obj->NAtom; a++) {
        offset = obj->AtomInfo[a].id - min_id;
        if(lookup[offset])
          lookup[offset] = -1;
        else {
          lookup[offset] = a + 1;
        }
      }
    }

    /* iterate through IDs and mark */

    {
      int i, a, offset, lkup;

      for(i = 0; i < n_id; i++) {
        offset = id[i] - min_id;
        if((offset >= 0) && (offset < range)) {
          lkup = lookup[offset];
          if(lkup > 0) {
            atom[lkup - 1] = true;
          } else if(lkup < 0) {
            for(a = 0; a < obj->NAtom; a++) {
              if(obj->AtomInfo[a].id == id[i])
                atom[a] = true;
            }
          }
        }
      }
    }
  }

  SelectorEmbedSelection(G, atom, name, NULL, true, -1);
  FreeP(atom);
  FreeP(lookup);
  SelectorClean(G);
}

void SelectorDefragment(PyMOLGlobals * G)
{
  CSelector *I = G->Selector;
  /* restore new member ordering so that CPU can continue to get good cache hit */

  int n_free = 0;
  int m;
  int *list, *l;
  int a;
  m = I->FreeMember;
  while(m) {
    n_free++;
    m = I->Member[m].next;
  }
  if(n_free) {
    list = pymol::malloc<int>(n_free);
    l = list;
    m = I->FreeMember;
    while(m) {
      *(l++) = m;
      m = I->Member[m].next;
    }
    UtilSortInPlace(G, list, n_free, sizeof(int), (UtilOrderFn *) IntInOrder);
    while(n_free > 5000) {      /* compact inactive members when possible */
      if(list[n_free - 1] == I->NMember) {
        I->NMember--;
        n_free--;
      } else
        break;
    }
    for(a = 0; a < (n_free - 1); a++) {
      I->Member[list[a]].next = list[a + 1];
    }
    I->Member[list[n_free - 1]].next = 0;
    I->FreeMember = list[0];
    FreeP(list);
  }
}

typedef struct {
  int color;
  int sele;
} ColorectionRec;

static void SelectorDeleteSeleAtOffset(PyMOLGlobals * G, int n)
{
  CSelector *I = G->Selector;
  int id;
  id = I->Info[n].ID;
  SelectorDelName(G, n);
  SelectorPurgeMembers(G, id);

  I->NActive--;
  {
    OVreturn_word result;
    /* repoint the name index at the relocated entry */
    if(OVreturn_IS_OK(result = OVOneToOne_GetReverse(I->NameOffset, I->NActive))) {
      OVOneToOne_DelForward(I->NameOffset, result.word);
      OVOneToOne_Set(I->NameOffset, result.word, n);
    }
    if (I->Name[n]!=I->Name[I->NActive])
      strcpy(I->Name[n], I->Name[I->NActive]);
    I->Info[n] = I->Info[I->NActive];
    I->Name[I->NActive][0] = 0;
  }
}

char *SelectorGetNameFromIndex(PyMOLGlobals * G, int index)
{
  CSelector *I = G->Selector;
  int a;
  for(a = 1; a < I->NActive; a++) {
    if(I->Info[a].ID == index) {
      return I->Name[a];
    }
  }
  return NULL;
}

#ifndef _PYMOL_NOPY
static void SelectorDeleteIndex(PyMOLGlobals * G, int index)
{
  CSelector *I = G->Selector;
  int n = 0;
  int a;
  for(a = 1; a < I->NActive; a++) {
    if(I->Info[a].ID == index) {
      n = a;
      break;
    }
  }
  if(n)
    SelectorDeleteSeleAtOffset(G, n);
}
#endif

#define cSSMaxHBond 6

#define cSSHelix3HBond          0x0001
#define cSSHelix4HBond          0x0002
#define cSSHelix5HBond          0x0004
#define cSSGotPhiPsi            0x0008
#define cSSPhiPsiHelix          0x0010
#define cSSPhiPsiNotHelix       0x0020
#define cSSPhiPsiStrand         0x0040
#define cSSPhiPsiNotStrand      0x0080
#define cSSAntiStrandSingleHB   0x0100
#define cSSAntiStrandDoubleHB   0x0200
#define cSSAntiStrandBuldgeHB   0x0400
#define cSSAntiStrandSkip       0x0800
#define cSSParaStrandSingleHB   0x1000
#define cSSParaStrandDoubleHB   0x2000
#define cSSParaStrandSkip       0x4000

#define cSSBreakSize 5

typedef struct {
  int real;
  int ca, n, c, o;              /* indices in selection-table space */
  float phi, psi;
  char ss, ss_save;
  int flags;
  int n_acc, n_don;
  int acc[cSSMaxHBond];         /* interactions where this residue is an acceptor */
  int don[cSSMaxHBond];         /* interactions where this residue is a donor */
  ObjectMolecule *obj;
  int preserve;
  int present;
} SSResi;

int SelectorAssignSS(PyMOLGlobals * G, int target, int present,
                     int state_value, int preserve, ObjectMolecule * single_object,
                     int quiet)
{

  /* PyMOL's secondary structure assignment algorithm: 

     General principal -- if it looks like a duck, then it's a duck:

     I. Helices
     - must have reasonably helical geometry within the helical span
     - near-ideal geometry guarantees helix assignment
     - a continuous ladder stre i+3, i+4, or i+5 hydrogen bonding
     with permissible geometry can reinforce marginal cases
     - a minimum helix is three residues with i+3 H-bond

     II. Sheets
     - Hydrogen bonding ladders are the primary guide
     - Out-of-the envelope 
     - 1-residue gaps in sheets are filled unless there
     is a turn.
   */

  CSelector *I = G->Selector;
  SSResi *res;
  int n_res = 0;
  int state_start, state_stop, state;
  int consensus = true;
  int first_last_only = false;
  int first_pass = true;

  if(!single_object) {
    if(state_value < 0) {
      switch (state_value) {
      case -2:                 /* api: state=-1: current global state */
      case -3:                 /* api: state=-2: effective object states TO DO! */
        SelectorUpdateTable(G, state_value, -1);
        break;
      default:
        SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
        break;
      }
    } else {
      SelectorUpdateTable(G, state_value, -1);
    }
  } else {
    SelectorUpdateTableSingleObject(G, single_object, state_value, false, NULL, 0, 0);
  }

  res = VLACalloc(SSResi, 1000);

  if(state_value < 0) {
    if(state_value == -4)
      consensus = false;
    if(state_value == -5)
      first_last_only = true;
    state_start = 0;
    state_stop = SelectorGetSeleNCSet(G, target);

    if (state_value == -2) {
      /* api: state=-1: current global state */
      StateIterator iter(G, NULL, state_value, state_stop);
      if (iter.next()) {
        state_start = iter.state;
        state_stop = iter.state + 1;
      }
    }
  } else {
    state_start = state_value;
    state_stop = state_value + 1;
  }
  for(state = state_start; state < state_stop; state++) {
    int a;
    ObjectMolecule *obj;
    int aa, a0, a1, at, idx;
    AtomInfoType *ai, *ai0, *ai1;
    CoordSet *cs;
    ObjectMolecule *last_obj = NULL;
    /* first, we need to count the number of residues under consideration */

    if(first_pass) {
      for(a = cNDummyAtoms; a < I->NAtom; a++) {

        obj = I->Obj[I->Table[a].model];
        at = +I->Table[a].atom;
        ai = obj->AtomInfo + at;

        /* see if CA coordinates exist... */

        if(SelectorIsMember(G, ai->selEntry, present)) {

          if((ai->protons == cAN_C) && (WordMatchExact(G, G->lex_const.CA, ai->name, true))) {

            if(last_obj != obj) {
              ObjectMoleculeUpdateNeighbors(obj);
              ObjectMoleculeVerifyChemistry(obj, state_value);
              last_obj = obj;
            }
            /* delimit residue */

            a0 = a - 1;
            while(a0 >= cNDummyAtoms) {
              ai0 = I->Obj[I->Table[a0].model]->AtomInfo + I->Table[a0].atom;
              if(!AtomInfoSameResidue(G, ai0, ai))
                break;
              a0--;
            }

            a1 = a + 1;
            while(a1 < I->NAtom) {
              ai1 = I->Obj[I->Table[a1].model]->AtomInfo + I->Table[a1].atom;
              if(!AtomInfoSameResidue(G, ai1, ai))
                break;
              a1++;
            }

            {
              int found_N = 0;
              int found_O = 0;
              int found_C = 0;

              /* locate key atoms */

              for(aa = a0 + 1; aa < a1; aa++) {
                ai = I->Obj[I->Table[aa].model]->AtomInfo + I->Table[aa].atom;
                if((ai->protons == cAN_C) && (WordMatchExact(G, G->lex_const.C, ai->name, true))) {
                  found_C = aa;
                }
                if((ai->protons == cAN_N) && (WordMatchExact(G, G->lex_const.N, ai->name, true))) {
                  found_N = aa;
                }
                if((ai->protons == cAN_O) && (WordMatchExact(G, G->lex_const.O, ai->name, true))) {
                  found_O = aa;
                }
              }

              if((found_C) && (found_N) && (found_O)) {

                VLACheck(res, SSResi, n_res);
                res[n_res].n = found_N;
                res[n_res].o = found_O;
                res[n_res].c = found_C;
                res[n_res].ca = a;
                res[n_res].obj = I->Obj[I->Table[a].model];
                res[n_res].real = true;

                n_res++;

              } else {
                if(!quiet) {
                  PRINTFB(G, FB_Selector, FB_Warnings)
                    " AssignSS-Warning: Ignoring incomplete residue /%s/%s/%s/%d%c ...\n",
                    obj->Name, LexStr(G, ai->segi), LexStr(G, ai->chain), ai->resv, ai->getInscode(true) ENDFB(G);
                }
              }
            }
          }
        }
      }                         /* count pass */

      if(preserve) {            /* if we're in preserve mode, then mark which objects don't get changed */
        int a, b;
        char ss;
        ObjectMolecule *p_obj = NULL;
        SSResi *r, *r2;
        for(a = 0; a < n_res; a++) {
          r = res + a;
          if(r->real) {
            if(p_obj != r->obj) {
              ss = r->obj->AtomInfo[I->Table[r->ca].atom].ssType[0];
              if((ss == 'S') || (ss == 'H') || (ss == 's') || (ss == 'h')) {
                p_obj = r->obj;

                b = a;
                while(b >= 0) {
                  r2 = res + b;
                  if(p_obj == r2->obj)
                    r2->preserve = true;
                  b--;
                }
                b = a + 1;
                while(b < n_res) {
                  r2 = res + b;
                  if(p_obj == r2->obj)
                    r2->preserve = true;
                  b++;
                }
              }
            }
          }
        }
      }
      /*  printf("n_res %d\n",n_res); */

      /* now, let's repack res. into discrete chunks so that we can do easy gap & ladder analysis */

      {
        SSResi *res2;
        int a;
        int n_res2 = 0;
        int add_break;
        int at_ca0, at_ca1;

        res2 = VLACalloc(SSResi, n_res * 2);

        for(a = 0; a < n_res; a++) {
          add_break = false;

          if(!a) {
            add_break = true;
          } else if(res[a].obj != res[a - 1].obj) {
            add_break = true;
          } else if(res[a].obj) {
            at_ca0 = I->Table[res[a].ca].atom;
            at_ca1 = I->Table[res[a - 1].ca].atom;
            if(!ObjectMoleculeCheckBondSep(res[a].obj, at_ca0, at_ca1, 3)) {    /* CA->N->C->CA = 3 bonds */
              add_break = true;
            }
          }

          if(add_break) {
            n_res2 += cSSBreakSize;
          }

          VLACheck(res2, SSResi, n_res2);
          res2[n_res2] = res[a];
          n_res2++;
        }

        n_res2 += cSSBreakSize;
        VLACheck(res2, SSResi, n_res2);

        VLAFreeP(res);
        res = res2;
        n_res = n_res2;
      }
      first_pass = false;
    }

    /* okay, the rest of this loop runs for each coordinate set */

    {
      int b;
      for(a = 0; a < n_res; a++) {
        res[a].present = res[a].real;

        if(res[a].present) {
          obj = res[a].obj;
          if(state < obj->NCSet)
            cs = obj->CSet[state];
          else
            cs = NULL;
          for(b = 0; b < 4; b++) {
            if(cs) {
              switch (b) {
              case 0:
                at = I->Table[res[a].n].atom;
                break;
              case 1:
                at = I->Table[res[a].o].atom;
                break;
              case 2:
                at = I->Table[res[a].c].atom;
                break;
              default:
              case 3:
                at = I->Table[res[a].ca].atom;
                break;
              }
              idx = cs->atmToIdx(at);
            } else
              idx = -1;
            if(idx < 0) {
              res[a].present = false;
            }
          }
        }
      }
    }

    /* next, we need to record hydrogen bonding relationships */

    {

      MapType *map;
      float *v0, *v1;
      int n1;
      int i, h, k, l;
      int at;

      int a, aa;
      int a0, a1;               /* SS res space */
      int as0, as1;             /* selection space */
      int at0, at1;             /* object-atom space */
      int exclude;

      ObjectMolecule *obj0, *obj1;

      CoordSet *cs;
      float cutoff;
      HBondCriteria hbcRec, *hbc;
      int *zero = NULL, *scratch = NULL;

      {
        int max_n_atom = I->NAtom;
        ObjectMolecule *lastObj = NULL;
        for(a = cNDummyAtoms; a < I->NAtom; a++) {
          ObjectMolecule *obj = I->Obj[I->Table[a].model];
          if(obj != lastObj) {
            if(max_n_atom < obj->NAtom)
              max_n_atom = obj->NAtom;
            lastObj = obj;
          }
        }
        zero = pymol::calloc<int>(max_n_atom);
        scratch = pymol::malloc<int>(max_n_atom);
      }

      for(a = 0; a < n_res; a++) {
        res[a].n_acc = 0;
        res[a].n_don = 0;
      }
      hbc = &hbcRec;
      ObjectMoleculeInitHBondCriteria(G, hbc);

      /* use parameters which reflect the spirit of Kabsch and Sander
         ( i.e. long hydrogen-bonds/polar electrostatic interactions ) */

      hbc->maxAngle = 63.0F;
      hbc->maxDistAtMaxAngle = 3.2F;
      hbc->maxDistAtZero = 4.0F;
      hbc->power_a = 1.6F;
      hbc->power_b = 5.0F;
      hbc->cone_dangle = 0.0F;  /* 180 deg. */
      if(hbc->maxDistAtMaxAngle != 0.0F) {
        hbc->factor_a = 0.5F / (float) pow(hbc->maxAngle, hbc->power_a);
        hbc->factor_b = 0.5F / (float) pow(hbc->maxAngle, hbc->power_b);
      }

      cutoff = hbc->maxDistAtMaxAngle;
      if(cutoff < hbc->maxDistAtZero) {
        cutoff = hbc->maxDistAtZero;
      }

      n1 = 0;

      for(aa = 0; aa < I->NAtom; aa++) {        /* first, clear flags */
        I->Flag1[aa] = false;
        I->Flag2[aa] = false;
      }

      for(a = 0; a < n_res; a++) {
        if(res[a].present) {
          obj0 = res[a].obj;

          if(obj0) {
            /* map will contain the h-bond backbone nitrogens */

            aa = res[a].n;
            at = I->Table[aa].atom;
            I->Flag2[aa] = a;   /* so we can find the atom again... */

            if(state < obj0->NCSet)
              cs = obj0->CSet[state];
            else
              cs = NULL;
            if(cs) {
              if(CoordSetGetAtomVertex(cs, at,
                    I->Vertex + 3 * aa /* record coordinate */)) {
                I->Flag1[aa] = true;
                n1++;
              }
            }

            /* also copy O coordinates for usage below */

            aa = res[a].o;
            at = I->Table[aa].atom;

            if(state < obj0->NCSet)
              cs = obj0->CSet[state];
            else
              cs = NULL;
            if(cs) {
              CoordSetGetAtomVertex(cs, at,
                  I->Vertex + 3 * aa /* record coordinate */);
            }
          }
        }
      }

      if(n1) {
        short too_many_atoms = false;
        map = MapNewFlagged(G, -cutoff, I->Vertex, I->NAtom, NULL, I->Flag1);
        if(map) {
          MapSetupExpress(map);

          for(a0 = 0; a0 < n_res; a0++) {

            if(res[a0].obj) {

              /* now iterate through carbonyls */
              obj0 = res[a0].obj;
              as0 = res[a0].o;
              at0 = I->Table[as0].atom;

              v0 = I->Vertex + 3 * as0;
              if(MapExclLocus(map, v0, &h, &k, &l)) {
                i = *(MapEStart(map, h, k, l));
                if(i) {
                  int nat = 0;
                  as1 = map->EList[i++];
                  while(as1 >= 0) {
                    v1 = I->Vertex + 3 * as1;

                    if(within3f(v0, v1, cutoff)) {

                      obj1 = I->Obj[I->Table[as1].model];
                      at1 = I->Table[as1].atom;

                      if(obj0 == obj1) {        /* don't count hbonds between adjacent residues */
                        exclude = SelectorCheckNeighbors(G, 5, obj0, at0, at1,
                                                         zero, scratch);
                      } else {
                        exclude = false;
                      }

                      /*                      if(!exclude) {
                         printf("at1 %s %s vs at0 %s %s\n",
                         obj1->AtomInfo[at1].resi,
                         obj1->AtomInfo[at1].name,
                         obj0->AtomInfo[at0].resi,
                         obj0->AtomInfo[at0].name
                         );
                         }
                       */
                      if((!exclude) && ObjectMoleculeGetCheckHBond(NULL, NULL, obj1,    /* donor first */
                                                                   at1, state, obj0,    /* then acceptor */
                                                                   at0, state, hbc)) {

                        /*                        printf(" found hbond between acceptor resi %s and donor resi %s\n",
                           res[a0].obj->AtomInfo[at0].resi,
                           res[I->Flag2[as1]].obj->AtomInfo[I->Table[as1].atom].resi); */

                        a1 = I->Flag2[as1];     /* index in SS n_res space */

                        /* store acceptor link */

                        n1 = res[a0].n_acc;
                        if(n1 < (cSSMaxHBond - 1)) {
                          res[a0].acc[n1] = a1;
                          res[a0].n_acc = n1 + 1;
                        }

                        /* store donor link */

                        n1 = res[a1].n_don;
                        if(n1 < (cSSMaxHBond - 1)) {
                          res[a1].don[n1] = a0;
                          res[a1].n_don = n1 + 1;
                        }
                      }
                    }
                    as1 = map->EList[i++];
		    nat++;
                  }
                  if (nat > 1000){ // if map returns more than 1000 atoms within 4, should be a dss error
                    too_many_atoms = true;
                    break;
                  }
                }
              }
            }
          }
        }
        MapFree(map);
	if (too_many_atoms){
	  PRINTFB(G, FB_Selector, FB_Errors)
	    " %s: ERROR: Unreasonable number of neighbors for dss, cannot assign secondary structure.\n", __func__ ENDFB(G);
	}
      }
      FreeP(zero);
      FreeP(scratch);
    }

    {                           /* compute phi, psi's */

      SSResi *r;
      int a;

      float helix_psi_delta, helix_phi_delta;
      float strand_psi_delta, strand_phi_delta;

      float helix_psi_target = SettingGet_f(G, NULL, NULL, cSetting_ss_helix_psi_target);
      float helix_psi_include =
        SettingGet_f(G, NULL, NULL, cSetting_ss_helix_psi_include);
      float helix_psi_exclude =
        SettingGet_f(G, NULL, NULL, cSetting_ss_helix_psi_exclude);

      float helix_phi_target = SettingGet_f(G, NULL, NULL, cSetting_ss_helix_phi_target);
      float helix_phi_include =
        SettingGet_f(G, NULL, NULL, cSetting_ss_helix_phi_include);
      float helix_phi_exclude =
        SettingGet_f(G, NULL, NULL, cSetting_ss_helix_phi_exclude);

      float strand_psi_target =
        SettingGet_f(G, NULL, NULL, cSetting_ss_strand_psi_target);
      float strand_psi_include =
        SettingGet_f(G, NULL, NULL, cSetting_ss_strand_psi_include);
      float strand_psi_exclude =
        SettingGet_f(G, NULL, NULL, cSetting_ss_strand_psi_exclude);

      float strand_phi_target =
        SettingGet_f(G, NULL, NULL, cSetting_ss_strand_phi_target);
      float strand_phi_include =
        SettingGet_f(G, NULL, NULL, cSetting_ss_strand_phi_include);
      float strand_phi_exclude =
        SettingGet_f(G, NULL, NULL, cSetting_ss_strand_phi_exclude);

      for(a = 0; a < n_res; a++) {
        r = res + a;
        if(r->real && ((r - 1)->real)) {
          r->flags = 0;

          if(ObjectMoleculeGetPhiPsi
             (r->obj, I->Table[r->ca].atom, &r->phi, &r->psi, state)) {
            r->flags |= cSSGotPhiPsi;

            helix_psi_delta = (float) fabs(r->psi - helix_psi_target);
            strand_psi_delta = (float) fabs(r->psi - strand_psi_target);
            helix_phi_delta = (float) fabs(r->phi - helix_phi_target);
            strand_phi_delta = (float) fabs(r->phi - strand_phi_target);

            if(helix_psi_delta > 180.0F)
              helix_psi_delta = 360.0F - helix_psi_delta;
            if(strand_psi_delta > 180.0F)
              strand_psi_delta = 360.0F - strand_psi_delta;
            if(helix_phi_delta > 180.0F)
              helix_phi_delta = 360.0F - helix_phi_delta;
            if(strand_phi_delta > 180.0F)
              strand_phi_delta = 360.0F - strand_phi_delta;

            /* printf("helix %d strand %d\n",helix_delta,strand_delta); */

            if((helix_psi_delta > helix_psi_exclude) ||
               (helix_phi_delta > helix_phi_exclude)) {
              r->flags |= cSSPhiPsiNotHelix;
            } else if((helix_psi_delta < helix_psi_include) &&
                      (helix_phi_delta < helix_phi_include)) {
              r->flags |= cSSPhiPsiHelix;
            }

            if((strand_psi_delta > strand_psi_exclude) ||
               (strand_phi_delta > strand_phi_exclude)) {
              r->flags |= cSSPhiPsiNotStrand;
            } else if((strand_psi_delta < strand_psi_include) &&
                      (strand_phi_delta < strand_phi_include)) {
              r->flags |= cSSPhiPsiStrand;
            }
          }
        }
      }
    }

    /* by default, tentatively assign everything as loop */

    {
      int a;
      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        if(res[a].present)
          res[a].ss = 'L';
      }
    }

    {
      SSResi *r, *r2;
      int a, b, c;

      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        r = res + a;
        if(r->real) {

          /* look for tell-tale i+3,4,5 hydrogen bonds for helix  */

          /* is residue an acceptor for i+3,4,5 residue? */
          for(b = 0; b < r->n_acc; b++) {
            r->flags |=
              ((r->acc[b] == (a + 3)) ? cSSHelix3HBond : 0) |
              ((r->acc[b] == (a + 4)) ? cSSHelix4HBond : 0) |
              ((r->acc[b] == (a + 5)) ? cSSHelix5HBond : 0);

          }

          /* is residue a donor for i-3,4,5 residue */
          for(b = 0; b < r->n_don; b++) {
            r->flags |=
              ((r->don[b] == (a - 3)) ? cSSHelix3HBond : 0) |
              ((r->don[b] == (a - 4)) ? cSSHelix4HBond : 0) |
              ((r->don[b] == (a - 5)) ? cSSHelix5HBond : 0);

          }

          /*        if(r->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) {
             printf("HelixHB %s \n",
             r->obj->AtomInfo[I->Table[r->ca].atom].resi);
             }
           */

          /* look for double h-bonded antiparallel beta sheet pairs:
           * 
           *  \ /\ /
           *   N  C
           *   #  O
           *   O  #
           *   C  N
           *  / \/ \
           *
           */

          for(b = 0; b < r->n_acc; b++) {       /* iterate through acceptors */
            r2 = (res + r->acc[b]);
            if(r2->real) {
              for(c = 0; c < r2->n_acc; c++) {
                if(r2->acc[c] == a) {   /* found a pair */
                  r->flags |= cSSAntiStrandDoubleHB;
                  r2->flags |= cSSAntiStrandDoubleHB;

                  /*                printf("anti double %s to %s\n",
                     r->obj->AtomInfo[I->Table[r->ca].atom].resi,
                     r2->obj->AtomInfo[I->Table[r2->ca].atom].resi); */

                }
              }
            }
          }

          /* look for antiparallel beta buldges
           * 
           *     CCNC
           *  \ / O  \ /
           *   N      C
           *   #      O
           *    O    #
           *     C  N
           *    / \/ \
           *
           */

          for(b = 0; b < r->n_acc; b++) {       /* iterate through acceptors */
            r2 = (res + r->acc[b]) + 1; /* go forward 1 */
            if(r2->real) {
              for(c = 0; c < r2->n_acc; c++) {
                if(r2->acc[c] == a) {   /* found a buldge */
                  r->flags |= cSSAntiStrandDoubleHB;
                  r2->flags |= cSSAntiStrandBuldgeHB;
                  (r2 - 1)->flags |= cSSAntiStrandBuldgeHB;

                  /*                printf("anti BULDGE %s to %s %s\n",
                     r->obj->AtomInfo[I->Table[r->ca].atom].resi,
                     r2->obj->AtomInfo[I->Table[r2->ca].atom].resi,
                     r2->obj->AtomInfo[I->Table[(r2-1)->ca].atom].resi); */

                }
              }
            }
          }

          /* look for antiparallel beta sheet ladders (single or double)
           *
           *        O
           *     N  C
           *  \ / \/ \ /
           *   C      N
           *   O      #
           *   #      O
           *   N      C
           *  / \ /\ / \
           *     C  N
           *     O
           */

          if((r + 1)->real && (r + 2)->real) {

            for(b = 0; b < r->n_acc; b++) {     /* iterate through acceptors */
              r2 = (res + r->acc[b]) - 2;       /* go back 2 */
              if(r2->real) {

                for(c = 0; c < r2->n_acc; c++) {

                  if(r2->acc[c] == a + 2) {     /* found a ladder */

                    (r)->flags |= cSSAntiStrandSingleHB;
                    (r + 1)->flags |= cSSAntiStrandSkip;
                    (r + 2)->flags |= cSSAntiStrandSingleHB;

                    (r2)->flags |= cSSAntiStrandSingleHB;
                    (r2 + 1)->flags |= cSSAntiStrandSkip;
                    (r2 + 2)->flags |= cSSAntiStrandSingleHB;

                    /*                  printf("anti ladder %s %s to %s %s\n",
                       r->obj->AtomInfo[I->Table[r->ca].atom].resi,
                       r->obj->AtomInfo[I->Table[(r+2)->ca].atom].resi,
                       r2->obj->AtomInfo[I->Table[r2->ca].atom].resi,
                       r2->obj->AtomInfo[I->Table[(r2+2)->ca].atom].resi); */
                  }
                }
              }
            }
          }

          /* look for parallel beta sheet ladders 
           *

           *    \ /\ /
           *     C  N
           *    O    #
           *   #      O
           *   N      C
           *  / \ /\ / \
           *     C  N
           *     O
           */

          if((r + 1)->real && (r + 2)->real) {

            for(b = 0; b < r->n_acc; b++) {     /* iterate through acceptors */
              r2 = (res + r->acc[b]);
              if(r2->real) {

                for(c = 0; c < r2->n_acc; c++) {

                  if(r2->acc[c] == a + 2) {     /* found a ladder */

                    (r)->flags |= cSSParaStrandSingleHB;
                    (r + 1)->flags |= cSSParaStrandSkip;
                    (r + 2)->flags |= cSSParaStrandSingleHB;

                    (r2)->flags |= cSSParaStrandDoubleHB;

                    /*                                    printf("parallel ladder %s %s to %s \n",
                       r->obj->AtomInfo[I->Table[r->ca].atom].resi,
                       r->obj->AtomInfo[I->Table[(r+2)->ca].atom].resi,
                       r2->obj->AtomInfo[I->Table[r2->ca].atom].resi); */
                  }
                }
              }
            }
          }
        }
      }
    }

    {
      int a;
      SSResi *r;
      /* convert flags to assignments */

      /* HELICES FIRST */

      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        r = res + a;

        if(r->real) {
          /* clean internal helical residues are easy to find using H-bonds */

          if(((r - 1)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r + 1)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond))) {
            if(!(r->flags & (cSSPhiPsiNotHelix))) {
              r->ss = 'H';
            }
          }

          /*
             if(((r-1)->flags & (cSSHelix3HBond )) &&
             ((r  )->flags & (cSSHelix3HBond )) &&
             ((r+1)->flags & (cSSHelix3HBond ))) {
             if(!(r->flags & (cSSPhiPsiNotHelix))) {
             r->ss = 'H';
             }
             }

             if(((r-1)->flags & (cSSHelix4HBond)) &&
             ((r  )->flags & (cSSHelix4HBond)) &&
             ((r+1)->flags & (cSSHelix4HBond))) {
             if(!(r->flags & (cSSPhiPsiNotHelix))) {
             r->ss = 'H';
             }
             }

             if(((r-1)->flags & (cSSHelix5HBond)) &&
             ((r  )->flags & (cSSHelix5HBond)) &&
             ((r+1)->flags & (cSSHelix5HBond))) {
             if(!(r->flags & (cSSPhiPsiNotHelix))) {
             r->ss = 'H';
             }
             }
           */

        }
      }

      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        r = res + a;

        if(r->real) {

          /* occasionally they'll be one whacked out residue missing h-bonds... 
             in an otherwise good segment */

          if(((r - 2)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r - 1)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r - 1)->flags & (cSSPhiPsiHelix)) &&
             ((r)->flags & (cSSPhiPsiHelix)) &&
             ((r + 1)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r + 1)->flags & (cSSPhiPsiHelix)) &&
             ((r + 2)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond))
            ) {
            r->ss = 'h';
          }
        }
      }

      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        r = res + a;
        if(r->real) {
          if(r->ss == 'h') {
            r->flags |= (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond);
            r->ss = 'H';
          }
        }
      }

      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        r = res + a;

        if(r->real) {

          /* deciding where the helix ends is trickier -- here we use helix geometry */

          if(((r)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r)->flags & (cSSPhiPsiHelix)) &&
             ((r + 1)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r + 1)->flags & (cSSPhiPsiHelix)) &&
             ((r + 2)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r + 2)->flags & (cSSPhiPsiHelix)) && ((r + 1)->ss == 'H')
            ) {
            r->ss = 'H';
          }

          if(((r)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r)->flags & (cSSPhiPsiHelix)) &&
             ((r - 1)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r - 1)->flags & (cSSPhiPsiHelix)) &&
             ((r - 2)->flags & (cSSHelix3HBond | cSSHelix4HBond | cSSHelix5HBond)) &&
             ((r - 2)->flags & (cSSPhiPsiHelix)) && ((r - 1)->ss == 'H')
            ) {
            r->ss = 'H';
          }

        }
      }

      /* THEN SHEETS/STRANDS */

      for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
        r = res + a;
        if(r->real) {

          /* Antiparallel Sheets */

          if(((r)->flags & (cSSAntiStrandDoubleHB)) &&
             (!((r->flags & (cSSPhiPsiNotStrand))))) {
            (r)->ss = 'S';
          }

          if(((r)->flags & (cSSAntiStrandBuldgeHB)) &&  /* no strand geometry filtering for buldges.. */
             ((r + 1)->flags & (cSSAntiStrandBuldgeHB))) {
            (r)->ss = 'S';
            (r + 1)->ss = 'S';
          }

          if(((r - 1)->flags & (cSSAntiStrandDoubleHB)) &&
             ((r)->flags & (cSSAntiStrandSkip)) &&
             (!(((r)->flags & (cSSPhiPsiNotStrand)))) &&
             ((r + 1)->flags & (cSSAntiStrandSingleHB | cSSAntiStrandDoubleHB))) {

            (r)->ss = 'S';
          }

          if(((r - 1)->flags & (cSSAntiStrandSingleHB | cSSAntiStrandDoubleHB)) &&
             ((r)->flags & (cSSAntiStrandSkip)) &&
             (!(((r)->flags & (cSSPhiPsiNotStrand)))) &&
             ((r + 1)->flags & (cSSAntiStrandDoubleHB))) {
            (r)->ss = 'S';
          }

          /* include open "ladders" if PHIPSI geometry supports assignment */

          if(((r - 1)->flags & (cSSAntiStrandSingleHB | cSSAntiStrandDoubleHB)) &&
             ((r - 1)->flags & (cSSPhiPsiStrand)) &&
             (!(((r - 1)->flags & (cSSPhiPsiNotStrand)))) &&
             ((r)->flags & (cSSPhiPsiStrand)) &&
             (!(((r - 1)->flags & (cSSPhiPsiNotStrand)))) &&
             ((r + 1)->flags & (cSSAntiStrandSingleHB | cSSAntiStrandDoubleHB)) &&
             ((r + 1)->flags & (cSSPhiPsiStrand))) {

            (r - 1)->ss = 'S';
            (r)->ss = 'S';
            (r + 1)->ss = 'S';
          }

          /* Parallel Sheets */

          if(((r)->flags & (cSSParaStrandDoubleHB)) &&
             (!(((r)->flags & (cSSPhiPsiNotStrand))))) {
            (r)->ss = 'S';
          }

          if(((r - 1)->flags & (cSSParaStrandDoubleHB)) &&
             ((r)->flags & (cSSParaStrandSkip)) &&
             (!(((r)->flags & (cSSPhiPsiNotStrand)))) &&
             ((r + 1)->flags & (cSSParaStrandSingleHB | cSSParaStrandDoubleHB))) {

            (r)->ss = 'S';
          }

          if(((r - 1)->flags & (cSSParaStrandSingleHB | cSSParaStrandDoubleHB)) &&
             ((r)->flags & (cSSParaStrandSkip)) &&
             (!(((r)->flags & (cSSPhiPsiNotStrand)))) &&
             ((r + 1)->flags & (cSSParaStrandDoubleHB))) {
            (r)->ss = 'S';
          }

          /* include open "ladders" if PHIPSI geometry supports assignment */

          if(((r - 1)->flags & (cSSParaStrandSingleHB | cSSParaStrandDoubleHB)) &&
             ((r - 1)->flags & (cSSPhiPsiStrand)) &&
             ((r)->flags & (cSSParaStrandSkip)) &&
             ((r)->flags & (cSSPhiPsiStrand)) &&
             ((r + 1)->flags & (cSSParaStrandSingleHB | cSSParaStrandDoubleHB)) &&
             ((r + 1)->flags & (cSSPhiPsiStrand))) {

            (r - 1)->ss = 'S';
            (r)->ss = 'S';
            (r + 1)->ss = 'S';

          }
        }
      }
    }

    {
      int a, b;
      SSResi *r, *r2;
      int repeat = true;
      int found;

      while(repeat) {
        repeat = false;

        for(a = cSSBreakSize; a < (n_res - cSSBreakSize); a++) {
          r = res + a;
          if(r->real) {

            /* make sure we don't have any 2-residue segments */

            if((r->ss == 'S') && ((r + 1)->ss == 'S') &&
               (((r - 1)->ss != 'S') && ((r + 2)->ss != 'S'))) {
              r->ss = 'L';
              (r + 1)->ss = 'L';
              repeat = true;
            }
            if((r->ss == 'H') && ((r + 1)->ss == 'H') &&
               (((r - 1)->ss != 'H') && ((r + 2)->ss != 'H'))) {
              r->ss = 'L';
              (r + 1)->ss = 'L';
              repeat = true;
            }

            /* make sure we don't have any 1-residue segments */

            if((r->ss == 'S') && (((r - 1)->ss != 'S') && ((r + 1)->ss != 'S'))) {
              r->ss = 'L';
              repeat = true;
            }
            if((r->ss == 'H') && (((r - 1)->ss != 'H') && ((r + 1)->ss != 'H'))) {
              r->ss = 'L';
              repeat = true;
            }

            /* double-check to make sure every terminal strand residue 
               that should have a partner has one */

            if((r->ss == 'S') && (((r - 1)->ss != 'S') || ((r + 1)->ss != 'S'))) {

              found = false;

              for(b = 0; b < r->n_acc; b++) {
                r2 = res + r->acc[b];
                if(r2->ss == r->ss) {
                  found = true;
                  break;
                }
              }

              if(!found) {
                for(b = 0; b < r->n_don; b++) {
                  r2 = res + r->don[b];
                  if(r2->ss == r->ss) {
                    found = true;
                    break;
                  }
                }
              }

              if(!found) {     
                /* allow these strand "skip" residues to persist if a neighbor has hydrogen bonds */
                if(r->flags & (cSSAntiStrandSkip | cSSParaStrandSkip)) {

                  if((r + 1)->ss == r->ss)
                    for(b = 0; b < (r + 1)->n_acc; b++) {
                      r2 = res + (r + 1)->acc[b];
                      if(r2->ss == r->ss) {
                        found = true;
                        break;
                      }
                    }

                  if(!found) {
                    if((r - 1)->ss == r->ss) {
                      for(b = 0; b < (r - 1)->n_don; b++) {
                        r2 = res + (r - 1)->don[b];
                        if(r2->ss == r->ss) {
                          found = true;
                          break;
                        }
                      }
                    }
                  }
                }
              }

              if(!found) {
                r->ss = 'L';
                repeat = true;
              }
            }
          }
        }
      }
    }

    {
      int a;
      for(a = 0; a < n_res; a++) {      /* now apply consensus or union behavior, if appropriate */
        if(res[a].present) {
          if(res[a].ss_save) {
            if(res[a].ss != res[a].ss_save) {
              if(consensus) {
                res[a].ss = res[a].ss_save = 'L';
              } else if(res[a].ss == 'L')
                res[a].ss = res[a].ss_save;
            }
          }
          res[a].ss_save = res[a].ss;
        }
      }
    }

    {
      int a, aa;
      ObjectMolecule *obj = NULL, *last_obj = NULL;
      AtomInfoType *ai;
      int changed_flag = false;

      for(a = 0; a < n_res; a++) {
        if(res[a].present && (!res[a].preserve)) {

          aa = res[a].ca;
          obj = I->Obj[I->Table[aa].model];

          if(obj != last_obj) {
            if(changed_flag && last_obj) {
              last_obj->invalidate(cRepCartoon, cRepInvRep, -1);
              SceneChanged(G);
              changed_flag = false;
            }
            last_obj = obj;
          }
          ai = obj->AtomInfo + I->Table[aa].atom;

          if(SelectorIsMember(G, ai->selEntry, target)) {
            ai->ssType[0] = res[a].ss;
            ai->cartoon = 0;    /* switch back to auto */
            ai->ssType[1] = 0;
            changed_flag = true;
          }
        }
      }

      if(changed_flag && last_obj) {
        last_obj->invalidate(cRepCartoon, cRepInvRep, -1);
        SceneChanged(G);
        changed_flag = false;
      }
    }
    if(first_last_only && (state == state_start))
      state = state_stop - 2;
  }

  VLAFreeP(res);
  return 1;
}

PyObject *SelectorColorectionGet(PyMOLGlobals * G, const char *prefix)
{
#ifdef _PYMOL_NOPY
  return NULL;
#else
  CSelector *I = G->Selector;
  PyObject *result = NULL;
  int n_used = 0;
  ColorectionRec *used = NULL, tmp;
  ov_size a, b, n;
  int sele;
  int found;
  int m;
  int color;
  AtomInfoType *ai;
  used = VLAlloc(ColorectionRec, 1000);

  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    ai = I->Obj[I->Table[a].model]->AtomInfo + I->Table[a].atom;
    color = ai->color;
    found = false;
    for(b = 0; b < n_used; b++) {
      if(used[b].color == color) {
        tmp = used[0];          /* optimize to minimize N^2 effects */
        used[0] = used[b];
        used[b] = tmp;
        found = true;
        break;
      }
    }
    if(!found) {
      VLACheck(used, ColorectionRec, n_used);
      used[n_used] = used[0];
      used[0].color = color;
      n_used++;
    }
  }
  for(a = 0; a < n_used; a++) {
    /* create selections */

    n = I->NActive;
    VLACheck(I->Name, SelectorWordType, n + 1);
    VLACheck(I->Info, SelectionInfoRec, n + 1);
    sele = I->NSelection++;
    used[a].sele = sele;
    sprintf(I->Name[n], cColorectionFormat, prefix, used[a].color);
    I->Name[n + 1][0] = 0;
    SelectorAddName(G, n);
    SelectionInfoInit(I->Info + n);
    I->Info[n].ID = sele;
    I->NActive++;
  }

  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    ai = I->Obj[I->Table[a].model]->AtomInfo + I->Table[a].atom;
    color = ai->color;
    for(b = 0; b < n_used; b++) {
      if(used[b].color == color) {
        tmp = used[0];          /* optimize to minimize N^2 effects */
        used[0] = used[b];
        used[b] = tmp;

        /* add selection onto atom */
        if(I->FreeMember > 0) {
          m = I->FreeMember;
          I->FreeMember = I->Member[m].next;
        } else {
          I->NMember++;
          m = I->NMember;
          VLACheck(I->Member, MemberType, m);
        }
        I->Member[m].selection = used[0].sele;
        I->Member[m].tag = 1;
        I->Member[m].next = ai->selEntry;
        ai->selEntry = m;
        break;
      }
    }
  }

  VLASize(used, ColorectionRec, n_used * 2);
  result = PConvIntVLAToPyList((int *) used);
  VLAFreeP(used);
  return (result);
#endif
}

int SelectorColorectionApply(PyMOLGlobals * G, PyObject * list, const char *prefix)
{
#ifdef _PYMOL_NOPY
  return 0;
#else

  CSelector *I = G->Selector;
  int ok = true;
  ColorectionRec *used = NULL;
  ov_size n_used = 0;
  int a, b;
  AtomInfoType *ai;
  ObjectMolecule *obj, *last = NULL;
  SelectorWordType name;

  if(ok)
    ok = (list != NULL);
  if(ok)
    ok = PyList_Check(list);
  if(ok)
    n_used = PyList_Size(list) / 2;
  if(ok)
    ok = ((used = VLAlloc(ColorectionRec, n_used)) != NULL);
  if(ok)
    ok = PConvPyListToIntArrayInPlace(list, (int *) used, n_used * 2);
  if(ok) {

    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);

    for(b = 0; b < n_used; b++) {       /* update selection indices */
      sprintf(name, cColorectionFormat, prefix, used[b].color);
      used[b].sele = SelectorIndexByName(G, name);
    }

    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      obj = I->Obj[I->Table[a].model];
      ai = obj->AtomInfo + I->Table[a].atom;

      for(b = 0; b < n_used; b++) {
        if(SelectorIsMember(G, ai->selEntry, used[b].sele)) {
          ai->color = used[b].color;
          if(obj != last) {
            obj->invalidate(cRepAll, cRepInvColor, -1);
            last = obj;
          }
          break;
        }
      }
    }
  }
  VLAFreeP(used);
  return (ok);
#endif
}

int SelectorColorectionSetName(PyMOLGlobals * G, PyObject * list, const char *prefix,
                               char *new_prefix)
{
#ifdef _PYMOL_NOPY
  return 0;
#else

  int ok = true;
  ColorectionRec *used = NULL;
  ov_size n_used = 0;
  ov_size b;
  SelectorWordType name;
  SelectorWordType new_name;

  if(ok)
    ok = (list != NULL);
  if(ok)
    ok = PyList_Check(list);
  if(ok)
    n_used = PyList_Size(list) / 2;
  if(ok)
    ok = ((used = VLAlloc(ColorectionRec, n_used)) != NULL);
  if(ok)
    ok = PConvPyListToIntArrayInPlace(list, (int *) used, n_used * 2);
  if(ok) {
    for(b = 0; b < n_used; b++) {       /* update selection indices */
      sprintf(name, cColorectionFormat, prefix, used[b].color);
      sprintf(new_name, cColorectionFormat, new_prefix, used[b].color);
      SelectorSetName(G, new_name, name);
    }
  }
  VLAFreeP(used);
  return (ok);
#endif

}

int SelectorColorectionFree(PyMOLGlobals * G, PyObject * list, const char *prefix)
{
#ifdef _PYMOL_NOPY
  return 0;
#else

  int ok = true;
  ColorectionRec *used = NULL;
  ov_size n_used = 0;
  ov_size b;
  SelectorWordType name;

  if(ok)
    ok = (list != NULL);
  if(ok)
    ok = PyList_Check(list);
  if(ok)
    n_used = PyList_Size(list) / 2;
  if(ok)
    ok = ((used = VLAlloc(ColorectionRec, n_used)) != NULL);
  if(ok)
    ok = PConvPyListToIntArrayInPlace(list, (int *) used, n_used * 2);
  if(ok) {

    for(b = 0; b < n_used; b++) {       /* update selection indices */
      sprintf(name, cColorectionFormat, prefix, used[b].color);
      used[b].sele = SelectorIndexByName(G, name);
    }

    for(b = 0; b < n_used; b++) {
      SelectorDeleteIndex(G, used[b].sele);
    }
  }
  VLAFreeP(used);
  return (ok);
#endif

}

PyObject *SelectorSecretsAsPyList(PyMOLGlobals * G)
{
  CSelector *I = G->Selector;
  int n_secret;
  int a;
  PyObject *result, *list;

  n_secret = 0;
  for(a = 0; a < I->NActive; a++) {
    if((I->Name[a][0] == '_') && (I->Name[a][1] == '!'))
      n_secret++;
  }
  result = PyList_New(n_secret);
  n_secret = 0;
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  for(a = 0; a < I->NActive; a++) {
    if((I->Name[a][0] == '_') && (I->Name[a][1] == '!')) {
      list = PyList_New(2);
      PyList_SetItem(list, 0, PyString_FromString(I->Name[a]));
      PyList_SetItem(list, 1, SelectorAsPyList(G, I->Info[a].ID));
      PyList_SetItem(result, n_secret, list);
      n_secret++;
    }
  }
  return (result);
}

int SelectorSecretsFromPyList(PyMOLGlobals * G, PyObject * list)
{
  int ok = true;
  ov_size n_secret = 0;
  ov_size a;
  PyObject *entry = NULL;
  SelectorWordType name;
  ov_size ll = 0;
  if(ok)
    ok = (list != NULL);
  if(ok)
    ok = PyList_Check(list);
  if(ok)
    n_secret = PyList_Size(list);
  if(ok) {
    for(a = 0; a < n_secret; a++) {
      if(ok)
        entry = PyList_GetItem(list, a);
      if(ok)
        ok = (entry != NULL);
      if(ok)
        ok = PyList_Check(entry);
      if(ok)
        ll = PyList_Size(entry);
      if(ok & (ll > 1)) {
        if(ok)
          ok = PConvPyStrToStr(PyList_GetItem(entry, 0), name, sizeof(SelectorWordType));
        if(ok)
          ok = SelectorFromPyList(G, name, PyList_GetItem(entry, 1));
      }
      if(!ok)
        break;
    }
  }
  return (ok);
}

typedef struct {
  int atom;
  int tag;
} SelAtomTag;

PyObject *SelectorAsPyList(PyMOLGlobals * G, int sele1)
{                               /* assumes SelectorUpdateTable has been called */
  CSelector *I = G->Selector;
  int a, b;
  int at;
  int s;
  SelAtomTag **vla_list = NULL;
  int n_obj = 0;
  int n_idx = 0;
  int cur = -1;
  ObjectMolecule **obj_list = NULL;
  ObjectMolecule *obj, *cur_obj = NULL;
  PyObject *result = NULL;
  PyObject *obj_pyobj;
  PyObject *idx_pyobj;
  PyObject *tag_pyobj;

  vla_list = VLACalloc(SelAtomTag *, 10);
  obj_list = VLAlloc(ObjectMolecule *, 10);

  n_idx = 0;
  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    int tag;
    at = I->Table[a].atom;
    obj = I->Obj[I->Table[a].model];
    s = obj->AtomInfo[at].selEntry;
    if((tag = SelectorIsMember(G, s, sele1))) {
      if(cur_obj != obj) {
        if(n_idx) {
          VLASize(vla_list[cur], SelAtomTag, n_idx);
        }
        cur++;
        VLACheck(vla_list, SelAtomTag *, n_obj);
        vla_list[cur] = VLAlloc(SelAtomTag, 1000);
        VLACheck(obj_list, ObjectMolecule *, n_obj);
        obj_list[cur] = obj;
        cur_obj = obj;
        n_obj++;
        n_idx = 0;
      }
      VLACheck(vla_list[cur], SelAtomTag, n_idx);
      vla_list[cur][n_idx].atom = at;
      vla_list[cur][n_idx].tag = tag;
      n_idx++;
    }
  }
  if(cur_obj) {
    if(n_idx) {
      VLASize(vla_list[cur], SelAtomTag, n_idx);
    }
  }
  if(n_obj) {
    result = PyList_New(n_obj);
    for(a = 0; a < n_obj; a++) {
      obj_pyobj = PyList_New(3);
      n_idx = VLAGetSize(vla_list[a]);
      idx_pyobj = PyList_New(n_idx);
      tag_pyobj = PyList_New(n_idx);
      for(b = 0; b < n_idx; b++) {
        PyList_SetItem(idx_pyobj, b, PyInt_FromLong(vla_list[a][b].atom));
        PyList_SetItem(tag_pyobj, b, PyInt_FromLong(vla_list[a][b].tag));
      }
      VLAFreeP(vla_list[a]);
      PyList_SetItem(obj_pyobj, 0, PyString_FromString(obj_list[a]->Name));
      PyList_SetItem(obj_pyobj, 1, idx_pyobj);
      PyList_SetItem(obj_pyobj, 2, tag_pyobj);
      PyList_SetItem(result, a, obj_pyobj);
    }
  } else {
    result = PyList_New(0);
  }
  VLAFreeP(vla_list);
  VLAFreeP(obj_list);
  return (result);
}

int SelectorFromPyList(PyMOLGlobals * G, const char *name, PyObject * list)
{
  int ok = true;
  CSelector *I = G->Selector;
  ov_size a, b;
  ov_diff n;
  int m, sele;
  ov_size ll;
  PyObject *obj_list = NULL;
  PyObject *idx_list = NULL, *tag_list;
  ov_size n_obj = 0, n_idx = 0;
  int idx, tag;
  const char *oname;
  ObjectMolecule *obj;
  int singleAtomFlag = true;
  int singleObjectFlag = true;
  ObjectMolecule *singleObject = NULL;
  int singleAtom = -1;

  AtomInfoType *ai;
  if(ok)
    ok = PyList_Check(list);
  if(ok)
    n_obj = PyList_Size(list);

  /* get rid of existing selection */
  SelectorDelete(G, name);

  n = I->NActive;
  VLACheck(I->Name, SelectorWordType, n + 1);
  VLACheck(I->Info, SelectionInfoRec, n + 1);
  strcpy(I->Name[n], name);
  I->Name[n + 1][0] = 0;
  SelectorAddName(G, n);
  sele = I->NSelection++;
  SelectionInfoInit(I->Info + n);
  I->Info[n].ID = sele;
  I->NActive++;
  if(ok) {
    for(a = 0; a < n_obj; a++) {
      ll = 0;
      if(ok)
        obj_list = PyList_GetItem(list, a);
      if(ok)
        ok = PyList_Check(obj_list);
      if(ok)
        ll = PyList_Size(obj_list);
      if(ok)
        ok = PConvPyStrToStrPtr(PyList_GetItem(obj_list, 0), &oname);
      obj = NULL;
      if(ok)
        obj = ExecutiveFindObjectMoleculeByName(G, oname);
      if(ok && obj) {
        if(ok)
          idx_list = PyList_GetItem(obj_list, 1);
        if(ll > 2)
          tag_list = PyList_GetItem(obj_list, 2);
        else
          tag_list = NULL;
        if(ok)
          ok = PyList_Check(idx_list);
        if(ok)
          n_idx = PyList_Size(idx_list);
        for(b = 0; b < n_idx; b++) {
          if(ok)
            ok = PConvPyIntToInt(PyList_GetItem(idx_list, b), &idx);
          if(tag_list)
            PConvPyIntToInt(PyList_GetItem(tag_list, b), &tag);
          else
            tag = 1;
          if(ok && (idx < obj->NAtom)) {
            ai = obj->AtomInfo + idx;
            if(I->FreeMember > 0) {
              m = I->FreeMember;
              I->FreeMember = I->Member[m].next;
            } else {
              I->NMember++;
              m = I->NMember;
              VLACheck(I->Member, MemberType, m);
            }
            I->Member[m].selection = sele;
            I->Member[m].tag = tag;
            I->Member[m].next = ai->selEntry;
            ai->selEntry = m;

            /* take note of selections which are one atom/one object */
            if(singleObjectFlag) {
              if(singleObject) {
                if(obj != singleObject) {
                  singleObjectFlag = false;
                }
              } else {
                singleObject = obj;
              }
            }

            if(singleAtomFlag) {
              if(singleAtom >= 0) {
                if(idx != singleAtom) {
                  singleAtomFlag = false;
                }
              } else {
                singleAtom = idx;
              }
            }
          }
        }
      }
    }
    {                           /* make note of single atom/object selections */
      SelectionInfoRec *info = I->Info + (I->NActive - 1);
      if(singleObjectFlag && singleObject) {
        info->justOneObjectFlag = true;
        info->theOneObject = singleObject;
        if(singleAtomFlag && (singleAtom >= 0)) {
          info->justOneAtomFlag = true;
          info->theOneAtom = singleAtom;
        }
      }
    }
  }
  return (ok);
}

int SelectorVdwFit(PyMOLGlobals * G, int sele1, int state1, int sele2, int state2,
                   float buffer, int quiet)
{
  int ok = true;
  CSelector *I = G->Selector;
  int *vla = NULL;
  int c;
  float sumVDW = 0.0, dist;
  int a1, a2;
  AtomInfoType *ai1, *ai2;
  int at1, at2;
  CoordSet *cs1, *cs2;
  ObjectMolecule *obj1, *obj2;
  int idx1, idx2;
  float *adj = NULL;
  int a;

  if(state1 < 0)
    state1 = 0;
  if(state2 < 0)
    state2 = 0;

  if(state1 != state2) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, state1, -1);
  }

  c =
    SelectorGetInterstateVLA(G, sele1, state1, sele2, state2, 2 * MAX_VDW + buffer, &vla);
  if(c) {
    adj = pymol::calloc<float>(2 * c);
    for(a = 0; a < c; a++) {
      a1 = vla[a * 2];
      a2 = vla[a * 2 + 1];

      at1 = I->Table[a1].atom;
      at2 = I->Table[a2].atom;

      obj1 = I->Obj[I->Table[a1].model];
      obj2 = I->Obj[I->Table[a2].model];

      if((state1 < obj1->NCSet) && (state2 < obj2->NCSet)) {
        cs1 = obj1->CSet[state1];
        cs2 = obj2->CSet[state2];
        if(cs1 && cs2) {        /* should always be true */

          ai1 = obj1->AtomInfo + at1;
          ai2 = obj2->AtomInfo + at2;

          idx1 = cs1->AtmToIdx[at1];    /* these are also pre-validated */
          idx2 = cs2->AtmToIdx[at2];

          sumVDW = ai1->vdw + ai2->vdw;
          dist = (float) diff3f(cs1->Coord + 3 * idx1, cs2->Coord + 3 * idx2);

          if(dist < (sumVDW + buffer)) {
            float shift = (dist - (sumVDW + buffer)) / 2.0F;
            adj[2 * a] = ai1->vdw + shift;
            adj[2 * a + 1] = ai2->vdw + shift;
          } else {
            adj[2 * a] = ai1->vdw;
            adj[2 * a + 1] = ai2->vdw;
          }

        }
      }
    }

    for(a = 0; a < c; a++) {
      a1 = vla[a * 2];
      a2 = vla[a * 2 + 1];

      at1 = I->Table[a1].atom;
      at2 = I->Table[a2].atom;

      obj1 = I->Obj[I->Table[a1].model];
      obj2 = I->Obj[I->Table[a2].model];

      if((state1 < obj1->NCSet) && (state2 < obj2->NCSet)) {
        cs1 = obj1->CSet[state1];
        cs2 = obj2->CSet[state2];
        if(cs1 && cs2) {        /* should always be true */

          ai1 = obj1->AtomInfo + at1;
          ai2 = obj2->AtomInfo + at2;

          if(adj[2 * a] < ai1->vdw) {
            ai1->vdw = adj[2 * a];
          }

          if(adj[2 * a + 1] < ai2->vdw) {
            ai2->vdw = adj[2 * a + 1];
          }

        }
      }
    }
  }

  VLAFreeP(vla);
  FreeP(adj);
  return ok;
}


/*========================================================================*/

int SelectorGetPairIndices(PyMOLGlobals * G, int sele1, int state1, int sele2, int state2,
                           int mode, float cutoff, float h_angle,
                           int **indexVLA, ObjectMolecule *** objVLA)
{
  CSelector *I = G->Selector;
  int *vla = NULL;
  int c;
  float dist;
  int a1, a2;
  int at1, at2;
  CoordSet *cs1, *cs2;
  ObjectMolecule *obj1, *obj2;
  int idx1, idx2;
  int a;
  int dist_cnt = 0;
  float dir[3];
  float v1[3], v2[3];
  int flag;
  float angle_cutoff = 0.0;

  if(mode == 1) {
    angle_cutoff = (float) cos(PI * h_angle / 180.0);
  }

  if(state1 < 0)
    state1 = 0;
  if(state2 < 0)
    state2 = 0;

  if(state1 != state2) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, state1, -1);
  }
  if(cutoff < 0)
    cutoff = 1000.0;
  c = SelectorGetInterstateVLA(G, sele1, state1, sele2, state2, cutoff, &vla);
  (*indexVLA) = VLAlloc(int, 1000);
  (*objVLA) = VLAlloc(ObjectMolecule *, 1000);

  for(a = 0; a < c; a++) {
    a1 = vla[a * 2];
    a2 = vla[a * 2 + 1];

    if(a1 != a2) {
      at1 = I->Table[a1].atom;
      at2 = I->Table[a2].atom;

      obj1 = I->Obj[I->Table[a1].model];
      obj2 = I->Obj[I->Table[a2].model];

      if(state1 < obj1->NCSet && state2 < obj2->NCSet) {
        cs1 = obj1->CSet[state1];
        cs2 = obj2->CSet[state2];
        if(cs1 && cs2) {
          idx1 = cs1->atmToIdx(at1);
          idx2 = cs2->atmToIdx(at2);

          if((idx1 >= 0) && (idx2 >= 0)) {
            subtract3f(cs1->Coord + 3 * idx1, cs2->Coord + 3 * idx2, dir);
            dist = (float) length3f(dir);
            if(dist > R_SMALL4) {
              float dist_1 = 1.0F / dist;
              scale3f(dir, dist_1, dir);
            }
            if(dist < cutoff) {
              if(mode == 1) {   /* coarse hydrogen bonding assessment */
                flag = false;
                if(ObjectMoleculeGetAvgHBondVector(obj1, at1, state1, v1, NULL) > 0.3)
                  if(dot_product3f(v1, dir) < -angle_cutoff)
                    flag = true;
                if(ObjectMoleculeGetAvgHBondVector(obj2, at2, state2, v2, NULL) > 0.3)
                  if(dot_product3f(v2, dir) > angle_cutoff)
                    flag = true;
              } else
                flag = true;

              if(flag) {
                VLACheck((*objVLA), ObjectMolecule *, dist_cnt + 1);
                VLACheck((*indexVLA), int, dist_cnt + 1);
                (*objVLA)[dist_cnt] = obj1;
                (*indexVLA)[dist_cnt] = at1;
                dist_cnt++;
                (*objVLA)[dist_cnt] = obj2;
                (*indexVLA)[dist_cnt] = at2;
                dist_cnt++;
              }
            }
          }
        }
      }
    }
  }

  VLASize((*objVLA), ObjectMolecule *, dist_cnt);
  VLASize((*indexVLA), int, dist_cnt);
  VLAFreeP(vla);
  dist_cnt = dist_cnt / 2;
  return (dist_cnt);
}


/*========================================================================*/
int SelectorCreateAlignments(PyMOLGlobals * G,
                             int *pair, int sele1, int *vla1, int sele2,
                             int *vla2, const char *name1, const char *name2,
                             int identical, int atomic_input)
{
  CSelector *I = G->Selector;
  int *flag1 = NULL, *flag2 = NULL;
  int *p;
  int i, np;
  int cnt;
  int mod1, mod2;               /* model indexes */
  int at1, at2, at1a, at2a;     /* atoms indexes */
  int vi1, vi2;                 /* vla indexes */
  int index1, index2;           /* indices in the selection array */
  AtomInfoType *ai1, *ai2, *ai1a, *ai2a;        /* atom information pointers */
  ObjectMolecule *obj1, *obj2;
  int cmp;
  PRINTFD(G, FB_Selector)
    " %s-DEBUG: entry.\n", __func__ ENDFD cnt = 0;
  /* number of pairs of atoms */
  np = VLAGetSize(pair) / 2;
  if(np) {

    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);  /* unnecessary? */
    /* flags initialized to false */
    flag1 = pymol::calloc<int>(I->NAtom);
    flag2 = pymol::calloc<int>(I->NAtom);

    /* we need to create two selection arrays: for the matched 
     * atoms in the original selections */
    p = pair;
    for(i = 0; i < np; i++) {   /* iterate through all pairs of matched residues */
      vi1 = *(p++);
      vi2 = *(p++);

      /* find positions in the selection arrays */
      /* SelectorGetResidueVLA returns a VLA with 3 entries per atom, index0=model,
       * index1=atom#, hence: *3, and *3+1 */
      mod1 = vla1[vi1 * 3];
      at1 = vla1[vi1 * 3 + 1];

      mod2 = vla2[vi2 * 3];
      at2 = vla2[vi2 * 3 + 1];

      PRINTFD(G, FB_Selector)
        " S.C.A.-DEBUG: mod1 %d at1 %d mod2 %d at2 %d\n", mod1, at1, mod2, at2
        ENDFD obj1 = I->Obj[mod1];
      obj2 = I->Obj[mod2];

      ai1 = obj1->AtomInfo + at1;
      ai2 = obj2->AtomInfo + at2;
      at1a = at1;
      at2a = at2;
      ai1a = ai1;
      ai2a = ai2;

      if(atomic_input) {
        index1 = SelectorGetObjAtmOffset(I, obj1, at1a);
        index2 = SelectorGetObjAtmOffset(I, obj2, at2a);
        flag1[index1] = true;
        flag2[index2] = true;
        cnt++;
      } else {

        /* search back to first atom in residue */
        while(at1a > 0 && AtomInfoSameResidue(G, ai1a, ai1a - 1)) { ai1a--; at1a--; }
        while(at2a > 0 && AtomInfoSameResidue(G, ai2a, ai2a - 1)) { ai2a--; at2a--; }

        while(1) {              /* match up all matching atom names in each residue */
          cmp = AtomInfoNameOrder(G, ai1a, ai2a);
          if(cmp == 0) {        /* atoms match */
            index1 = SelectorGetObjAtmOffset(I, obj1, at1a);
            index2 = SelectorGetObjAtmOffset(I, obj2, at2a);

            PRINTFD(G, FB_Selector)
              " S.C.A.-DEBUG: compare %s %s %d\n", LexStr(G, ai1a->name), LexStr(G, ai2a->name), cmp
              ENDFD PRINTFD(G, FB_Selector)
              " S.C.A.-DEBUG: entry %d %d\n",
              ai1a->selEntry, ai2a->selEntry ENDFD if((index1 >= 0) && (index2 >= 0)) {
              if(SelectorIsMember(G, ai1a->selEntry, sele1) &&
                 SelectorIsMember(G, ai2a->selEntry, sele2)) {
                if((!identical) || (ai1a->resn == ai2a->resn)) {
                  flag1[index1] = true;
                  flag2[index2] = true;
                  cnt++;
                }
              }
            }
            at1a++;
            at2a++;
          } else if(cmp < 0) {  /* 1 is before 2 */
            at1a++;
          } else if(cmp > 0) {  /* 1 is after 2 */
            at2a++;
          }
          if(at1a >= obj1->NAtom)
            break;
          if(at2a >= obj2->NAtom)
            break;
          ai1a = obj1->AtomInfo + at1a;
          ai2a = obj2->AtomInfo + at2a;
          /* make sure we're still in the same residue */
          if(!AtomInfoSameResidue(G, ai1a, ai1))
            break;
          if(!AtomInfoSameResidue(G, ai2a, ai2))
            break;
        }
      }
    }
    if(cnt) {
      SelectorEmbedSelection(G, flag1, name1, NULL, false, -1);
      SelectorEmbedSelection(G, flag2, name2, NULL, false, -1);
    }
    FreeP(flag1);
    FreeP(flag2);
  }
  PRINTFD(G, FB_Selector)
    " %s-DEBUG: exit, cnt = %d.\n", __func__, cnt ENDFD return cnt;
}


/*========================================================================*/
int SelectorCountStates(PyMOLGlobals * G, int sele)
{
  CSelector *I = G->Selector;
  int a;
  int result = 0;
  int n_frame;
  int at1;
  ObjectMolecule *last = NULL;
  ObjectMolecule *obj;
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  if(I->NAtom) {
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      obj = I->Obj[I->Table[a].model];
      if(obj != last) {
        at1 = I->Table[a].atom;
        if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
          {
            n_frame = obj->getNFrame();
            if(result < n_frame)
              result = n_frame;
          }
          last = obj;
        }
      }
    }
  }
  return (result);
}


/*========================================================================*/
int SelectorCheckIntersection(PyMOLGlobals * G, int sele1, int sele2)
{
  CSelector *I = G->Selector;
  int a;
  int at1;
  ObjectMolecule *obj;

  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  if(I->NAtom) {
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      obj = I->Obj[I->Table[a].model];
      at1 = I->Table[a].atom;
      if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele1) &&
         SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele2))
        return 1;
    }
  }
  return 0;
}


/*========================================================================*/
int SelectorCountAtoms(PyMOLGlobals * G, int sele, int state)
{
  CSelector *I = G->Selector;
  int a;
  int result = 0;
  int at1;
  ObjectMolecule *obj;

  SelectorUpdateTable(G, state, -1);
  if(I->NAtom) {
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      obj = I->Obj[I->Table[a].model];
      at1 = I->Table[a].atom;
      if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
        result++;
      }
    }
  }
  return (result);
}

/*========================================================================*/
void SelectorSetDeleteFlagOnSelectionInObject(PyMOLGlobals * G, int sele, ObjectMolecule *obj, signed char val){
  CSelector *I = G->Selector;
  int a,nflags = 0;
  int at1;
  ObjectMolecule *obj0;

  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  if(I->NAtom) {
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      obj0 = I->Obj[I->Table[a].model];
      if (obj==obj0){
	at1 = I->Table[a].atom;
	if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
	  obj->AtomInfo[at1].deleteFlag = val;
	  nflags++;
	}
      }
    }
  }
}

/*========================================================================*/
int *SelectorGetResidueVLA(PyMOLGlobals * G, int sele, int ca_only,
                           ObjectMolecule * exclude)
{
  /* returns a VLA containing atom indices followed by residue integers
     (residue names packed as characters into integers)
     The indices are the first and last residue in the selection...
   */
  CSelector *I = G->Selector;
  int *result = NULL, *r;
  AtomInfoType *ai1 = NULL, *ai2;

  /* update the selector's table */
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);

  /* make room for model#, at#, rcode, per atom */
  result = VLAlloc(int, I->NAtom * 3);

  r = result;
  PRINTFD(G, FB_Selector)
    " %s-DEBUG: entry, sele = %d\n", __func__, sele ENDFD;

  for(SeleAtomIterator iter(G, sele); iter.next();) {
    if(iter.obj == exclude)
      continue;

    ai2 = iter.getAtomInfo();

    if(ca_only) {
      if(!(ai2->flags & cAtomFlag_guide))
        continue;
    } else if(ai1 && AtomInfoSameResidue(G, ai1, ai2)) {
      continue;
    }

    *(r++) = I->Table[iter.a].model;
    *(r++) = I->Table[iter.a].atom;

    const char * resn = LexStr(G, ai2->resn);
    *(r)    = (resn[0] << (8 * 2));
    if (resn[0] && resn[1]) {
      *(r) |= (resn[1] << (8 * 1));
      *(r) |= (resn[2] << (8 * 0));
    }
    r++;

    ai1 = ai2;
  }
  if(result) {
    VLASize(result, int, (ov_size) (r - result));
  }
  PRINTFD(G, FB_Selector)
    " %s-DEBUG: exit, result = %p, size = %d\n", __func__,
    (void *) result, (unsigned int) VLAGetSize(result)
    ENDFD;

  return (result);
}


/*========================================================================*/
/* bad to make this non-static? */
/* static int *SelectorGetIndexVLA(PyMOLGlobals * G, int sele) */
static int *SelectorGetIndexVLA(PyMOLGlobals * G, int sele)
{
  return (SelectorGetIndexVLAImpl(G, G->Selector, sele));
}
static int *SelectorGetIndexVLAImpl(PyMOLGlobals * G, CSelector *I, int sele)
{                               /* assumes updated tables */
  int a, c = 0;
  int *result = NULL;
  ObjectMolecule *obj;
  int at1;

  result = VLAlloc(int, (I->NAtom / 10) + 1);
  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    obj = I->Obj[I->Table[a].model];
    at1 = I->Table[a].atom;
    if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
      VLACheck(result, int, c);
      result[c++] = a;
    }
  }
  VLASize(result, int, c);
  return (result);
}


/*========================================================================*/
void SelectorUpdateObjectSele(PyMOLGlobals * G, ObjectMolecule * obj)
{
  if(obj->Name[0]) {
    SelectorDelete(G, obj->Name);
    SelectorCreate(G, obj->Name, NULL, obj, true, NULL);    
    /* create a selection with same name */
    if(SettingGetGlobal_b(G, cSetting_auto_classify_atoms))
    {
      SelectorClassifyAtoms(G, 0, false, obj);

      // for file formats other than PDB
      if (obj->need_hetatm_classification) {
        for (auto ai = obj->AtomInfo.data(), ai_end = ai + obj->NAtom;
            ai != ai_end; ++ai) {
          if (!(ai->flags & cAtomFlag_polymer)) {
            ai->hetatm = true;
            ai->flags |= cAtomFlag_ignore;
          }
        }
        obj->need_hetatm_classification = false;
      }
    }
  }
}


/*========================================================================*/
void SelectorLogSele(PyMOLGlobals * G, const char *name)
{
  CSelector *I = G->Selector;
  int a;
  OrthoLineType line, buf1;
  int cnt = -1;
  int first = 1;
  int append = 0;
  ObjectMolecule *obj;
  int at1;
  int sele;
  int logging;
  int robust;
  logging = SettingGetGlobal_i(G, cSetting_logging);
  robust = SettingGetGlobal_b(G, cSetting_robust_logs);
  if(logging) {
    sele = SelectorIndexByName(G, name);
    if(sele >= 0) {
      SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        obj = I->Obj[I->Table[a].model];
        at1 = I->Table[a].atom;
        if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
          if(cnt < 0) {
            if(first) {
              switch (logging) {
              case cPLog_pml:
                sprintf(line, "_ cmd.select(\"%s\",\"(", name);
                break;
              case cPLog_pym:
                sprintf(line, "cmd.select(\"%s\",\"(", name);
                break;
              }
              append = 0;
              cnt = 0;
              first = 0;
            } else {
              switch (logging) {
              case cPLog_pml:
                sprintf(line, "_ cmd.select(\"%s\",\"(%s", name, name);
                break;
              case cPLog_pym:
                sprintf(line, "cmd.select(\"%s\",\"(%s", name, name);
                break;
              }
              append = 1;
              cnt = 0;
            }
          }
          if(append)
            strcat(line, "|");
          if(robust)
            ObjectMoleculeGetAtomSeleFast(obj, at1, buf1);
          else
            sprintf(buf1, "%s`%d", obj->Name, at1 + 1);
          strcat(line, buf1);
          append = 1;
          cnt++;
          if(strlen(line) > (sizeof(OrthoLineType) / 2)) {
            strcat(line, ")\")\n");
            PLog(G, line, cPLog_no_flush);
            cnt = -1;
          }
        }
      }
      if(cnt > 0) {
        strcat(line, ")\")\n");
        PLog(G, line, cPLog_no_flush);
        PLogFlush(G);
      }
    }
  }
}


/*========================================================================*/
/*
 * This is the most heavily called routine in interactive PyMOL
 *
 * s:    AtomInfoType.selEntry
 * sele: selection index or 0 for "all"
 */
int SelectorIsMember(PyMOLGlobals * G, int s, int sele)
{
  if(sele > 1) {
    const MemberType *mem, *member = G->Selector->Member;
    for(; s; s = mem->next) {
      mem = member + s;
      if(mem->selection == sele)
        return mem->tag;
    }
  } else if(!sele)
    return true;                /* "all" is selection number 0, unordered */
  return false;
}


/*========================================================================*/
int SelectorMoveMember(PyMOLGlobals * G, int s, int sele_old, int sele_new)
{
  CSelector *I = G->Selector;
  int result = false;
  while(s) {
    if(I->Member[s].selection == sele_old) {
      I->Member[s].selection = sele_new;
      result = true;
    }
    s = I->Member[s].next;
  }
  return result;
}


/*========================================================================*/
static int SelectorIndexByID(PyMOLGlobals * G, int id)
{
  CSelector *I = G->Selector;
  int i = 0;
  int result = -1;
  SelectionInfoRec *info = I->Info;
  while(i < I->NActive) {
    if((info++)->ID == id) {
      result = i;
      break;
    }
    i++;
  }
  return result;
}


/*========================================================================*/
ObjectMolecule *SelectorGetFastSingleObjectMolecule(PyMOLGlobals * G, int sele)
{
  CSelector *I = G->Selector;
  ObjectMolecule *result = NULL;
  SelectionInfoRec *info;
  int sele_idx = SelectorIndexByID(G, sele);
  if((sele_idx >= 0) && (sele_idx < I->NActive)) {
    info = I->Info + sele_idx;
    if(info->justOneObjectFlag) {
      if(ExecutiveValidateObjectPtr(G, (CObject *) info->theOneObject, cObjectMolecule))
        result = info->theOneObject;
    } else {
      result = SelectorGetSingleObjectMolecule(G, sele);        /* fallback onto slow approach */
    }
  }
  return (result);
}


/*========================================================================*/
ObjectMolecule *SelectorGetFastSingleAtomObjectIndex(PyMOLGlobals * G, int sele,
                                                     int *index)
{
  CSelector *I = G->Selector;
  ObjectMolecule *result = NULL;
  SelectionInfoRec *info;
  int got_it = false;
  int sele_idx = SelectorIndexByID(G, sele);

  if((sele_idx >= 0) && (sele_idx < I->NActive)) {
    info = I->Info + sele_idx;
    if(info->justOneObjectFlag && info->justOneAtomFlag) {
      ObjectMolecule *obj = info->theOneObject;
      int at = info->theOneAtom;
      if(ExecutiveValidateObjectPtr(G, (CObject *) obj, cObjectMolecule)) {
        if((at < obj->NAtom) && SelectorIsMember(G, obj->AtomInfo[at].selEntry, sele)) {
          result = obj;
          *index = at;
          got_it = true;
        }
      }
    }
    if(!got_it) {               /* fallback onto slow approach */
      if(!SelectorGetSingleAtomObjectIndex(G, sele, &result, index))
        result = NULL;
    }
  }
  return (result);
}


/*========================================================================
 * SelectorGetSingleObjectMolecule -- get a ptr to the molecule indiecated
 *    by the selection parameter
 * PARAMS
 *   (int) selection #
 * RETURNS
 *   (ptr) pts to the ObjectMolecule or NULL if not found
 */
ObjectMolecule *SelectorGetSingleObjectMolecule(PyMOLGlobals * G, int sele)
{
  /* slow way */

  int a;
  ObjectMolecule *result = NULL;
  ObjectMolecule *obj;
  CSelector *I = G->Selector;
  int at1;
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);

  for(a = 0; a < I->NAtom; a++) {
    obj = I->Obj[I->Table[a].model];
    at1 = I->Table[a].atom;
    if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
      if(result) {
        if(result != obj) {
          result = NULL;
          break;
        }
      } else {
        result = obj;
      }
    }
  }
  return (result);
}


/*========================================================================*/
ObjectMolecule *SelectorGetFirstObjectMolecule(PyMOLGlobals * G, int sele)
{
  /* slow way */

  int a;
  ObjectMolecule *result = NULL;
  ObjectMolecule *obj;
  CSelector *I = G->Selector;
  int at1;
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);

  for(a = 0; a < I->NAtom; a++) {
    obj = I->Obj[I->Table[a].model];
    at1 = I->Table[a].atom;
    if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
      result = obj;
      break;
    }
  }
  return (result);
}


/*========================================================================*/
ObjectMolecule **SelectorGetObjectMoleculeVLA(PyMOLGlobals * G, int sele)
{
  int a;
  ObjectMolecule *last = NULL;
  ObjectMolecule *obj, **result = NULL;
  CSelector *I = G->Selector;
  int at1;
  int n = 0;
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);

  result = VLAlloc(ObjectMolecule *, 10);
  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    obj = I->Obj[I->Table[a].model];
    at1 = I->Table[a].atom;
    if(SelectorIsMember(G, obj->AtomInfo[at1].selEntry, sele)) {
      if(obj != last) {
        VLACheck(result, ObjectMolecule *, n);
        result[n] = obj;
        last = obj;
        n++;
      }
    }
  }
  VLASize(result, ObjectMolecule *, n);
  return (result);
}


/*========================================================================*/
int SelectorGetSingleAtomObjectIndex(PyMOLGlobals * G, int sele, ObjectMolecule ** in_obj,
                                     int *index)
{
  /* slow way */

  int found_it = false;
  int a;
  void *iterator = NULL;
  ObjectMolecule *obj = NULL;

  while(ExecutiveIterateObjectMolecule(G, &obj, &iterator)) {
    int n_atom = obj->NAtom;
    const AtomInfoType *ai = obj->AtomInfo.data();
    for(a = 0; a < n_atom; a++) {
      int s = (ai++)->selEntry;
      if(SelectorIsMember(G, s, sele)) {
        if(found_it) {
          return false;         /* ADD'L EXIT POINT */
        } else {
          found_it = true;
          (*in_obj) = obj;
          (*index) = a;
        }
      }
    }
  }
  return (found_it);
}


/*========================================================================*/
int SelectorGetSingleAtomVertex(PyMOLGlobals * G, int sele, int state, float *v)
{
  ObjectMolecule *obj = NULL;
  int index = 0;
  int found_it = false;
  if(SelectorGetSingleAtomObjectIndex(G, sele, &obj, &index))
    found_it = ObjectMoleculeGetAtomTxfVertex(obj, state, index, v);
  return (found_it);
}


/*========================================================================*/
void SelectorDeletePrefixSet(PyMOLGlobals * G, const char *pref)
{
  ov_diff a;
  CSelector *I = G->Selector;
  SelectorWordType name_copy;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);

  while(1) {
    a = SelectGetNameOffset(G, pref, strlen(pref), ignore_case);
    if(a > 0) {
      strcpy(name_copy, I->Name[a]);
      ExecutiveDelete(G, name_copy);    /* import to use a copy, otherwise 
                                         * you'll delete all objects  */
    } else
      break;
  }
}


/*========================================================================*/
#define MAX_DEPTH 1000

static int SelectorCheckNeighbors(PyMOLGlobals * G, int maxDist, ObjectMolecule * obj,
                                  int at1, int at2, int *zero, int *scratch)
{
  int s;
  int a, a1;
  int stkDepth = 0;
  int si = 0;
  int stk[MAX_DEPTH];
  int dist = 0;

  zero[at1] = dist;
  scratch[si++] = at1;
  stk[stkDepth] = at1;
  stkDepth++;

  while(stkDepth) {             /* this will explore a tree */
    stkDepth--;
    a = stk[stkDepth];
    dist = zero[a] + 1;

    s = obj->Neighbor[a];       /* add neighbors onto the stack */
    s++;                        /* skip count */
    while(1) {
      a1 = obj->Neighbor[s];
      if(a1 == at2) {
        while(si--) {
          zero[scratch[si]] = 0;
        }
        /* EXIT POINT 1 */
        return 1;
      }
      if(a1 >= 0) {
        if((!zero[a1]) && (stkDepth < MAX_DEPTH) && (dist < maxDist)) {
          zero[a1] = dist;
          scratch[si++] = a1;
          stk[stkDepth] = a1;
          stkDepth++;
        }
      } else
        break;
      s += 2;
    }
  }
  while(si--) {
    zero[scratch[si]] = 0;
  }
  /* EXIT POINT 2 */
  return 0;
}


/*========================================================================*/
static
int SelectorWalkTree(PyMOLGlobals * G, int *atom, int *comp, int *toDo, int **stk,
                     int stkDepth, ObjectMolecule * obj,
                     int sele1, int sele2, int sele3, int sele4)
{
  int s;
  int c = 0;
  int a, a1;
  int seleFlag;
  AtomInfoType *ai;

  while(stkDepth) {             /* this will explore a tree, stopping at protected atoms */
    stkDepth--;
    a = (*stk)[stkDepth];
    toDo[a] = 0;
    seleFlag = false;
    ai = obj->AtomInfo + a;
    s = ai->selEntry;
    seleFlag = SelectorIsMember(G, s, sele1);
    if(!seleFlag)
      seleFlag = SelectorIsMember(G, s, sele2);
    if(!seleFlag)
      seleFlag = SelectorIsMember(G, s, sele3);
    if(!seleFlag)
      seleFlag = SelectorIsMember(G, s, sele4);
    if(!seleFlag) {
      if(!(ai->protekted == 1)) {       /* if not explicitly protected... */
        atom[a] = 1;            /* mark this atom into the selection */
        comp[a] = 1;
      }
      s = obj->Neighbor[a];     /* add neighbors onto the stack */
      s++;                      /* skip count */
      while(1) {
        a1 = obj->Neighbor[s];
        if(a1 >= 0) {
          if(toDo[a1]) {
            VLACheck((*stk), int, stkDepth);
            (*stk)[stkDepth] = a1;
            stkDepth++;
          }
        } else
          break;
        s += 2;
      }
      c++;
    }
  }
  return (c);
}


/*========================================================================*/
static int SelectorWalkTreeDepth(PyMOLGlobals * G, int *atom, int *comp, int *toDo,
                                 int **stk, int stkDepth, ObjectMolecule * obj, int sele1,
                                 int sele2, int sele3, int sele4, int **extraStk,
                                 WalkDepthRec * wd)
{
  int s;
  int c = 0;
  int a, a1;
  int seleFlag;
  int depth;
  AtomInfoType *ai;

  wd->depth1 = -1;
  wd->depth2 = -1;
  wd->depth3 = -1;
  wd->depth4 = -1;
  VLACheck(*extraStk, int, stkDepth);
  UtilZeroMem(*extraStk, sizeof(int) * stkDepth);

  while(stkDepth) {             /* this will explore a tree, stopping at protected atoms */
    stkDepth--;
    a = (*stk)[stkDepth];
    depth = ((*extraStk)[stkDepth] + 1);
    seleFlag = false;
    ai = obj->AtomInfo + a;
    s = ai->selEntry;

    /* record how many cycles it take to reach each & any picked atoms */

    seleFlag = false;
    if(SelectorIsMember(G, s, sele1)) {
      if(((wd->depth1 < 0) || (wd->depth1 > depth))) {
        wd->depth1 = depth;
      }
      seleFlag = true;
    }
    if(SelectorIsMember(G, s, sele2)) {
      if(((wd->depth2 < 0) || (wd->depth2 > depth))) {
        wd->depth2 = depth;
      }
      seleFlag = true;
    }
    if(SelectorIsMember(G, s, sele3)) {
      if(((wd->depth3 < 0) || (wd->depth3 > depth))) {
        wd->depth3 = depth;
      }
      seleFlag = true;
    }
    if(SelectorIsMember(G, s, sele4)) {
      if(((wd->depth4 < 0) || (wd->depth4 > depth))) {
        wd->depth4 = depth;
      }
      seleFlag = true;
    }

    if(!seleFlag) {
      toDo[a] = 0;
      if(!(ai->protekted == 1)) {       /* if not explicitly protected... */
        atom[a] = 1;            /* mark this atom into the selection */
        comp[a] = 1;
      }
      s = obj->Neighbor[a];     /* add neighbors onto the stack */
      s++;                      /* skip count */
      while(1) {
        a1 = obj->Neighbor[s];
        if(a1 >= 0) {
          if(toDo[a1]) {
            VLACheck((*stk), int, stkDepth);
            (*stk)[stkDepth] = a1;
            VLACheck((*extraStk), int, stkDepth);
            (*extraStk)[stkDepth] = depth;
            stkDepth++;
          }
        } else
          break;
        s += 2;
      }
      c++;
    }
  }
  return (c);
}


/*========================================================================*/

int SelectorIsAtomBondedToSele(PyMOLGlobals * G, ObjectMolecule * obj, int sele1atom,
                               int sele2)
{
  int a0, a2, s, ss;
  int bonded = false;
  ObjectMoleculeUpdateNeighbors(obj);

  a0 = ObjectMoleculeGetAtomIndex(obj, sele1atom);

  if(a0 >= 0) {
    s = obj->Neighbor[a0];
    s++;                        /* skip count */
    while(1) {
      a2 = obj->Neighbor[s];
      if(a2 < 0)
        break;
      ss = obj->AtomInfo[a2].selEntry;
      if(SelectorIsMember(G, ss, sele2)) {
        bonded = true;
        break;
      }
      s += 2;
    }
  }
  return bonded;
}

static void update_min_walk_depth(WalkDepthRec * minWD,
                                  int frag, WalkDepthRec * wd,
                                  int sele1, int sele2, int sele3, int sele4)
{
  /* first, does this fragment even qualify ? */
  int qualifies = true;
  int cnt = 0;
  wd->sum = 0;
  if(sele1 >= 0) {
    if(wd->depth1 < 0) {
      qualifies = false;
    } else {
      wd->sum += wd->depth1;
      cnt++;
    }
  }
  if(sele2 >= 0) {
    if(wd->depth2 < 0) {
      qualifies = false;
    } else {
      wd->sum += wd->depth2;
      cnt++;
    }
  }
  if(sele3 >= 0) {
    if(wd->depth3 < 0) {
      qualifies = false;
    } else {
      wd->sum += wd->depth3;
      cnt++;
    }
  }
  if(sele4 >= 0) {
    if(wd->depth4 < 0) {
      qualifies = false;
    } else {
      wd->sum += wd->depth4;
      cnt++;
    }
  }
  if(qualifies && (cnt > 1)) {

    /* is it better than the current min? */

    if((!minWD->frag) || (wd->sum < minWD->sum)) {
      (*minWD) = (*wd);
      minWD->frag = frag;
    }
  }
}


/*========================================================================*/
int SelectorSubdivide(PyMOLGlobals * G, const char *pref, int sele1, int sele2,
                      int sele3, int sele4, const char *fragPref, const char *compName, int *bondMode)
{
  CSelector *I = G->Selector;
  int a0 = 0, a1 = 0, a2;
  int *atom = NULL;
  int *toDo = NULL;
  int *comp = NULL;
  int *pkset = NULL;
  int set_cnt = 0;
  int nFrag = 0;
  int *stk = NULL;
  int stkDepth;
  int c, s;
  int cycFlag = false;
  SelectorWordType name, link_sele = "";
  ObjectMolecule *obj1 = NULL, *obj2 = NULL, *obj3 = NULL, *obj4 = NULL;
  int index1 = 0, index2 = 0, index3 = 0, index4 = 0;

  /* this is seriously getting out of hand -- need to switch over to arrays soon */

  int *atom1_base = NULL, *atom2_base = NULL, *atom3_base = NULL, *atom4_base = NULL;
  int *toDo1_base = NULL, *toDo2_base = NULL, *toDo3_base = NULL, *toDo4_base = NULL;
  int *comp1_base = NULL, *comp2_base = NULL, *comp3_base = NULL, *comp4_base = NULL;
  int *pkset1_base = NULL, *pkset2_base = NULL, *pkset3_base = NULL, *pkset4_base = NULL;

  PRINTFD(G, FB_Selector)
    " SelectorSubdivideObject: entered...\n" ENDFD;
  SelectorDeletePrefixSet(G, pref);
  SelectorDeletePrefixSet(G, fragPref);
  ExecutiveDelete(G, cEditorLink);
  ExecutiveDelete(G, cEditorSet);
  /* delete any existing matches */

  obj1 = SelectorGetFastSingleAtomObjectIndex(G, sele1, &index1);
  obj2 = SelectorGetFastSingleAtomObjectIndex(G, sele2, &index2);
  obj3 = SelectorGetFastSingleAtomObjectIndex(G, sele3, &index3);
  obj4 = SelectorGetFastSingleAtomObjectIndex(G, sele4, &index4);

  if(obj1 || obj2 || obj3 || obj4) {

    if(obj1)
      ObjectMoleculeUpdateNeighbors(obj1);
    if(obj2)
      ObjectMoleculeUpdateNeighbors(obj2);
    if(obj3)
      ObjectMoleculeUpdateNeighbors(obj3);
    if(obj4)
      ObjectMoleculeUpdateNeighbors(obj4);

    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);

    comp = pymol::calloc<int>(I->NAtom);
    atom = pymol::malloc<int>(I->NAtom);
    toDo = pymol::malloc<int>(I->NAtom);
    pkset = pymol::calloc<int>(I->NAtom);

    /* NOTE: SeleBase only safe with cSelectorUpdateTableAllStates!  */

    if(obj1) {
      atom1_base = atom + obj1->SeleBase;
      toDo1_base = toDo + obj1->SeleBase;
      comp1_base = comp + obj1->SeleBase;
      pkset1_base = pkset + obj1->SeleBase;
    }

    if(obj2) {
      atom2_base = atom + obj2->SeleBase;
      toDo2_base = toDo + obj2->SeleBase;
      comp2_base = comp + obj2->SeleBase;
      pkset2_base = pkset + obj2->SeleBase;
    }

    if(obj3) {
      atom3_base = atom + obj3->SeleBase;
      toDo3_base = toDo + obj3->SeleBase;
      comp3_base = comp + obj3->SeleBase;
      pkset3_base = pkset + obj3->SeleBase;
    }

    if(obj4) {
      atom4_base = atom + obj4->SeleBase;
      toDo4_base = toDo + obj4->SeleBase;
      comp4_base = comp + obj4->SeleBase;
      pkset4_base = pkset + obj4->SeleBase;
    }

    stk = VLAlloc(int, 100);

    {
      int a;
      int *p1;
      p1 = toDo;
      for(a = 0; a < I->NAtom; a++)
        *(p1++) = true;
    }

    if(*bondMode) {
      /* verify bond mode, or clear the flag */

      *bondMode = false;

      if((sele1 >= 0) && (sele2 >= 0) && (sele3 < 0) && (sele4 < 0) && (obj1 == obj2)) {
        /* two selections only, in same object... */

        a0 = index1;
        a1 = index2;

        if((a0 >= 0) && (a1 >= 0)) {
          s = obj1->Neighbor[a0];       /* add neighbors onto the stack */
          s++;                  /* skip count */
          while(1) {
            a2 = obj1->Neighbor[s];
            if(a2 < 0)
              break;
            if(a2 == a1) {
              *bondMode = true;
              break;
            }
            s += 2;
          }
        }
      }
    }

    /* ===== BOND MODE ===== (sele0 and sele1 only) */

    if(*bondMode) {
      if(obj1 == obj2) {        /* just to be safe */

        pkset1_base[a0] = 1;
        pkset1_base[a1] = 1;
        SelectorEmbedSelection(G, pkset, cEditorBond, NULL, false, -1);

        a0 = index1;
        if(a0 >= 0) {
          stkDepth = 0;
          s = obj1->Neighbor[a0];       /* add neighbors onto the stack */
          s++;                  /* skip count */
          while(1) {
            a1 = obj1->Neighbor[s];
            if(a1 >= 0) {
              if(toDo1_base[a1]) {
                VLACheck(stk, int, stkDepth);
                stk[stkDepth] = a1;
                stkDepth++;
              }
            } else
              break;
            s += 2;
          }
          UtilZeroMem(atom, sizeof(int) * I->NAtom);
          atom1_base[a0] = 1;   /* create selection for this atom alone as fragment base atom */
          comp1_base[a0] = 1;
          sprintf(name, "%s%1d", fragPref, nFrag + 1);
          SelectorEmbedSelection(G, atom, name, NULL, false, -1);
          c =
            SelectorWalkTree(G, atom1_base, comp1_base, toDo1_base, &stk, stkDepth, obj1,
                             sele1, sele2, -1, -1) + 1;
          sprintf(name, "%s%1d", pref, nFrag + 1);

          /* check for cyclic situation */
          cycFlag = false;
          a2 = index2;
          if(a2 >= 0) {
            stkDepth = 0;
            s = obj1->Neighbor[a2];     /* add neighbors onto the stack */
            s++;                /* skip count */
            while(1) {
              a1 = obj1->Neighbor[s];
              if(a1 < 0)
                break;
              if((a1 >= 0) && (a1 != a0)) {
                if(!toDo1_base[a1]) {
                  cycFlag = true;       /* we have a cycle... */
                  break;
                }
              }
              s += 2;
            }
          }
          if(cycFlag) {         /* cyclic situation is a bit complex... */

            a0 = index2;
            if(a0 >= 0) {
              stkDepth = 0;
              s = obj1->Neighbor[a0];   /* add neighbors onto the stack */
              s++;              /* skip count */
              while(1) {
                a1 = obj1->Neighbor[s];
                if(a1 >= 0) {
                  if(toDo1_base[a1]) {
                    VLACheck(stk, int, stkDepth);
                    stk[stkDepth] = a1;
                    stkDepth++;
                  }
                } else
                  break;
                s += 2;
              }
              atom1_base[a0] = 1;
              comp1_base[a0] = 1;
              c =
                SelectorWalkTree(G, atom1_base, comp1_base, toDo1_base, &stk, stkDepth,
                                 obj1, sele1, sele2, -1, -1) + 1;
            }
          }
          SelectorEmbedSelection(G, atom, name, NULL, false, -1);
          nFrag++;
        }

        if(!cycFlag) {
          a0 = index2;
          if(a0 >= 0) {
            stkDepth = 0;
            s = obj1->Neighbor[a0];     /* add neighbors onto the stack */
            s++;                /* skip count */
            while(1) {
              a1 = obj1->Neighbor[s];
              if(a1 >= 0) {
                if(toDo1_base[a1]) {
                  VLACheck(stk, int, stkDepth);
                  stk[stkDepth] = a1;
                  stkDepth++;
                }
              } else
                break;
              s += 2;
            }

            UtilZeroMem(atom, sizeof(int) * I->NAtom);
            atom1_base[a0] = 1; /* create selection for this atom alone as fragment base atom */
            comp1_base[a0] = 1;
            sprintf(name, "%s%1d", fragPref, nFrag + 1);
            SelectorEmbedSelection(G, atom, name, NULL, false, -1);
            c =
              SelectorWalkTree(G, atom1_base, comp1_base, toDo1_base, &stk, stkDepth,
                               obj1, sele1, sele2, -1, -1) + 1;
            sprintf(name, "%s%1d", pref, nFrag + 1);
            SelectorEmbedSelection(G, atom, name, NULL, false, -1);
            nFrag++;
          }
        }
      }
    } else {
      /* ===== WALK MODE ===== (any combination of sele0, sele1, sele2, sele3 */

      int *extraStk = VLAlloc(int, 50);
      WalkDepthRec curWalk, minWalk;
      minWalk.sum = 0;
      minWalk.frag = 0;

      if(obj1) {
        a0 = index1;
        if(a0 >= 0) {
          pkset1_base[a0] = 1;
          set_cnt++;
          comp1_base[a0] = 1;
          stkDepth = 0;
          s = obj1->Neighbor[a0];       /* add neighbors onto the stack */
          s++;                  /* skip count */
          while(1) {
            a1 = obj1->Neighbor[s];
            if(a1 < 0)
              break;
            if(toDo1_base[a1]) {
              stkDepth = 1;
              stk[0] = a1;
              UtilZeroMem(atom, sizeof(int) * I->NAtom);
              atom1_base[a1] = 1;       /* create selection for this atom alone as fragment base atom */
              comp1_base[a1] = 1;
              sprintf(name, "%s%1d", fragPref, nFrag + 1);
              SelectorEmbedSelection(G, atom, name, NULL, false, -1);
              atom1_base[a1] = 0;
              c = SelectorWalkTreeDepth(G, atom1_base, comp1_base, toDo1_base, &stk,
                                        stkDepth, obj1, sele1, sele2, sele3, sele4,
                                        &extraStk, &curWalk);
              if(c) {
                nFrag++;
                sprintf(name, "%s%1d", pref, nFrag);
                SelectorEmbedSelection(G, atom, name, NULL, false, -1);
                update_min_walk_depth(&minWalk,
                                      nFrag, &curWalk, sele1, sele2, sele3, sele4);
              }
            }
            s += 2;
          }
        }
      }

      if(obj2) {
        a0 = index2;
        if(a0 >= 0) {
          pkset2_base[a0] = 1;
          set_cnt++;
          comp2_base[a0] = 1;
          stkDepth = 0;
          s = obj2->Neighbor[a0];       /* add neighbors onto the stack */
          s++;                  /* skip count */
          while(1) {
            a1 = obj2->Neighbor[s];
            if(a1 < 0)
              break;
            if(toDo2_base[a1]) {
              stkDepth = 1;
              stk[0] = a1;
              UtilZeroMem(atom, sizeof(int) * I->NAtom);
              atom2_base[a1] = 1;       /* create selection for this atom alone as fragment base atom */
              comp2_base[a1] = 1;
              sprintf(name, "%s%1d", fragPref, nFrag + 1);
              SelectorEmbedSelection(G, atom, name, NULL, false, -1);
              atom2_base[a1] = 0;
              c = SelectorWalkTreeDepth(G, atom2_base, comp2_base, toDo2_base, &stk,
                                        stkDepth, obj2, sele1, sele2, sele3, sele4,
                                        &extraStk, &curWalk);
              if(c) {
                nFrag++;
                sprintf(name, "%s%1d", pref, nFrag);
                SelectorEmbedSelection(G, atom, name, NULL, false, -1);
                update_min_walk_depth(&minWalk,
                                      nFrag, &curWalk, sele1, sele2, sele3, sele4);
              }
            }
            s += 2;
          }
        }
      }

      if(obj3) {
        a0 = index3;
        if(a0 >= 0) {
          pkset3_base[a0] = 1;
          set_cnt++;
          comp3_base[a0] = 1;
          stkDepth = 0;
          s = obj3->Neighbor[a0];       /* add neighbors onto the stack */
          s++;                  /* skip count */
          while(1) {
            a1 = obj3->Neighbor[s];
            if(a1 < 0)
              break;
            if(toDo3_base[a1]) {
              stkDepth = 1;
              stk[0] = a1;
              UtilZeroMem(atom, sizeof(int) * I->NAtom);
              atom3_base[a1] = 1;       /* create selection for this atom alone as fragment base atom */
              comp3_base[a1] = 1;
              sprintf(name, "%s%1d", fragPref, nFrag + 1);
              SelectorEmbedSelection(G, atom, name, NULL, false, -1);
              atom3_base[a1] = 0;
              c = SelectorWalkTreeDepth(G, atom3_base, comp3_base, toDo3_base, &stk,
                                        stkDepth, obj3, sele1, sele2, sele3, sele4,
                                        &extraStk, &curWalk);
              if(c) {
                nFrag++;
                sprintf(name, "%s%1d", pref, nFrag);
                SelectorEmbedSelection(G, atom, name, NULL, false, -1);
                update_min_walk_depth(&minWalk,
                                      nFrag, &curWalk, sele1, sele2, sele3, sele4);

              }
            }
            s += 2;
          }
        }
      }

      if(obj4) {
        a0 = index4;
        if(a0 >= 0) {
          pkset4_base[a0] = 1;
          set_cnt++;
          comp4_base[a0] = 1;
          stkDepth = 0;
          s = obj4->Neighbor[a0];       /* add neighbors onto the stack */
          s++;                  /* skip count */
          while(1) {
            a1 = obj4->Neighbor[s];
            if(a1 < 0)
              break;
            if(toDo4_base[a1]) {
              stkDepth = 1;
              stk[0] = a1;
              UtilZeroMem(atom, sizeof(int) * I->NAtom);
              atom4_base[a1] = 1;       /* create selection for this atom alone as fragment base atom */
              comp4_base[a1] = 1;
              sprintf(name, "%s%1d", fragPref, nFrag + 1);
              SelectorEmbedSelection(G, atom, name, NULL, false, -1);
              atom4_base[a1] = 0;
              c = SelectorWalkTreeDepth(G, atom4_base, comp4_base, toDo4_base, &stk,
                                        stkDepth, obj4, sele1, sele2, sele3, sele4,
                                        &extraStk, &curWalk);
              if(c) {
                nFrag++;
                sprintf(name, "%s%1d", pref, nFrag);
                SelectorEmbedSelection(G, atom, name, NULL, false, -1);
                update_min_walk_depth(&minWalk,
                                      nFrag, &curWalk, sele1, sele2, sele3, sele4);
              }
            }
            s += 2;
          }
        }
      }

      if(minWalk.frag) {        /* create the linking selection if one exists */
        sprintf(link_sele, "%s%d|?pk1|?pk2|?pk3|?pk4", pref, minWalk.frag);
      }
      VLAFreeP(extraStk);
    }

    if(set_cnt > 1) {
      SelectorEmbedSelection(G, pkset, cEditorSet, NULL, false, -1);
    }

    if(nFrag) {
      SelectorEmbedSelection(G, comp, compName, NULL, false, -1);
    }

    if(link_sele[0])
      SelectorCreate(G, cEditorLink, link_sele, NULL, true, NULL);

    FreeP(toDo);
    FreeP(atom);
    FreeP(comp);
    FreeP(pkset);
    VLAFreeP(stk);
    SelectorClean(G);
  }
  PRINTFD(G, FB_Selector)
    " SelectorSubdivideObject: leaving...nFrag %d\n", nFrag ENDFD;

  return (nFrag);
}


/*========================================================================*/
int SelectorGetSeleNCSet(PyMOLGlobals * G, int sele)
{
  CSelector *I = G->Selector;

  int a, s, at = 0;
  ObjectMolecule *obj, *last_obj = NULL;
  int result = 0;

  if((obj = SelectorGetFastSingleAtomObjectIndex(G, sele, &at))) {
    int a = obj->NCSet;
    CoordSet *cs;
    int idx;

    while(a--) {
      cs = obj->CSet[a];
      idx = cs->atmToIdx(at);
      if(idx >= 0) {
        result = a + 1;
        break;
      }
    }
  } else {
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      obj = I->Obj[I->Table[a].model];
      if(obj != last_obj) {
        at = I->Table[a].atom;
        s = obj->AtomInfo[at].selEntry;
        if(SelectorIsMember(G, s, sele)) {
          if(result < obj->NCSet) {
            result = obj->NCSet;
            last_obj = obj;
          }
        }
      }
    }
  }
  return (result);
}


/*========================================================================*/
static int SelectorGetArrayNCSet(
    PyMOLGlobals* G, const sele_array_t& uptr, int no_dummies)
{
  const int* array = uptr.get();
  CSelector *I = G->Selector;
  int a;
  ObjectMolecule *obj;
  int result = 0;
  int start = 0;
  if(no_dummies)
    start = cNDummyAtoms;
  for(a = start; a < I->NAtom; a++) {
    if(*(array++)) {
      if(a >= cNDummyAtoms) {
        obj = I->Obj[I->Table[a].model];
        if(result < obj->NCSet)
          result = obj->NCSet;
      } else {
        if(result < 1)
          result = 1;           /* selected dummy has at least one CSet */
      }

    }
  }
  return (result);
}


/*========================================================================*/
float SelectorSumVDWOverlap(PyMOLGlobals * G, int sele1, int state1, int sele2,
                            int state2, float adjust)
{
  CSelector *I = G->Selector;
  int *vla = NULL;
  int c;
  float result = 0.0;
  float sumVDW = 0.0, dist;
  int a1, a2;
  AtomInfoType *ai1, *ai2;
  int at1, at2;
  CoordSet *cs1, *cs2;
  ObjectMolecule *obj1, *obj2;
  int idx1, idx2;
  int a;

  if(state1 < 0)
    state1 = 0;
  if(state2 < 0)
    state2 = 0;

  if(state1 != state2) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, state1, -1);
  }

  c =
    SelectorGetInterstateVLA(G, sele1, state1, sele2, state2, 2 * MAX_VDW + adjust, &vla);
  for(a = 0; a < c; a++) {
    a1 = vla[a * 2];
    a2 = vla[a * 2 + 1];

    at1 = I->Table[a1].atom;
    at2 = I->Table[a2].atom;

    obj1 = I->Obj[I->Table[a1].model];
    obj2 = I->Obj[I->Table[a2].model];

    if((state1 < obj1->NCSet) && (state2 < obj2->NCSet)) {
      cs1 = obj1->CSet[state1];
      cs2 = obj2->CSet[state2];
      if(cs1 && cs2) {          /* should always be true */

        ai1 = obj1->AtomInfo + at1;
        ai2 = obj2->AtomInfo + at2;

        idx1 = cs1->AtmToIdx[at1];      /* these are also pre-validated */
        idx2 = cs2->AtmToIdx[at2];

        sumVDW = ai1->vdw + ai2->vdw + adjust;
        dist = (float) diff3f(cs1->Coord + 3 * idx1, cs2->Coord + 3 * idx2);

        if(dist < sumVDW) {
          result += ((sumVDW - dist) / 2.0F);
        }
      }
    }
  }
  VLAFreeP(vla);
  return (result);
}


/*========================================================================*/
static int SelectorGetInterstateVLA(PyMOLGlobals * G,
                                    int sele1, int state1,
                                    int sele2, int state2, float cutoff, int **vla)
{                               /* Assumes valid tables */
  CSelector *I = G->Selector;
  MapType *map;
  const float *v2;
  int n1, n2;
  int c, i, j, h, k, l;
  int at;
  int a, s, idx;
  ObjectMolecule *obj;
  CoordSet *cs;

  if(!(*vla))
    (*vla) = VLAlloc(int, 1000);

  c = 0;
  n1 = 0;

  for(a = 0; a < I->NAtom; a++) {
    /* foreach atom, grab its atom ID, object ID, selection ID,
     * and current state's coordinate set */
    I->Flag1[a] = false;
    at = I->Table[a].atom;
    obj = I->Obj[I->Table[a].model];
    s = obj->AtomInfo[at].selEntry;
    if(SelectorIsMember(G, s, sele1)) {
      if(state1 < obj->NCSet)
        cs = obj->CSet[state1];
      else
        cs = NULL;
      if(cs) {
        if(CoordSetGetAtomVertex(cs, at, I->Vertex + 3 * a)) {
          I->Flag1[a] = true;
          n1++;
        }
      }
    }
  }
  /* now create and apply voxel map */
  c = 0;
  if(n1) {
    n2 = 0;
    map = MapNewFlagged(G, -cutoff, I->Vertex, I->NAtom, NULL, I->Flag1);
    if(map) {
      MapSetupExpress(map);
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        at = I->Table[a].atom;
        obj = I->Obj[I->Table[a].model];
        s = obj->AtomInfo[at].selEntry;
        if(SelectorIsMember(G, s, sele2)) {
          if(state2 < obj->NCSet)
            cs = obj->CSet[state2];
          else
            cs = NULL;
          if(cs) {
            idx = cs->atmToIdx(at);
            if(idx >= 0) {
              v2 = cs->Coord + (3 * idx);
              if(MapExclLocus(map, v2, &h, &k, &l)) {
                i = *(MapEStart(map, h, k, l));
                if(i) {
                  j = map->EList[i++];
                  while(j >= 0) {
                    if(within3f(I->Vertex + 3 * j, v2, cutoff)) {
                      VLACheck((*vla), int, c * 2 + 1);
                      *((*vla) + c * 2) = j;
                      *((*vla) + c * 2 + 1) = a;
                      c++;
                    }
                    j = map->EList[i++];
                  }
                }
              }
              n2++;
            }
          }
        }
      }
      MapFree(map);
    }
  }
  return (c);
}


/*========================================================================*/
int SelectorMapMaskVDW(PyMOLGlobals * G, int sele1, ObjectMapState * oMap, float buffer,
                       int state)
{
  CSelector *I = G->Selector;
  MapType *map;
  float *v2;
  int n1;
  int a, b, c, i, j, h, k, l;
  int at;
  int s;
  AtomInfoType *ai;
  ObjectMolecule *obj;
  CoordSet *cs;
  int state1, state2;
  int once_flag;

  c = 0;
  n1 = 0;
  SelectorUpdateTable(G, state, -1);

  for(a = 0; a < I->NAtom; a++) {
    I->Flag1[a] = false;
    at = I->Table[a].atom;
    obj = I->Obj[I->Table[a].model];
    s = obj->AtomInfo[at].selEntry;
    if(SelectorIsMember(G, s, sele1)) {
      once_flag = true;
      for(state2 = 0; state2 < obj->NCSet; state2++) {
        if(state < 0)
          once_flag = false;
        if(!once_flag)
          state1 = state2;
        else
          state1 = state;
        if(state1 < obj->NCSet)
          cs = obj->CSet[state1];
        else
          cs = NULL;
        if(cs) {
          if(CoordSetGetAtomVertex(cs, at, I->Vertex + 3 * a)) {
            I->Flag1[a] = true;
            n1++;
          }
        }
        if(once_flag)
          break;
      }
    }
  }
  /* now create and apply voxel map */
  c = 0;
  if(n1) {
    map = MapNewFlagged(G, -(buffer + MAX_VDW), I->Vertex, I->NAtom, NULL, I->Flag1);
    if(map) {
      MapSetupExpress(map);

      for(a = oMap->Min[0]; a <= oMap->Max[0]; a++) {
        for(b = oMap->Min[1]; b <= oMap->Max[1]; b++) {
          for(c = oMap->Min[2]; c <= oMap->Max[2]; c++) {
            F3(oMap->Field->data, a, b, c) = 0.0;

            v2 = F4Ptr(oMap->Field->points, a, b, c, 0);

            if(MapExclLocus(map, v2, &h, &k, &l)) {
              i = *(MapEStart(map, h, k, l));
              if(i) {
                j = map->EList[i++];
                while(j >= 0) {
                  ai = I->Obj[I->Table[j].model]->AtomInfo + I->Table[j].atom;
                  if(within3f(I->Vertex + 3 * j, v2, ai->vdw + buffer)) {
                    F3(oMap->Field->data, a, b, c) = 1.0;
                  }
                  j = map->EList[i++];
                }
              }
            }
          }
        }
      }
      oMap->Active = true;
      MapFree(map);
    }
  }
  return (c);
}

static double max2d(double a, double b)
{
  if(a > b)
    return a;
  else
    return b;
}

static double max6d(double a, double b, double c, double d, double e, double f)
{

  if(d > a)
    a = d;
  if(e > b)
    b = e;
  if(f > c)
    c = f;
  if(b > a)
    a = b;
  if(c > a)
    a = c;
  return (a);
}

#define D_SMALL10 1e-10

typedef double AtomSF[11];


/*========================================================================*/
int SelectorMapGaussian(PyMOLGlobals * G, int sele1, ObjectMapState * oMap,
                        float buffer, int state, int normalize, int use_max, int quiet,
                        float resolution)
{
  CSelector *I = G->Selector;
  MapType *map;
  float *v2;
  int n1, n2;
  int a, b, c, i, j, h, k, l;
  int at;
  int s, idx;
  AtomInfoType *ai;
  ObjectMolecule *obj;
  CoordSet *cs;
  int state1, state2;
  float *point = NULL, *fp;
  int *sfidx = NULL, *ip;
  float *b_factor = NULL, *bf, bfact;
  float *occup = NULL, *oc;
  int prot;
  int once_flag;
  float d, e_val;
  double sum, sumsq;
  float mean, stdev;
  double sf[256][11], *sfp;
  AtomSF *atom_sf = NULL;
  double b_adjust = (double) SettingGetGlobal_f(G, cSetting_gaussian_b_adjust);
  double elim = 7.0;
  double rcut2;
  float rcut;
  float max_rcut = 0.0F;
  float b_floor = SettingGetGlobal_f(G, cSetting_gaussian_b_floor);
  float blur_factor = 1.0F;

  {
    if(resolution < R_SMALL4)
      resolution = SettingGetGlobal_f(G, cSetting_gaussian_resolution);
    if(resolution < 1.0 ) 
      resolution = 1.0F;
    blur_factor = 2.0F / resolution;    
    /* a gaussian_resolution of 2.0 is considered perfect ? Hmm...where is this from??? */

  }

  if(b_adjust > 500.0)
    b_adjust = 500.0;           /* constrain to be somewhat reasonable */

  for(a = 0; a < 256; a++) {
    sf[a][0] = -1.0;
  }

  sf[cAN_H][0] = 0.493002;
  sf[cAN_H][1] = 10.510900;
  sf[cAN_H][2] = 0.322912;
  sf[cAN_H][3] = 26.125700;
  sf[cAN_H][4] = 0.140191;
  sf[cAN_H][5] = 3.142360;
  sf[cAN_H][6] = 0.040810;
  sf[cAN_H][7] = 57.799698;
  sf[cAN_H][8] = 0.003038;
  sf[cAN_H][9] = 0.0;

  /* LP currently using scattering factors of carbon 
     (Roche Pocket viewer relies upon this behavior) */

  sf[cAN_LP][0] = 2.310000;
  sf[cAN_LP][1] = 20.843899;
  sf[cAN_LP][2] = 1.020000;
  sf[cAN_LP][3] = 10.207500;
  sf[cAN_LP][4] = 1.588600;
  sf[cAN_LP][5] = 0.568700;
  sf[cAN_LP][6] = 0.865000;
  sf[cAN_LP][7] = 51.651199;
  sf[cAN_LP][8] = 0.215600;
  sf[cAN_LP][9] = 0.0;

  sf[cAN_C][0] = 2.310000;
  sf[cAN_C][1] = 20.843899;
  sf[cAN_C][2] = 1.020000;
  sf[cAN_C][3] = 10.207500;
  sf[cAN_C][4] = 1.588600;
  sf[cAN_C][5] = 0.568700;
  sf[cAN_C][6] = 0.865000;
  sf[cAN_C][7] = 51.651199;
  sf[cAN_C][8] = 0.215600;
  sf[cAN_C][9] = 0.0;

  sf[cAN_O][0] = 3.048500;
  sf[cAN_O][1] = 13.277100;
  sf[cAN_O][2] = 2.286800;
  sf[cAN_O][3] = 5.701100;
  sf[cAN_O][4] = 1.546300;
  sf[cAN_O][5] = 0.323900;
  sf[cAN_O][6] = 0.867000;
  sf[cAN_O][7] = 32.908897;
  sf[cAN_O][8] = 0.250800;
  sf[cAN_O][9] = 0.0;

  sf[cAN_N][0] = 12.212600;
  sf[cAN_N][1] = 0.005700;
  sf[cAN_N][2] = 3.132200;
  sf[cAN_N][3] = 9.893300;
  sf[cAN_N][4] = 2.012500;
  sf[cAN_N][5] = 28.997499;
  sf[cAN_N][6] = 1.166300;
  sf[cAN_N][7] = 0.582600;
  sf[cAN_N][8] = -11.528999;
  sf[cAN_N][9] = 0.0;

  sf[cAN_S][0] = 6.905300;
  sf[cAN_S][1] = 1.467900;
  sf[cAN_S][2] = 5.203400;
  sf[cAN_S][3] = 22.215099;
  sf[cAN_S][4] = 1.437900;
  sf[cAN_S][5] = 0.253600;
  sf[cAN_S][6] = 1.586300;
  sf[cAN_S][7] = 56.172001;
  sf[cAN_S][8] = 0.866900;
  sf[cAN_S][9] = 0.0;

  sf[cAN_Cl][0] = 11.460400;
  sf[cAN_Cl][1] = 0.010400;
  sf[cAN_Cl][2] = 7.196400;
  sf[cAN_Cl][3] = 1.166200;
  sf[cAN_Cl][4] = 6.255600;
  sf[cAN_Cl][5] = 18.519400;
  sf[cAN_Cl][6] = 1.645500;
  sf[cAN_Cl][7] = 47.778400;
  sf[cAN_Cl][8] = 0.866900;
  sf[cAN_Cl][9] = 0.0;

  sf[cAN_Br][0] = 17.178900;
  sf[cAN_Br][1] = 2.172300;
  sf[cAN_Br][2] = 5.235800;
  sf[cAN_Br][3] = 16.579599;
  sf[cAN_Br][4] = 5.637700;
  sf[cAN_Br][5] = 0.260900;
  sf[cAN_Br][6] = 3.985100;
  sf[cAN_Br][7] = 41.432800;
  sf[cAN_Br][8] = 2.955700;
  sf[cAN_Br][9] = 0.0;

  sf[cAN_I][0] = 20.147200;
  sf[cAN_I][1] = 4.347000;
  sf[cAN_I][2] = 18.994900;
  sf[cAN_I][3] = 0.381400;
  sf[cAN_I][4] = 7.513800;
  sf[cAN_I][5] = 27.765999;
  sf[cAN_I][6] = 2.273500;
  sf[cAN_I][7] = 66.877602;
  sf[cAN_I][8] = 4.071200;
  sf[cAN_I][9] = 0.0;

  sf[cAN_F][0] = 3.539200;
  sf[cAN_F][1] = 10.282499;
  sf[cAN_F][2] = 2.641200;
  sf[cAN_F][3] = 4.294400;
  sf[cAN_F][4] = 1.517000;
  sf[cAN_F][5] = 0.261500;
  sf[cAN_F][6] = 1.024300;
  sf[cAN_F][7] = 26.147600;
  sf[cAN_F][8] = 0.277600;
  sf[cAN_F][9] = 0.0;

  sf[cAN_K][0] = 8.218599;
  sf[cAN_K][1] = 12.794900;
  sf[cAN_K][2] = 7.439800;
  sf[cAN_K][3] = 0.774800;
  sf[cAN_K][4] = 1.051900;
  sf[cAN_K][5] = 213.186996;
  sf[cAN_K][6] = 0.865900;
  sf[cAN_K][7] = 41.684097;
  sf[cAN_K][8] = 1.422800;
  sf[cAN_K][9] = 0.0;

  sf[cAN_Mg][0] = 5.420400;
  sf[cAN_Mg][1] = 2.827500;
  sf[cAN_Mg][2] = 2.173500;
  sf[cAN_Mg][3] = 79.261101;
  sf[cAN_Mg][4] = 1.226900;
  sf[cAN_Mg][5] = 0.380800;
  sf[cAN_Mg][6] = 2.307300;
  sf[cAN_Mg][7] = 7.193700;
  sf[cAN_Mg][8] = 0.858400;
  sf[cAN_Mg][9] = 0.0;

  sf[cAN_Na][0] = 4.762600;
  sf[cAN_Na][1] = 3.285000;
  sf[cAN_Na][2] = 3.173600;
  sf[cAN_Na][3] = 8.842199;
  sf[cAN_Na][4] = 1.267400;
  sf[cAN_Na][5] = 0.313600;
  sf[cAN_Na][6] = 1.112800;
  sf[cAN_Na][7] = 129.423996;
  sf[cAN_Na][8] = 0.676000;
  sf[cAN_Na][9] = 0.0;

  sf[cAN_P][0] = 6.434500;
  sf[cAN_P][1] = 1.906700;
  sf[cAN_P][2] = 4.179100;
  sf[cAN_P][3] = 27.157000;
  sf[cAN_P][4] = 1.780000;
  sf[cAN_P][5] = 0.526000;
  sf[cAN_P][6] = 1.490800;
  sf[cAN_P][7] = 68.164497;
  sf[cAN_P][8] = 1.114900;
  sf[cAN_P][9] = 0.0;

  sf[cAN_Zn][0] = 14.074300;
  sf[cAN_Zn][1] = 3.265500;
  sf[cAN_Zn][2] = 7.031800;
  sf[cAN_Zn][3] = 0.233300;
  sf[cAN_Zn][4] = 5.162500;
  sf[cAN_Zn][5] = 10.316299;
  sf[cAN_Zn][6] = 2.410000;
  sf[cAN_Zn][7] = 58.709702;
  sf[cAN_Zn][8] = 1.304100;
  sf[cAN_Zn][9] = 0.0;

  sf[cAN_Ca][0] = 8.626600;
  sf[cAN_Ca][1] = 10.442100;
  sf[cAN_Ca][2] = 7.387300;
  sf[cAN_Ca][3] = 0.659900;
  sf[cAN_Ca][4] = 1.589900;
  sf[cAN_Ca][5] = 85.748398;
  sf[cAN_Ca][6] = 1.021100;
  sf[cAN_Ca][7] = 178.436996;
  sf[cAN_Ca][8] = 1.375100;
  sf[cAN_Ca][9] = 0.0;

  sf[cAN_Cu][0] = 13.337999;
  sf[cAN_Cu][1] = 3.582800;
  sf[cAN_Cu][2] = 7.167600;
  sf[cAN_Cu][3] = 0.247000;
  sf[cAN_Cu][4] = 5.615800;
  sf[cAN_Cu][5] = 11.396600;
  sf[cAN_Cu][6] = 1.673500;
  sf[cAN_Cu][7] = 64.812599;
  sf[cAN_Cu][8] = 1.191000;
  sf[cAN_Cu][9] = 0.0;

  sf[cAN_Fe][0] = 11.769500;
  sf[cAN_Fe][1] = 4.761100;
  sf[cAN_Fe][2] = 7.357300;
  sf[cAN_Fe][3] = 0.307200;
  sf[cAN_Fe][4] = 3.522200;
  sf[cAN_Fe][5] = 15.353500;
  sf[cAN_Fe][6] = 2.304500;
  sf[cAN_Fe][7] = 76.880501;
  sf[cAN_Fe][8] = 1.036900;
  sf[cAN_Fe][9] = 0.0;

  sf[cAN_Se][0] = 17.000599;
  sf[cAN_Se][1] = 2.409800;
  sf[cAN_Se][2] = 5.819600;
  sf[cAN_Se][3] = 0.272600;
  sf[cAN_Se][4] = 3.973100;
  sf[cAN_Se][5] = 15.237200;
  sf[cAN_Se][6] = 4.354300;
  sf[cAN_Se][7] = 43.816299;
  sf[cAN_Se][8] = 2.840900;
  sf[cAN_Se][9] = 0.0;

  buffer += MAX_VDW;
  c = 0;
  n1 = 0;
  if(state >= cSelectorUpdateTableEffectiveStates) {
    SelectorUpdateTable(G, state, -1);
  } else {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  }
  for(a = 0; a < I->NAtom; a++) {
    at = I->Table[a].atom;
    obj = I->Obj[I->Table[a].model];
    s = obj->AtomInfo[at].selEntry;
    if(SelectorIsMember(G, s, sele1)) {
      once_flag = true;
      for(state1 = 0; state1 < obj->NCSet; state1++) {
        if(state < 0)
          once_flag = false;
        if(!once_flag)
          state2 = state1;
        else
          state2 = state;
        if(state2 < obj->NCSet)
          cs = obj->CSet[state2];
        else
          cs = NULL;
        if(cs) {
          idx = cs->atmToIdx(at);
          if(idx >= 0) {
            n1++;
          }
        }
        if(once_flag)
          break;
      }
    }
  }
  point = pymol::malloc<float>(3 * n1);
  sfidx = pymol::malloc<int>(n1);
  b_factor = pymol::malloc<float>(n1);
  occup = pymol::malloc<float>(n1);
  atom_sf = pymol::malloc<AtomSF>(n1);

  if(!quiet) {
    PRINTFB(G, FB_ObjectMap, FB_Details)
      " ObjectMap: Computing Gaussian map for %d atom positions.\n", n1 ENDFB(G);
  }

  n1 = 0;
  fp = point;
  ip = sfidx;
  bf = b_factor;
  oc = occup;
  for(a = 0; a < I->NAtom; a++) {
    at = I->Table[a].atom;
    obj = I->Obj[I->Table[a].model];
    ai = obj->AtomInfo + at;
    s = ai->selEntry;
    if(SelectorIsMember(G, s, sele1)) {
      once_flag = true;
      for(state1 = 0; state1 < obj->NCSet; state1++) {
        if(state < 0)
          once_flag = false;
        if(!once_flag)
          state2 = state1;
        else
          state2 = state;
        if(state2 < obj->NCSet)
          cs = obj->CSet[state2];
        else
          cs = NULL;
        if(cs) {
          if(CoordSetGetAtomVertex(cs, at, fp)) {
            prot = ai->protons;
            if(sf[prot][0] == -1.0F)
              prot = cAN_C;
            bfact = ai->b + (float) b_adjust;
            if(bfact < b_floor)
              bfact = b_floor;
            if((bfact > R_SMALL4) && (ai->q > R_SMALL4)) {
              fp += 3;
              *(ip++) = prot;
              *(bf++) = bfact;
              *(oc++) = ai->q;
              n1++;
            }
          }
        }
        if(once_flag)
          break;
      }
    }
  }

  for(a = 0; a < n1; a++) {
    double *src_sf;

    src_sf = &sf[sfidx[a]][0];
    bfact = b_factor[a];

    for(b = 0; b < 10; b += 2) {
      double sfa, sfb;
      sfa = src_sf[b];
      sfb = src_sf[b + 1];

      atom_sf[a][b] = occup[a] * sfa * pow(sqrt1d(4 * PI / (sfb + bfact)), 3.0);
      atom_sf[a][b + 1] = 4 * PI * PI / (sfb + bfact);

    }

    rcut2 = max6d(0.0,
                  (elim + log(max2d(fabs(atom_sf[a][0]), D_SMALL10))) / atom_sf[a][1],
                  (elim + log(max2d(fabs(atom_sf[a][2]), D_SMALL10))) / atom_sf[a][3],
                  (elim + log(max2d(fabs(atom_sf[a][4]), D_SMALL10))) / atom_sf[a][5],
                  (elim + log(max2d(fabs(atom_sf[a][6]), D_SMALL10))) / atom_sf[a][7],
                  (elim + log(max2d(fabs(atom_sf[a][8]), D_SMALL10))) / atom_sf[a][9]);
    rcut = ((float) sqrt1d(rcut2)) / blur_factor;
    atom_sf[a][10] = rcut;
    if(max_rcut < rcut)
      max_rcut = rcut;
  }

  /* now create and apply voxel map */
  c = 0;
  if(n1) {
    n2 = 0;
    map = MapNew(G, -max_rcut, point, n1, NULL);
    if(map) {
      MapSetupExpress(map);
      sum = 0.0;
      sumsq = 0.0;
      for(a = oMap->Min[0]; a <= oMap->Max[0]; a++) {
        OrthoBusyFast(G, a - oMap->Min[0], oMap->Max[0] - oMap->Min[0] + 1);
        for(b = oMap->Min[1]; b <= oMap->Max[1]; b++) {
          for(c = oMap->Min[2]; c <= oMap->Max[2]; c++) {
            e_val = 0.0;
            v2 = F4Ptr(oMap->Field->points, a, b, c, 0);
            if(MapExclLocus(map, v2, &h, &k, &l)) {
              i = *(MapEStart(map, h, k, l));
              if(i) {
                j = map->EList[i++];
                if(use_max) {
                  float e_partial;
                  while(j >= 0) {
                    d = (float) diff3f(point + 3 * j, v2) * blur_factor;        
                    /* scale up width */
                    sfp = atom_sf[j];
                    if(d < sfp[10]) {
                      d = d * d;
                      if(d < R_SMALL8)
                        d = R_SMALL8;
                      e_partial = (float) ((sfp[0] * exp(-sfp[1] * d))
                                           + (sfp[2] * exp(-sfp[3] * d))
                                           + (sfp[4] * exp(-sfp[5] * d))
                                           + (sfp[6] * exp(-sfp[7] * d))
                                           + (sfp[8] * exp(-sfp[9] * d))) * blur_factor;       
                      /* scale down intensity */
                      if(e_partial > e_val)
                        e_val = e_partial;
                    }
                    j = map->EList[i++];
                  }
                } else {
                  while(j >= 0) {
                    d = (float) diff3f(point + 3 * j, v2) * blur_factor;       
                    /* scale up width */
                    sfp = atom_sf[j];
                    if(d < sfp[10]) {
                      d = d * d;
                      if(d < R_SMALL8)
                        d = R_SMALL8;
                      e_val += (float) ((sfp[0] * exp(-sfp[1] * d))
                                        + (sfp[2] * exp(-sfp[3] * d))
                                        + (sfp[4] * exp(-sfp[5] * d))
                                        + (sfp[6] * exp(-sfp[7] * d))
                                        + (sfp[8] * exp(-sfp[9] * d))) * blur_factor;  
                      /* scale down intensity */
                    }
                    j = map->EList[i++];
                  }
                }
              }
            }
            F3(oMap->Field->data, a, b, c) = e_val;
            sum += e_val;
            sumsq += (e_val * e_val);
            n2++;
          }
        }
      }
      mean = (float) (sum / n2);
      stdev = (float) sqrt1d((sumsq - (sum * sum / n2)) / (n2 - 1));
      if(normalize) {

        if(!quiet) {
          PRINTFB(G, FB_ObjectMap, FB_Details)
            " ObjectMap: Normalizing: mean = %8.6f & stdev = %8.6f.\n", mean, stdev
            ENDFB(G);
        }

        if(stdev < R_SMALL8)
          stdev = R_SMALL8;

        for(a = oMap->Min[0]; a <= oMap->Max[0]; a++) {
          for(b = oMap->Min[1]; b <= oMap->Max[1]; b++) {
            for(c = oMap->Min[2]; c <= oMap->Max[2]; c++) {
              fp = F3Ptr(oMap->Field->data, a, b, c);

              *fp = (*fp - mean) / stdev;
            }
          }
        }
      } else {
        if(!quiet) {
          PRINTFB(G, FB_ObjectMap, FB_Details)
            " ObjectMap: Not normalizing: mean = %8.6f and stdev = %8.6f.\n",
            mean, stdev ENDFB(G);
        }
      }
      oMap->Active = true;
      MapFree(map);
    }
  }
  FreeP(point);
  FreeP(sfidx);
  FreeP(atom_sf);
  FreeP(b_factor);
  FreeP(occup);
  return (c);
}


/*========================================================================*/
int SelectorMapCoulomb(PyMOLGlobals * G, int sele1, ObjectMapState * oMap,
                       float cutoff, int state, int neutral, int shift, float shift_power)
{
  CSelector *I = G->Selector;
  MapType *map;
  float *v2;
  int a, b, c, j, i;
  int h, k, l;
  int at;
  int s, idx;
  AtomInfoType *ai;
  ObjectMolecule *obj;
  CoordSet *cs;
  int state1, state2;
  int once_flag;
  int n_at = 0;
  double tot_charge = 0.0;
  float *point = NULL;
  float *charge = NULL;
  int n_point = 0;
  int n_occur;
  float *v0, *v1;
  float c_factor = 1.0F;
  float cutoff_to_power = 1.0F;
  const float _1 = 1.0F;

  if(shift)
    cutoff_to_power = (float) pow(cutoff, shift_power);

  c_factor = SettingGetGlobal_f(G, cSetting_coulomb_units_factor) /
             SettingGetGlobal_f(G, cSetting_coulomb_dielectric);

  c = 0;
  SelectorUpdateTable(G, state, -1);

  point = VLAlloc(float, I->NAtom * 3);
  charge = VLAlloc(float, I->NAtom);

  /* first count # of times each atom appears */

  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    at = I->Table[a].atom;
    obj = I->Obj[I->Table[a].model];
    s = obj->AtomInfo[at].selEntry;
    ai = obj->AtomInfo + at;
    if(SelectorIsMember(G, s, sele1)) {
      n_occur = 0;
      /* count */
      once_flag = true;
      for(state2 = 0; state2 < obj->NCSet; state2++) {
        if(state < 0)
          once_flag = false;
        if(!once_flag)
          state1 = state2;
        else
          state1 = state;
        if(state1 < obj->NCSet)
          cs = obj->CSet[state1];
        else
          cs = NULL;
        if(cs) {
          idx = cs->atmToIdx(at);
          if(idx >= 0) {
            n_occur++;
            n_at++;
          }
        }
        if(once_flag)
          break;
      }
      /* copy */
      if(n_occur) {
        once_flag = true;
        for(state2 = 0; state2 < obj->NCSet; state2++) {
          if(state < 0)
            once_flag = false;
          if(!once_flag)
            state1 = state2;
          else
            state1 = state;
          if(state1 < obj->NCSet)
            cs = obj->CSet[state1];
          else
            cs = NULL;
          if(cs) {
            idx = cs->atmToIdx(at);
            if(idx >= 0) {
              VLACheck(point, float, 3 * n_point + 2);
              VLACheck(charge, float, n_point);
              v0 = cs->Coord + (3 * idx);
              v1 = point + 3 * n_point;
              copy3f(v0, v1);
              charge[n_point] = ai->partialCharge * ai->q / n_occur;

              tot_charge += charge[n_point];
              n_point++;
            }
          }
          if(once_flag)
            break;
        }
      }
    }
  }

  PRINTFB(G, FB_Selector, FB_Details)
    " %s: Total charge is %0.3f for %d points (%d atoms).\n", __func__, tot_charge,
    n_point, n_at ENDFB(G);

  if(neutral && (fabs(tot_charge) > R_SMALL4)) {
    float adjust;

    adjust = (float) (-tot_charge / n_point);

    for(a = 0; a < n_point; a++) {
      charge[a] += adjust;
    }

    PRINTFB(G, FB_Selector, FB_Details)
      " %s: Setting net charge to zero...\n", __func__ ENDFB(G);

  }

  for(a = 0; a < n_point; a++) {        /* premultiply c_factor by charges */
    charge[a] *= c_factor;
  }

  /* now create and apply voxel map */
  c = 0;
  if(n_point) {
    int *min = oMap->Min;
    int *max = oMap->Max;
    CField *data = oMap->Field->data;
    CField *points = oMap->Field->points;
    float dist;

    if(cutoff > 0.0F) {         /* we are using a cutoff */
      if(shift) {
        PRINTFB(G, FB_Selector, FB_Details)
          " %s: Evaluating local Coulomb potential for grid (shift=%0.2f)...\n", __func__,
          cutoff ENDFB(G);
      } else {
        PRINTFB(G, FB_Selector, FB_Details)
          " %s: Evaluating Coulomb potential for grid (cutoff=%0.2f)...\n", __func__,
          cutoff ENDFB(G);
      }

      map = MapNew(G, -(cutoff), point, n_point, NULL);
      if(map) {
        int *elist;
        float dx, dy, dz;
        float cut = cutoff;
        float cut2 = cutoff * cutoff;

        MapSetupExpress(map);
        elist = map->EList;
        for(a = min[0]; a <= max[0]; a++) {
          OrthoBusyFast(G, a - min[0], max[0] - min[0] + 1);
          for(b = min[1]; b <= max[1]; b++) {
            for(c = min[2]; c <= max[2]; c++) {
              F3(data, a, b, c) = 0.0F;
              v2 = F4Ptr(points, a, b, c, 0);

              if(MapExclLocus(map, v2, &h, &k, &l)) {
                i = *(MapEStart(map, h, k, l));
                if(i) {
                  j = elist[i++];
                  while(j >= 0) {
                    v1 = point + 3 * j;
                    while(1) {

                      dx = v1[0] - v2[0];
                      dy = v1[1] - v2[1];
                      dx = (float) fabs(dx);
                      dy = (float) fabs(dy);
                      if(dx > cut)
                        break;
                      dz = v1[2] - v2[2];
                      dx = dx * dx;
                      if(dy > cut)
                        break;
                      dz = (float) fabs(dz);
                      dy = dy * dy;
                      if(dz > cut)
                        break;
                      dx = dx + dy;
                      dz = dz * dz;
                      if(dx > cut2)
                        break;
                      dy = dx + dz;
                      if(dy > cut2)
                        break;
                      dist = (float) sqrt1f(dy);

                      if(dist > R_SMALL4) {
                        if(shift) {
                          if(dist < cutoff) {
                            F3(data, a, b, c) += (charge[j] / dist) *
                              (_1 - (float) pow(dist, shift_power) / cutoff_to_power);
                          }
                        } else {
                          F3(data, a, b, c) += charge[j] / dist;
                        }
                      }

                      break;
                    }
                    j = map->EList[i++];
                  }
                }
              }
            }
          }
        }
        MapFree(map);
      }
    } else {
      float *v1;
      PRINTFB(G, FB_Selector, FB_Details)
        " %s: Evaluating Coulomb potential for grid (no cutoff)...\n", __func__
        ENDFB(G);

      for(a = min[0]; a <= max[0]; a++) {
        OrthoBusyFast(G, a - min[0], max[0] - min[0] + 1);
        for(b = min[1]; b <= max[1]; b++) {
          for(c = min[2]; c <= max[2]; c++) {
            F3(data, a, b, c) = 0.0F;
            v1 = point;
            v2 = F4Ptr(points, a, b, c, 0);
            for(j = 0; j < n_point; j++) {
              dist = (float) diff3f(v1, v2);
              v1 += 3;
              if(dist > R_SMALL4) {
                F3(data, a, b, c) += charge[j] / dist;
              }
            }
          }
        }
      }
    }
    oMap->Active = true;
  }
  VLAFreeP(point);
  VLAFreeP(charge);
  return (1);
}


/*========================================================================*/
int SelectorAssignAtomTypes(PyMOLGlobals * G, int sele, int state, int quiet, int format)
{
#ifndef NO_MMLIBS
  CSelector *I = G->Selector;
  int ok = true;

  SelectorUpdateTable(G, state, -1);

  if(ok) {
    ObjectMolecule *prevobj = NULL;
    int a;
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      int at = I->Table[a].atom;
      ObjectMolecule *obj = I->Obj[I->Table[a].model];
      int s = obj->AtomInfo[at].selEntry;
      I->Table[a].index = 0;
      if(SelectorIsMember(G, s, sele)) {
	ObjectMoleculeInvalidateAtomType(obj, state);
      }
    }
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      int at = I->Table[a].atom;
      ObjectMolecule *obj = I->Obj[I->Table[a].model];
      int s = obj->AtomInfo[at].selEntry;
      I->Table[a].index = 0;
      if(obj != prevobj && SelectorIsMember(G, s, sele)) {
	ObjectMoleculeUpdateAtomTypeInfoForState(G, obj, state, 1, format);
          prevobj = obj;
      }
    }
  }
  return 1;
#else
  if (format != 1) {
    PRINTFB(G, FB_Selector, FB_Errors)
      " Error: assign_atom_types only supports format='mol2'\n" ENDFB(G);
    return 0;
  }

  SelectorUpdateTable(G, state, -1);

  ObjectMolecule *obj = NULL;

  for (SeleAtomIterator iter(G, sele); iter.next();) {
    if (obj != iter.obj) {
      obj = iter.obj;
      ObjectMoleculeVerifyChemistry(obj, state);
    }

    LexAssign(G, iter.getAtomInfo()->textType,
        getMOL2Type(obj, iter.getAtm()));
  }
  return 1;
#endif
}

/*========================================================================*/
/*
 * Get selection coordinates as Nx3 numpy array. Equivalent to
 *
 * PyMOL> coords = []
 * PyMOL> cmd.iterate_state(state, sele, 'coords.append([x,y,z])')
 * PyMOL> coords = numpy.array(coords)
 */
PyObject *SelectorGetCoordsAsNumPy(PyMOLGlobals * G, int sele, int state)
{
#ifndef _PYMOL_NUMPY
  printf("No numpy support\n");
  return NULL;
#else

  double matrix[16];
  double *matrix_ptr = NULL;
  float *v_ptr, v_tmp[3], *dataptr;
  int i, nAtom = 0;
  int typenum = -1;
  const int base_size = sizeof(float);
  SeleCoordIterator iter(G, sele, state);
  CoordSet *mat_cs = NULL;
  PyObject *result = NULL;
  npy_intp dims[2] = {0, 3};

  for(iter.reset(); iter.next();)
    nAtom++;

  if(!nAtom)
    return NULL;

  dims[0] = nAtom;

  import_array1(NULL);

  switch(base_size) {
    case 4: typenum = NPY_FLOAT32; break;
    case 8: typenum = NPY_FLOAT64; break;
  }

  if(typenum == -1) {
    printf("error: no typenum for float size %d\n", base_size);
    return NULL;
  }

  result = PyArray_SimpleNew(2, dims, typenum);
  dataptr = (float*) PyArray_DATA((PyArrayObject *)result);

  for(i = 0, iter.reset(); iter.next(); i++) {
    v_ptr = iter.getCoord();

    if(mat_cs != iter.cs) {
      /* compute the effective matrix for output coordinates */
      matrix_ptr = ObjectGetTotalMatrix(iter.obj, state, false, matrix) ? matrix : NULL;
      mat_cs = iter.cs;
    }

    if(matrix_ptr) {
      transform44d3f(matrix_ptr, v_ptr, v_tmp);
      v_ptr = v_tmp;
    }

    copy3f(v_ptr, dataptr + i * 3);
  }

  return result;
#endif
}

/*========================================================================*/
/*
 * Load coordinates from a Nx3 sequence into the given selection.
 * Most efficiant with numpy arrays. Equivalent to
 *
 * PyMOL> coords = iter(coords)
 * PyMOL> cmd.alter_state(state, sele, '(x,y,z) = coords.next()')
 */
int SelectorLoadCoords(PyMOLGlobals * G, PyObject * coords, int sele, int state)
{
#ifdef _PYMOL_NOPY
  return false;
#else

  double matrix[16];
  double *matrix_ptr = NULL;
  float v_xyz[3];
  int a, b, nAtom = 0, itemsize;
  SeleCoordIterator iter(G, sele, state);
  CoordSet *mat_cs = NULL;
  PyObject *v, *w;
  bool is_np_array = false;
  void * ptr;

  if(!PySequence_Check(coords)) {
    ErrMessage(G, "LoadCoords", "passed argument is not a sequence");
    ok_raise(1);
  }

  // atom count in selection
  while(iter.next())
    nAtom++;

  // sequence length must match atom count
  if(nAtom != PySequence_Size(coords)) {
    ErrMessage(G, "LoadCoords", "atom count mismatch");
    return false;
  }

  // detect numpy arrays, allows faster data access (see below)
#ifdef _PYMOL_NUMPY
  import_array1(false);

  if(PyArray_Check(coords)) {
    if(PyArray_NDIM((PyArrayObject *)coords) != 2 ||
        PyArray_DIM((PyArrayObject *)coords, 1) != 3) {
      ErrMessage(G, "LoadCoords", "numpy array shape mismatch");
      return false;
    }
    itemsize = PyArray_ITEMSIZE((PyArrayObject *)coords);
    switch(itemsize) {
      case sizeof(double):
      case sizeof(float):
        is_np_array = true;
        break;
      default:
        PRINTFB(G, FB_Selector, FB_Warnings)
          " LoadCoords-Warning: numpy array with unsupported dtype\n" ENDFB(G);
    }
  }
#endif

  for(a = 0, iter.reset(); iter.next(); a++) {
    // get xyz from python
    if (is_np_array) {
      // fast implementation for numpy arrays only
#ifdef _PYMOL_NUMPY
      for(b = 0; b < 3; b++) {
        ptr = PyArray_GETPTR2((PyArrayObject *)coords, a, b);

        switch(itemsize) {
          case sizeof(double):
            v_xyz[b] = (float) *((double*)ptr);
            break;
          default:
            v_xyz[b] = *((float*)ptr);
        }
      }
#endif
    } else {
      // general implementation for any 2d sequence
      v = PySequence_ITEM(coords, a);

      // get xyz from python sequence item
      for(b = 0; b < 3; b++) {
        if(!(w = PySequence_GetItem(v, b)))
          break;

        v_xyz[b] = (float) PyFloat_AsDouble(w);
        Py_DECREF(w);
      }

      Py_DECREF(v);
    }

    ok_assert(2, !PyErr_Occurred());

    // coord set specific stuff
    if(mat_cs != iter.cs) {
      // update matrix
      matrix_ptr = ObjectGetTotalMatrix(iter.obj, state, false, matrix) ? matrix : NULL;
      mat_cs = iter.cs;

      // invalidate reps
      iter.cs->invalidateRep(cRepAll, cRepInvRep);
    }

    // handle matrix
    if(matrix_ptr) {
      inverse_transform44d3f(matrix_ptr, v_xyz, v_xyz);
    }

    // copy coordinates
    copy3f(v_xyz, iter.getCoord());
  }

  return true;

  // error handling
ok_except2:
  PyErr_Print();
ok_except1:
  ErrMessage(G, "LoadCoords", "failed");
  return false;
#endif
}

/*========================================================================*/
void SelectorUpdateCmd(PyMOLGlobals * G, int sele0, int sele1, int sta0, int sta1,
                       int matchmaker, int quiet)
{
  CSelector *I = G->Selector;
  int a, b;
  int at0 = 0, at1;
  int *vla0 = NULL;
  int *vla1 = NULL;
  int c0 = 0, c1 = 0;
  int i0 = 0, i1;
  ObjectMolecule *obj0 = NULL, *obj1;
  CoordSet *cs0;
  const CoordSet *cs1;
  int matched_flag;
  int b_start;
  int ccc = 0;

  bool ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);
  bool ignore_case_chain = SettingGetGlobal_b(G, cSetting_ignore_case_chain);

  PRINTFD(G, FB_Selector)
    " %s-Debug: entered sta0 %d sta1 %d", __func__, sta0, sta1 ENDFD;

  // either both or none must be "all states"
  if (sta0 != sta1) {
    if (sta0 == cSelectorUpdateTableAllStates) {
      sta0 = sta1;
    } else if (sta1 == cSelectorUpdateTableAllStates) {
      sta1 = sta0;
    }
  }

  if((sta0 < 0) || (sta1 < 0) || (sta0 != sta1)) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, sta0, -1);
  }

  vla0 = SelectorGetIndexVLA(G, sele0);
  vla1 = SelectorGetIndexVLA(G, sele1);

  if (vla0 && vla1) {
    c0 = VLAGetSize(vla0);
    c1 = VLAGetSize(vla1);
  }

  if (c0 < 1 || c1 < 1)
    ErrMessage(G, "Update", "no coordinates updated.");
  else {

    b = 0;
    for(a = 0; a < c1; a++) {   /* iterate over source atoms */
      /* NOTE, this algorithm is N^2 and slow in the worst case...
         however the best case (N) is quite common, especially when merging 
         files written out of PyMOL */

      i1 = vla1[a];
      at1 = I->Table[i1].atom;
      obj1 = I->Obj[I->Table[i1].model];
      matched_flag = false;

      switch (matchmaker) {
      case 0:                 
        /* simply assume that atoms are stored in PyMOL in the identical order, one for one */
        if(b < c0) {
          i0 = vla0[b];
          at0 = I->Table[i0].atom;
          obj0 = I->Obj[I->Table[i0].model];
          b++;
          matched_flag = true;
        }
        break;
      case 1:                  
        /* match each pair based on atom info */
        b_start = b;
        matched_flag = false;
        while(1) {
          i0 = vla0[b];
          at0 = I->Table[i0].atom;
          obj0 = I->Obj[I->Table[i0].model];
          if(obj0 != obj1) {
            if(AtomInfoMatch(G, obj1->AtomInfo + at1, obj0->AtomInfo + at0, ignore_case, ignore_case_chain)) {
              matched_flag = true;
              break;
            }
          } else if(at0 == at1) {
            matched_flag = true;
            break;
          }
          b++;
          if(b >= c0)
            b = 0;
          if(b == b_start)
            break;
        }
        break;
      case 2:                  /* match based on ID */
        {
          int target = obj1->AtomInfo[at1].id;
          b_start = b;
          matched_flag = false;
          while(1) {
            i0 = vla0[b];
            at0 = I->Table[i0].atom;
            obj0 = I->Obj[I->Table[i0].model];
            if(obj0 != obj1) {
              if(obj0->AtomInfo[at0].id == target) {
                matched_flag = true;
                break;
              }
            } else if(at0 == at1) {
              matched_flag = true;
              break;
            }
            b++;
            if(b >= c0)
              b = 0;
            if(b == b_start)
              break;
          }
        }
        break;
      case 3:                  /* match based on rank */
        {
          int target = obj1->AtomInfo[at1].rank;
          b_start = b;
          matched_flag = false;
          while(1) {
            i0 = vla0[b];
            at0 = I->Table[i0].atom;
            obj0 = I->Obj[I->Table[i0].model];
            if(obj0 != obj1) {
              if(obj0->AtomInfo[at0].rank == target) {
                matched_flag = true;
                break;
              }
            } else if(at0 == at1) {
              matched_flag = true;
            }
            b++;
            if(b >= c0)
              b = 0;
            if(b == b_start)
              break;
          }
        }
        break;
      case 4:                  /* match based on index */
        {
          b_start = b;
          matched_flag = false;
          while(1) {
            i0 = vla0[b];
            at0 = I->Table[i0].atom;
            obj0 = I->Obj[I->Table[i0].model];
            if(obj0 != obj1) {
              if(at0 == at1) {
                matched_flag = true;
                break;
              }
            } else if(at0 == at1) {
              matched_flag = true;
              break;
            }
            b++;
            if(b >= c0)
              b = 0;
            if(b == b_start)
              break;
          }
        }
        break;
      }

      if(matched_flag) {        /* atom matched, so copy coordinates */
        ccc++;

        StateIterator iter0(G, obj0->Setting, sta0, obj0->NCSet);
        StateIterator iter1(G, obj1->Setting, sta1, obj1->NCSet);

        while (iter0.next() && iter1.next()) {
          cs0 = obj0->CSet[iter0.state];
          cs1 = obj1->CSet[iter1.state];
          if (cs1 && cs0) {
            int idx0 = cs0->atmToIdx(at0);
            int idx1 = cs1->atmToIdx(at1);
            if (idx0 >= 0 && idx1 >= 0) {
              copy3f(cs1->coordPtr(idx1), cs0->coordPtr(idx0));
            }
          }
        }
      }
    }
    obj0 = NULL;

    {
      ObjectMolecule **objs = SelectorGetObjectMoleculeVLA(G, sele0);
      int sz = VLAGetSize(objs);
      for(b = 0; b < sz; b++) {
	objs[b]->invalidate(cRepAll, cRepInvCoord, -1);
      }
      VLAFree(objs);
    }
    SceneChanged(G);
    if(!quiet) {
      PRINTFB(G, FB_Selector, FB_Actions)
        " Update: coordinates updated for %d atoms.\n", ccc ENDFB(G);

    }
  }
  VLAFreeP(vla0);
  VLAFreeP(vla1);
}


/*========================================================================*/

int SelectorCreateObjectMolecule(PyMOLGlobals * G, int sele, const char *name,
                                 int target, int source, int discrete,
                                 int zoom, int quiet, int singletons, int copy_properties)
{
  CSelector *I = G->Selector;
  int ok = true;
  int a, b, a2, b1, b2, c, d, s, at;
  const BondType *ii1;
  int nBond = 0;
  int nCSet, nAtom, ts;
  int isNew;
  CoordSet *cs = NULL;
  CoordSet *cs1, *cs2;
  ObjectMolecule *obj;
  CObject *ob;
  ObjectMolecule *targ = NULL;
  ObjectMolecule *info_src = NULL;
  int static_singletons = SettingGetGlobal_b(G, cSetting_static_singletons);

  if(singletons < 0)
    singletons = static_singletons;

  ob = ExecutiveFindObjectByName(G, name);
  if(ob)
    if(ob->type == cObjectMolecule)
      targ = (ObjectMolecule *) ob;

  c = 0;
    SelectorUpdateTable(G, source, -1);

  if(!targ) {
    isNew = true;
    if(discrete < 0)
      discrete = SelectorIsSelectionDiscrete(G, sele, false);
    targ = new ObjectMolecule(G, discrete);
    targ->Bond = pymol::vla<BondType>(1);
    {
      /* copy object color of previous object (if any) */
      ObjectMolecule *singleObj = NULL;
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        at = I->Table[a].atom;
        I->Table[a].index = -1;
        obj = I->Obj[I->Table[a].model];
        s = obj->AtomInfo[at].selEntry;
        if(SelectorIsMember(G, s, sele)) {
          if(!singleObj)
            singleObj = obj;
          else if(singleObj && (obj != singleObj)) {
            singleObj = NULL;
            break;
          }
        }
      }
      if(singleObj)
        targ->Color = singleObj->Color;
      /* should also consider copying lots of other stuff from the source object ... */
    }
  } else {
    isNew = false;
  }

  for(a = cNDummyAtoms; a < I->NAtom; a++) {
    at = I->Table[a].atom;
    I->Table[a].index = -1;
    obj = I->Obj[I->Table[a].model];
    s = obj->AtomInfo[at].selEntry;
    if(SelectorIsMember(G, s, sele)) {
      I->Table[a].index = c;    /* Mark records as to which atom they'll be */
      c++;
      if(!info_src)
        info_src = obj;
    }
  }
  if(isNew && info_src) {       /* copy symmetry information, etc. */
    if (targ->Symmetry != nullptr) {
      targ->Symmetry = new CSymmetry(*info_src->Symmetry);
    }
  }
  nAtom = c;

  nBond = 0;
  auto bond = pymol::vla<BondType>(nAtom * 4);
  for(a = cNDummyModels; a < I->NModel; a++) {  /* find bonds wholly contained in the selection */
    obj = I->Obj[a];
    ii1 = obj->Bond;
    for(b = 0; b < obj->NBond; b++) {
      b1 = SelectorGetObjAtmOffset(I, obj, ii1->index[0]);
      b2 = SelectorGetObjAtmOffset(I, obj, ii1->index[1]);
      if((b1 >= 0) && (b2 >= 0)) {
        if((I->Table[b1].index >= 0) && (I->Table[b2].index >= 0)) {
          BondType* dst_bond = bond.check(nBond);
          {
            AtomInfoBondCopy(G, ii1, dst_bond);
            dst_bond->index[0] = I->Table[b1].index;    /* store what will be the new index */
            dst_bond->index[1] = I->Table[b2].index;
            /*            printf("Selector-DEBUG %d %d\n",dst_bond->index[0],dst_bond->index[1]); */
            nBond++;
          }
        }
      }
      ii1++;
    }
  }

  pymol::vla<AtomInfoType> atInfo(nAtom);
  /* copy the atom info records and create new zero-based IDs */
  c = 0;
  {
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      if(I->Table[a].index >= 0) {
        obj = I->Obj[I->Table[a].model];
        at = I->Table[a].atom;
        VLACheck(atInfo, AtomInfoType, c);
        AtomInfoCopy(G, obj->AtomInfo + at, atInfo + c);
        c++;
      }
    }
  }

  cs = CoordSetNew(G);          /* set up a dummy coordinate set for the merge xref */
  cs->NIndex = nAtom;
  cs->enumIndices();
  cs->TmpBond = std::move(bond);           /* load up the bonds */
  cs->NTmpBond = nBond;

  /*  printf("Selector-DEBUG nAtom %d\n",nAtom); */
  ObjectMoleculeMerge(targ, std::move(atInfo), cs, false, cAIC_AllMask, true);     /* will free atInfo */
  /* cs->IdxToAtm will now have the reverse mapping from the new subset
     to the new merged molecule */

  ObjectMoleculeExtendIndices(targ, -1);
  ObjectMoleculeUpdateIDNumbers(targ);
  ObjectMoleculeUpdateNonbonded(targ);

  if(!isNew) {                  /* recreate selection table */

      SelectorUpdateTable(G, source, -1);

  }

  /* get maximum state index for the selection...note that
     we'll be creating states from 1 up to the maximum required to
     capture atoms in the selection 
   */

  nCSet = 0;

  {
    c = 0;
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      at = I->Table[a].atom;
      I->Table[a].index = -1;
      obj = I->Obj[I->Table[a].model];
      s = obj->AtomInfo[at].selEntry;
      if(SelectorIsMember(G, s, sele)) {
        I->Table[a].index = c;  /* Mark records  */
        if(nCSet < obj->NCSet)
          nCSet = obj->NCSet;
        c++;
      }
    }
  }

  if(c != nAtom)
    ErrFatal(G, "SelectorCreate", "inconsistent selection.");
  /* cs->IdxToAtm now has the relevant indexes for the coordinate transfer */

  for(StateIterator iter(G, NULL, source, nCSet); iter.next();) {
    d = iter.state;
    {
      cs2 = CoordSetNew(G);
      c = 0;
      cs2->Coord = pymol::vla<float>(3 * nAtom);
      cs2->AtmToIdx = pymol::vla<int>(targ->NAtom + 1);
      for(a = 0; a < targ->NAtom; a++)
        cs2->AtmToIdx[a] = -1;
      cs2->NAtIndex = targ->NAtom;
      cs2->IdxToAtm = pymol::vla<int>(nAtom);
      for(a = cNDummyAtoms; a < I->NAtom; a++)  /* any selected atoms in this state? */
        if(I->Table[a].index >= 0) {
          at = I->Table[a].atom;
          obj = I->Obj[I->Table[a].model];
          cs1 = NULL;
          if(d < obj->NCSet) {
            cs1 = obj->CSet[d];
          } else if(singletons && (obj->NCSet == 1)) {
            cs1 = obj->CSet[0];
          }
          if(cs1) {
            if((!cs2->Name[0]) && (cs1->Name[0]))       /* copy the molecule name (if any) */
              strcpy(cs2->Name, cs1->Name);

            if(CoordSetGetAtomVertex(cs1, at, cs2->Coord + c * 3)) {
              a2 = cs->IdxToAtm[I->Table[a].index];     /* actual merged atom index */
              cs2->IdxToAtm[c] = a2;
              cs2->AtmToIdx[a2] = c;
              c++;
            }
          }
        }
      VLASize(cs2->IdxToAtm, int, c);
      VLASize(cs2->Coord, float, c * 3);
      cs2->NIndex = c;
      if(target >= 0) {
        if(source == -1)
          ts = target + d;
        else
          ts = target;
      } else {
        ts = d;
      }
      VLACheck(targ->CSet, CoordSet *, ts);
      if(targ->NCSet <= ts)
        targ->NCSet = ts + 1;
      if(targ->CSet[ts])
        targ->CSet[ts]->fFree();
      targ->CSet[ts] = cs2;
      cs2->Obj = targ;
    }
  }
  if(cs)
    cs->fFree();
  if(targ->DiscreteFlag) {      /* if the new object is discrete, then eliminate the AtmToIdx array */
    for(d = 0; d < targ->NCSet; d++) {
      cs = targ->CSet[d];
      if(cs) {
        if(cs->AtmToIdx) {
          for(a = 0; a < cs->NIndex; a++) {
            b = cs->IdxToAtm[a];
            targ->DiscreteAtmToIdx[b] = a;
            targ->DiscreteCSet[b] = cs;
          }
          cs->AtmToIdx.freeP();
        }
      }
    }
  }
  SceneCountFrames(G);
  if(!quiet) {
    PRINTFB(G, FB_Selector, FB_Details)
      " Selector: found %d atoms.\n", nAtom ENDFB(G);
  }
  if (ok)
    ok &= ObjectMoleculeSort(targ);
  if(isNew) {
    ObjectSetName((CObject *) targ, name);
    ExecutiveManageObject(G, (CObject *) targ, zoom, quiet);
  } else {
    ExecutiveUpdateObjectSelection(G, (CObject *) targ);
  }
  SceneChanged(G);
  return ok;
}


/*========================================================================*/
int SelectorSetName(PyMOLGlobals * G, const char *new_name, const char *old_name)
{
  CSelector *I = G->Selector;
  ov_diff i;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);

  i = SelectGetNameOffset(G, old_name, 1, ignore_case);
  if(i >= 0) {
    SelectorDelName(G, i);
    UtilNCopy(I->Name[i], new_name, WordLength);
    SelectorAddName(G, i);
    return true;
  } else {
    return false;
  }
}


/*========================================================================
 * SelectorIndexByName -- fetch the global selector's ID for sname
 * PARAMS
 *  (string) sname, object name
 * RETURNS
 *   (int) index #, or -1 if not found
 */
int SelectorIndexByName(PyMOLGlobals * G, const char *sname, int ignore_case)
{
  CSelector *I = G->Selector;
  ov_diff i = -1;

  if(sname) {
    if (ignore_case < 0)
      ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);

    while((sname[0] == '%') || (sname[0] == '?'))
      sname++;
    i = SelectGetNameOffset(G, sname, 1, ignore_case);

    if((i >= 0) && (sname[0] != '_')) {  /* don't do checking on internal selections */
      const char *best;
      best = ExecutiveFindBestNameMatch(G, sname);      /* suppress spurious matches
                                                           of selections with non-selections */
      if((best != sname) && (strcmp(best, I->Name[i])))
        i = -1;
    }
    if(i >= 0)
      i = I->Info[i].ID;
  }
  return (i);
}


/*========================================================================*/
static void SelectorPurgeMembers(PyMOLGlobals * G, int sele)
{
  CSelector *I = G->Selector;
  void *iterator = NULL;
  ObjectMolecule *obj = NULL;
  short changed = 0;

  if(I->Member) {
    MemberType *I_Member = I->Member;
    int I_FreeMember = I->FreeMember;

    while(ExecutiveIterateObjectMolecule(G, &obj, &iterator)) {
      if(obj->type == cObjectMolecule) {
        AtomInfoType *ai = obj->AtomInfo.data();
        int a, n_atom = obj->NAtom;
        for(a = 0; a < n_atom; a++) {
          int s = (ai++)->selEntry;
          int l = -1;
          while(s) {
            MemberType *i_member_s = I_Member + s;
            int nxt = i_member_s->next;
            if(i_member_s->selection == sele) {
              if(l > 0)
                I_Member[l].next = i_member_s->next;
              else
                ai[-1].selEntry = i_member_s->next;
	      changed = 1;
              i_member_s->next = I_FreeMember;
              I_FreeMember = s;
            }
            l = s;
            s = nxt;
          }
        }
      }
    }
    I->FreeMember = I_FreeMember;
  }
  if (changed){
    // not sure if this is needed since its in SelectorClean()
    ExecutiveInvalidateSelectionIndicatorsCGO(G);
  }
}


/*========================================================================*/
int SelectorPurgeObjectMembers(PyMOLGlobals * G, ObjectMolecule * obj)
{
  int a = 0;
  int s = 0;
  int nxt;
  short changed = 0;

  CSelector *I = G->Selector;
  if(I->Member) {
    for(a = 0; a < obj->NAtom; a++) {
      s = obj->AtomInfo[a].selEntry;
      while(s) {
        nxt = I->Member[s].next;
        I->Member[s].next = I->FreeMember;
        I->FreeMember = s;
        s = nxt;
      }
      obj->AtomInfo[a].selEntry = 0;
      changed = 1;
    }
  }
  if (changed){
    // not sure if this is needed since its in SelectorClean()
    ExecutiveInvalidateSelectionIndicatorsCGO(G);
  }

  return 1;
}


/*========================================================================*/
void SelectorDelete(PyMOLGlobals * G, const char *sele)


/* should (only) be called by Executive or by Selector, unless the

   named selection has never been registered with the executive 

   (i.e., temporary on-the-fly selection) */
{
  ov_diff n;
  n = SelectGetNameOffset(G, sele, 999, SettingGetGlobal_b(G, cSetting_ignore_case));   /* already exist? */
  if(n >= 0) {                  /* get rid of existing selection -- but never selection 0 (all) */
    SelectorDeleteSeleAtOffset(G, n);
  }
}


/*========================================================================*/
void SelectorGetUniqueTmpName(PyMOLGlobals * G, char *name_buffer)
{
  CSelector *I = G->Selector;
  sprintf(name_buffer, "%s%d", cSelectorTmpPrefix, I->TmpCounter++);
}

/*========================================================================*/
/*
 * If `input` is already a name of an object or a valid position keyword
 * (center, origin, all, ...), then simply copy it to `store`. Otherwise
 * process the selection expression and create a temporary named selection.
 *
 * Unlike `SelectorGetTmp`, this will accept names of maps, groups, etc.
 */
int SelectorGetTmp2(PyMOLGlobals * G, const char *input, char *store, bool quiet)
{
  int count = 0;
  /* ASSUMES that store is at least as big as an OrthoLineType */
  CSelector *I = G->Selector;
  PRINTFD(G, FB_Selector)
    " %s-Debug: entered with \"%s\".\n", __func__, input ENDFD;

  store[0] = 0;

  /* skip trivial cases */

  if(input[0] && !((input[0] == '\'') && (input[1] == '\'') && (!input[2]))) {

    /* OKAY, this routine is in flux.  Eventually this routine will...

       (1) fully parse the input recognizing selection keywords, nested
       parens, quotes, escaped strings, etc.

       (2) replace selection blocks with temporary selection names

       (3) return a space-separated list of names for processing

       However, right now, this routine simply handles two cases.

       A. where the input is a selection, in which case store is set to a
       temporary selection name

       B. where the input is simply a list of space-separated name patterns,
       in which case store is simply passed along as a copy of the input

     */

    // make selection if "input" doesn't fit into "store"
    int is_selection = strlen(input) >= OrthoLineLength;

    const char *p = input;
    OrthoLineType word;
    OVreturn_word result;

    if (!is_selection) while(*p) {
      /* copy first word/token of p into "word", remainder of string in p */
      p = ParseWord(word, p, sizeof(OrthoLineType));
      /* see a paren? then this must be a selection */

      if(word[0] == '(') {
        is_selection = true;
        break;
      }

      if(strchr(word, '/')) {
        is_selection = true;
        break;
      }

      /* encounterd a selection keyword? then this must be a selection */

      if(OVreturn_IS_OK((result = OVLexicon_BorrowFromCString(I->Lex, word)))) {
        if(OVreturn_IS_OK((result = OVOneToAny_GetKey(I->Key, result.word)))) {
          if((result.word != SELE_ALLz) &&
             (result.word != SELE_ORIz) && (result.word != SELE_CENz)) {
            is_selection = true;
            break;
          }
        }
      }

      if(!ExecutiveValidName(G, word)) {        /* don't recognize the name? */
        if(!ExecutiveValidNamePattern(G, word)) {       /* don't recognize this as a pattern? */
          is_selection = true;  /* must be a selection */
          break;
        }
      }
    }
    if(is_selection) {          /* incur the computational expense of 
                                   parsing the input as an atom selection */
      WordType name;
      sprintf(name, "%s%d", cSelectorTmpPrefix, I->TmpCounter++);
      count = SelectorCreate(G, name, input, NULL, quiet, NULL);
      if(count >= 0) {
        strcpy(store, name);
      } else {
        store[0] = 0;
      }
    } else {                    /* otherwise, just parse the input as a space-separated list of names */
      /* not a selection */
      strcpy(store, input);
    }
  }
  PRINTFD(G, FB_Selector)
    " %s-Debug: leaving with \"%s\".\n", __func__, store ENDFD;
  return count;

}

/*========================================================================*/
/*
 * Like SelectorGetTmp2, but doesn't accept names from any non-molecular
 * entities like groups or map objects (those will be processed as selection
 * expressions).
 */
int SelectorGetTmp(PyMOLGlobals * G, const char *input, char *store, bool quiet)
{
  int count = 0;
  CSelector *I = G->Selector;

  store[0] = 0;

  // trivial (but valid) case: empty selection string
  ok_assert(1, input[0]);

  // if object molecule or named selection, then don't create a temp selection
  if (ExecutiveIsMoleculeOrSelection(G, input)
      && strncmp(input, cSelectorTmpPrefix, cSelectorTmpPrefixLen) != 0) {
    strcpy(store, input);
    return 0;
  }

  // evaluate expression and create a temp selection
  sprintf(store, "%s%d", cSelectorTmpPrefix, I->TmpCounter++);
  count = SelectorCreate(G, store, input, NULL, quiet, NULL);
  if(count < 0)
    store[0] = 0;

ok_except1:
  return count;
}

int SelectorCheckTmp(PyMOLGlobals * G, const char *name)
{
  if(WordMatch(G, cSelectorTmpPattern, name, false) + 1 ==
     - cSelectorTmpPrefixLen)
    return true;
  else
    return false;
}


/*========================================================================*/
void SelectorFreeTmp(PyMOLGlobals * G, const char *name)
{                               /* remove temporary selections */
  if(name && name[0]) {
    if(strncmp(name, cSelectorTmpPrefix, cSelectorTmpPrefixLen) == 0) {
      ExecutiveDelete(G, name);
    }
  }
}


/*========================================================================*/
static int SelectorEmbedSelection(PyMOLGlobals * G, const int *atom, const char *name,
                                  ObjectMolecule * obj, int no_dummies, int exec_managed)
{
  /* either atom or obj should be NULL, not both and not neither */

  CSelector *I = G->Selector;
  int tag;
  int newFlag = true;
  int n, a, m, sele;
  int c = 0;
  int start = 0;
  int singleAtomFlag = true;
  int singleObjectFlag = true;
  ObjectMolecule *singleObject = NULL, *selObj;
  int singleAtom = -1;
  int index;
  AtomInfoType *ai;

  if(exec_managed < 0) {
    if(atom)                    /* automatic behavior: manage selections defined via atom masks */
      exec_managed = true;
    else
      exec_managed = false;
  }

  n = SelectGetNameOffset(G, name, 999, SettingGetGlobal_b(G, cSetting_ignore_case));   /* already exist? */
  if(n == 0)                    /* don't allow redefinition of "all" */
    return 0;
  if(n > 0) {                   /* get rid of existing selection */
    SelectorDeleteSeleAtOffset(G, n);
    newFlag = false;
  }

  /*  printf("I->NMember %d I->FreeMember %d\n",I->NMember,I->FreeMember); */

  n = I->NActive;
  /* make sure there's enough space for n+1 SelectorWordTypes
   * and SelectorInfoRecs in the Selector */
  VLACheck(I->Name, SelectorWordType, n + 1);
  VLACheck(I->Info, SelectionInfoRec, n + 1);
  /* copy the name and null terminate it */
  strcpy(I->Name[n], name);
  I->Name[n + 1][0] = 0;
  /* This name is in Name, so now set it in the OVLexicon hash */
  SelectorAddName(G, n);
  sele = I->NSelection++;
  SelectionInfoInit(I->Info + n);
  I->Info[n].ID = sele;
  I->NActive++;

  if(no_dummies) {
    start = 0;
  } else {
    start = cNDummyAtoms;
  }
  for(a = start; a < I->NAtom; a++) {
    tag = false;
    /* set tag based on passed in atom list or on global atom table */
    if(atom) {
      if(atom[a])
        tag = atom[a];
    } else {
      if(I->Obj[I->Table[a].model] == obj)
        tag = 1;
    }
    if(tag) {
      /* if this this atom is tagged, grab its object
       * index, and info record */
      selObj = I->Obj[I->Table[a].model];
      index = I->Table[a].atom;
      ai = selObj->AtomInfo + index;

      /* update whether or not this is a selection w/only one object */
      if(singleObjectFlag) {
        if(singleObject) {
          if(selObj != singleObject) {
            singleObjectFlag = false;
          }
        } else {
          singleObject = selObj;
        }
      }
      /* update whether or not this is a selection w/only one atom */
      if(singleAtomFlag) {
        if(singleAtom >= 0) {
          if(index != singleAtom) {
            singleAtomFlag = false;
          }
        } else {
          singleAtom = index;
        }
      }

      /* store this is the Selectors->Member table, so make sure there's room */
      c++;
      if(I->FreeMember > 0) {
        m = I->FreeMember;
        I->FreeMember = I->Member[m].next;
      } else {
        I->NMember++;
        m = I->NMember;
        VLACheck(I->Member, MemberType, m);
      }
      I->Member[m].selection = sele;
      I->Member[m].tag = tag;
      /* at runtime, selections can now have transient ordering --
         but these are not yet persistent through session saves & restores */
      I->Member[m].next = ai->selEntry;
      ai->selEntry = m;
    }
  }

  /* after scanning, update whether or not we touched multiple objects/atoms */
  if(c) {                       
    SelectionInfoRec *info = I->Info + (I->NActive - 1);
    if(singleObjectFlag) {
      info->justOneObjectFlag = true;
      info->theOneObject = singleObject;
      if(singleAtomFlag) {
        info->justOneAtomFlag = true;
        info->theOneAtom = singleAtom;
      }
    }
  }

  if(exec_managed) {
    if(newFlag)
      ExecutiveManageSelection(G, name);
  }
  PRINTFD(G, FB_Selector)
    " Selector: Embedded %s, %d atoms.\n", name, c ENDFD;
  return (c);
}


/*========================================================================*/
static sele_array_t SelectorApplyMultipick(PyMOLGlobals * G, Multipick * mp)
{
  CSelector *I = G->Selector;
  sele_array_t result;
  int n;
  Picking *p;
  ObjectMolecule *obj;
  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  sele_array_calloc(result, I->NAtom);
  n = mp->picked[0].src.index;
  p = mp->picked + 1;
  while(n--) {                  /* what if this object isn't a molecule object?? */
    obj = (ObjectMolecule *) p->context.object;
    /* NOTE: SeleBase only safe with cSelectorUpdateTableAllStates!  */
    result[obj->SeleBase + p->src.index] = true;
    p++;
  }
  return (result);
}


/*========================================================================*/
static sele_array_t SelectorSelectFromTagDict(PyMOLGlobals * G, const std::unordered_map<int, int>& id2tag)
{
  CSelector *I = G->Selector;
  sele_array_t result{};
  int a;
  AtomInfoType *ai;
  OVreturn_word ret;

  SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);    /* for now, update the entire table */
  {
    TableRec *i_table = I->Table, *table_a;
    ObjectMolecule **i_obj = I->Obj;

    sele_array_calloc(result, I->NAtom);
    if(result) {
      table_a = i_table + cNDummyAtoms;
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        ai = i_obj[table_a->model]->AtomInfo + table_a->atom;
        if(ai->unique_id) {
          auto it = id2tag.find(ai->unique_id);
          if(it != id2tag.end()) {
            result[a] = it->second;
          }
        }
        table_a++;
      }
    }
  }
  return (result);
}


/*========================================================================*/

static int _SelectorCreate(PyMOLGlobals * G, const char *sname, const char *sele,
                           ObjectMolecule ** obj, int quiet, Multipick * mp,
                           CSeqRow * rowVLA, int nRow, int **obj_idx, int *n_idx,
                           int n_obj, const std::unordered_map<int, int>* id2tag, int executive_manage,
                           int state, int domain)
{
  sele_array_t atom{};
  OrthoLineType name;
  int ok = true;
  int c = 0;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);
  ObjectMolecule *embed_obj = NULL;

  PRINTFD(G, FB_Selector)
    "SelectorCreate-Debug: entered...\n" ENDFD;

  /* copy sname into name and check if it's a keyword; abort on 
   * the selection name == keyword: eg. "select all, none" */
  if(sname[0] == '%')
    strcpy(name, &sname[1]);
  else
    strcpy(name, sname);
  if(WordMatchExact(G, cKeywordAll, name, ignore_case)) {
    name[0] = 0;                /* force error */
  }
  UtilCleanStr(name);
  /* name was invalid, output error msg to user */
  if(!name[0]) {
      PRINTFB(G, FB_Selector, FB_Errors)
	"Selector-Error: Invalid selection name \"%s\".\n", sname ENDFB(G);
  }
  if(ok) {
    if(sele) {
      atom = SelectorSelect(G, sele, state, domain, quiet);
      if(!atom)
        ok = false;
    } else if(id2tag) {
      atom = SelectorSelectFromTagDict(G, *id2tag);
    } else if(obj && obj[0]) {  /* optimized full-object selection */
      if(n_obj <= 0) {
        embed_obj = *obj;
        if(obj_idx && n_idx) {
          atom =
            SelectorUpdateTableSingleObject(G, embed_obj, cSelectorUpdateTableAllStates,
                                            false, *obj_idx, *n_idx, (n_obj == 0));
        } else {
          atom =
            SelectorUpdateTableSingleObject(G, embed_obj, cSelectorUpdateTableAllStates,
                                            false, NULL, 0, (n_obj == 0));
        }
      } else {
        atom = SelectorUpdateTableMultiObjectIdxTag(G, obj, false, obj_idx, n_idx, n_obj);
      }
    } else if(mp) {
      atom = SelectorApplyMultipick(G, mp);
#if 0
    } else if(rowVLA) {
      atom = SelectorApplySeqRowVLA(G, rowVLA, nRow);
#endif
    } else
      ok = false;
  }
  if(ok)
    c = SelectorEmbedSelection(G, atom.get(), name, embed_obj, false, executive_manage);
  atom.reset();
  SelectorClean(G);
  /* ignore reporting on quiet */
  if(!quiet) {
    /* ignore reporting on internal/private names */
    if(name[0] != '_') {
      if(ok) {
        PRINTFB(G, FB_Selector, FB_Actions)
          " Selector: selection \"%s\" defined with %d atoms.\n", name, c ENDFB(G);
      }
    }
  }
  if(ok) {
    PRINTFD(G, FB_Selector)
      " %s: \"%s\" created with %d atoms.\n", __func__, name, c ENDFD;
  } else {
    PRINTFD(G, FB_Selector)
      " %s: \"%s\" not created due to error\n", __func__, name ENDFD;
  }
  if(!ok)
    c = -1;
  return (c);
}

int SelectorCreateFromTagDict(PyMOLGlobals * G, const char *sname, const std::unordered_map<int, int>& id2tag,
                              int exec_managed)
{
  return _SelectorCreate(G, sname, NULL, NULL, true, NULL, NULL, 0, NULL, NULL, 0, &id2tag,
                         exec_managed, -1, -1);
}

int SelectorCreateEmpty(PyMOLGlobals * G, const char *name, int exec_managed)
{
  return _SelectorCreate(G, name, "none", NULL, 1, NULL, NULL, 0, NULL, 0, 0, NULL,
                         exec_managed, -1, -1);
}

int SelectorCreateSimple(PyMOLGlobals * G, const char *name, const char *sele)
{
  return _SelectorCreate(G, name, sele, NULL, 1, NULL, NULL, 0, NULL, 0, 0, NULL, -1, -1,
                         -1);
}

int SelectorCreateFromObjectIndices(PyMOLGlobals * G, const char *sname, ObjectMolecule * obj,
                                    int *idx, int n_idx)
{
  return _SelectorCreate(G, sname, NULL, &obj, true, NULL, NULL, 0, &idx, &n_idx, -1, NULL, -1, -1, -1); 
  /* n_obj = -1 disables numbered tags */
}

int SelectorCreateOrderedFromObjectIndices(PyMOLGlobals * G, const char *sname,
                                           ObjectMolecule * obj, int *idx, int n_idx)
{
  return _SelectorCreate(G, sname, NULL, &obj, true, NULL, NULL, 0, &idx, &n_idx, 0, NULL, -1, -1, -1);
  /* assigned numbered tags */
}

int SelectorCreateOrderedFromMultiObjectIdxTag(PyMOLGlobals * G, const char *sname,
                                               ObjectMolecule ** obj,
                                               int **idx_tag, int *n_idx, int n_obj)
{
  return _SelectorCreate(G, sname, NULL, obj, true, NULL, NULL, 0, idx_tag, n_idx, n_obj,
                         NULL, -1, -1, -1);
}

int SelectorCreate(PyMOLGlobals * G, const char *sname, const char *sele, ObjectMolecule * obj,
                   int quiet, Multipick * mp)
{
  return _SelectorCreate(G, sname, sele, &obj, quiet, mp, NULL, 0, NULL, 0, 0, NULL, -1,
                         -1, -1);
}

int SelectorCreateWithStateDomain(PyMOLGlobals * G, const char *sname, const char *sele,
                                  ObjectMolecule * obj, int quiet, Multipick * mp,
                                  int state, const char *domain)
{
  int domain_sele = -1;
  ObjectNameType valid_name;

  UtilNCopy(valid_name, sname, sizeof(valid_name));
  if(SettingGetGlobal_b(G, cSetting_validate_object_names)) {
    ObjectMakeValidName(G, valid_name);
    sname = valid_name;
  }

  if(domain && domain[0]) {
    if(!WordMatchExact(G, cKeywordAll, domain, true)) { /* allow domain=all */
      domain_sele = SelectorIndexByName(G, domain);
      if(domain_sele < 0) {

        PRINTFB(G, FB_Selector, FB_Errors)
          "Selector-Error: Invalid domain selection name \"%s\".\n", domain ENDFB(G);
        return -1;
      }
    }
  }
  return _SelectorCreate(G, sname, sele, &obj, quiet, mp, NULL, 0, NULL, 0, 0, NULL, -1,
                         state, domain_sele);
}


/*========================================================================*/
static void SelectorClean(PyMOLGlobals * G)
{
  SelectorCleanImpl(G, G->Selector);
}

static void SelectorCleanImpl(PyMOLGlobals * G, CSelector *I)
{
  FreeP(I->Table);
  I->Table = NULL;
  FreeP(I->Obj);
  I->Obj = NULL;
  FreeP(I->Vertex);
  I->Vertex = NULL;
  FreeP(I->Flag1);
  I->Flag1 = NULL;
  FreeP(I->Flag2);
  I->Flag2 = NULL;
  I->NAtom = 0;
  ExecutiveInvalidateSelectionIndicatorsCGO(G);
}


/*========================================================================*/
static sele_array_t SelectorUpdateTableSingleObject(PyMOLGlobals * G, ObjectMolecule * obj,
                                            int req_state,
                                            int no_dummies, int *idx,
                                            int n_idx, int numbered_tags)
{
  int a = 0;
  int c = 0;
  int modelCnt;
  sele_array_t result{};
  int tag = true;
  int state = req_state;
  CSelector *I = G->Selector;

  PRINTFD(G, FB_Selector)
    "SelectorUpdateTableSingleObject-Debug: entered for %s...\n", obj->Name ENDFD;

  SelectorClean(G);

  switch (req_state) {
  case cSelectorUpdateTableAllStates:
    state = req_state;
    break;
  case cSelectorUpdateTableEffectiveStates:
    state = ObjectGetCurrentState(obj, true);
    break;
  case cSelectorUpdateTableCurrentState:
    state = SceneGetState(G);
    break;
  default:
    if(req_state < 0)
      state = cSelectorUpdateTableAllStates;    /* fail safe */
    break;
  }

  switch (req_state) {
  case cSelectorUpdateTableAllStates:
    I->SeleBaseOffsetsValid = true;     /* all states -> all atoms -> offsets valid */
    break;
  default:
    I->SeleBaseOffsetsValid = false;    /* not including all atoms, so atom-based offsets are invalid */
    break;
  }

  I->NCSet = 0;
  if(no_dummies) {
    modelCnt = 0;
    c = 0;
  } else {
    modelCnt = cNDummyModels;
    c = cNDummyAtoms;
  }
  c += obj->NAtom;
  if(I->NCSet < obj->NCSet)
    I->NCSet = obj->NCSet;
  modelCnt++;
  I->Table = pymol::calloc<TableRec>(c);
  ErrChkPtr(G, I->Table);
  I->Obj = pymol::calloc<ObjectMolecule *>(modelCnt);
  ErrChkPtr(G, I->Obj);
  if(no_dummies) {
    modelCnt = 0;
    c = 0;
  } else {
    c = cNDummyAtoms;
    modelCnt = cNDummyModels;
  }
  I->Obj[modelCnt] = obj;

  obj->SeleBase = c;

  if(state < 0) {
    for(a = 0; a < obj->NAtom; a++) {
      I->Table[c].model = modelCnt;
      I->Table[c].atom = a;
      c++;
    }
  } else if(state < obj->NCSet) {
    TableRec *rec = I->Table + c;
    CoordSet *cs = obj->CSet[state];
    if(cs) {
      for(a = 0; a < obj->NAtom; a++) {
        int ix;
        ix = cs->atmToIdx(a);
        if(ix >= 0) {
          rec->model = modelCnt;
          rec->atom = a;
          rec++;
        }
      }
    }
    c = rec - I->Table;
  }

  if(idx && n_idx) {
    sele_array_calloc(result, c);
    if(n_idx > 0) {
      for(a = 0; a < n_idx; a++) {
        int at = idx[a];
        if(numbered_tags)
          tag = a + SELECTOR_BASE_TAG;
        if((at >= 0) && (at < obj->NAtom)) {
          /* create an ordered selection based on the input order of the object indices */
          result[obj->SeleBase + at] = tag;
        }
      }
    } else {                    /* -1 terminated list */
      int *at_idx = idx;
      int at;
      a = SELECTOR_BASE_TAG + 1;
      while((at = *(at_idx++)) >= 0) {
        if(numbered_tags) {
          tag = a++;
        }
        if((at >= 0) && (at < obj->NAtom)) {
          /* create an ordered selection based on the input order of the object indices */
          result[obj->SeleBase + at] = tag;
        }
      }
    }
  }
  modelCnt++;
  I->NModel = modelCnt;
  I->NAtom = c;
  I->Flag1 = pymol::malloc<int>(c);
  ErrChkPtr(G, I->Flag1);
  I->Flag2 = pymol::malloc<int>(c);
  ErrChkPtr(G, I->Flag2);
  I->Vertex = pymol::malloc<float>(c * 3);
  ErrChkPtr(G, I->Vertex);

  PRINTFD(G, FB_Selector)
    "SelectorUpdateTableSingleObject-Debug: leaving...\n" ENDFD;

  return (result);
}


/*========================================================================*/
int SelectorUpdateTable(PyMOLGlobals * G, int req_state, int domain)
{
  return (SelectorUpdateTableImpl(G, G->Selector, req_state, domain));
}

int SelectorUpdateTableImpl(PyMOLGlobals * G, CSelector *I, int req_state, int domain)
{
  int a = 0;
  ov_size c = 0;
  int modelCnt;
  int state = req_state;
  void *iterator = NULL;
  ObjectMolecule *obj = NULL;

  /* Origin and Center are dummy objects */
  if(!I->Origin)
    I->Origin = ObjectMoleculeDummyNew(G, cObjectMoleculeDummyOrigin);

  if(!I->Center)
    I->Center = ObjectMoleculeDummyNew(G, cObjectMoleculeDummyCenter);

  SelectorClean(G);
  I->NCSet = 0;

  /* take a summary of PyMOL's current state; foreach molecular object
   * sum up the number of atoms, count how many models, states, etc... */
  modelCnt = cNDummyModels;
  c = cNDummyAtoms;
  while(ExecutiveIterateObjectMolecule(G, &obj, &iterator)) {
    c += obj->NAtom;
    if(I->NCSet < obj->NCSet)
      I->NCSet = obj->NCSet;
    modelCnt++;
  }
  /* allocate space for each atom, in the record table */
  I->Table = pymol::calloc<TableRec>(c);
  ErrChkPtr(G, I->Table);
  I->Obj = pymol::calloc<ObjectMolecule *>(modelCnt);
  ErrChkPtr(G, I->Obj);

  switch (req_state) {
  case cSelectorUpdateTableAllStates:
    I->SeleBaseOffsetsValid = true;     /* all states -> all atoms -> offsets valid */
    break;
  default:
    I->SeleBaseOffsetsValid = false;    /* not including all atoms, so atom-based offsets are invalid */
    break;
  }

  c = 0;
  modelCnt = 0;

  /* update the origin and center dummies */
  obj = I->Origin;
  if(obj) {
    I->Obj[modelCnt] = I->Origin;
    obj->SeleBase = c;          /* make note of where this object starts */
    for(a = 0; a < obj->NAtom; a++) {
      I->Table[c].model = modelCnt;
      I->Table[c].atom = a;
      c++;
    }
    modelCnt++;
  }

  obj = I->Center;
  if(obj) {
    I->Obj[modelCnt] = I->Center;
    obj->SeleBase = c;          /* make note of where this object starts */
    for(a = 0; a < obj->NAtom; a++) {
      I->Table[c].model = modelCnt;
      I->Table[c].atom = a;
      c++;
    }
    modelCnt++;
  }

  if(req_state < cSelectorUpdateTableAllStates) {
    state = SceneGetState(G);   /* just in case... */
  }

  while(ExecutiveIterateObjectMolecule(G, &obj, &iterator)) {
    int skip_flag = false;
    if(req_state < 0) {
      switch (req_state) {
      case cSelectorUpdateTableAllStates:
        state = -1;             /* all states */
        /* proceed... */
        break;
      case cSelectorUpdateTableCurrentState:
        state = SettingGetGlobal_i(G, cSetting_state) - 1;
        break;
      case cSelectorUpdateTableEffectiveStates:
        state = ObjectGetCurrentState(obj, true);
        break;
      default:                 /* unknown input -- fail safe (all states) */
        state = -1;
        break;
      }
    } else {
      if(state >= obj->NCSet)
        skip_flag = true;
      else if(!obj->CSet[state])
        skip_flag = true;
    }

    if(!skip_flag) {
      /* fill in the table */
      I->Obj[modelCnt] = obj;
      {
        int n_atom = obj->NAtom;
        TableRec *rec = I->Table + c;
        TableRec *start_rec = rec;
        if(state < 0) {         /* all states */
          if(domain < 0) {      /* domain=all */
            for(a = 0; a < n_atom; a++) {
              rec->model = modelCnt;
              rec->atom = a;
              rec++;
            }
          } else {
            const AtomInfoType *ai = obj->AtomInfo.data();
            int included_one = false;
            int excluded_one = false;
            for(a = 0; a < n_atom; a++) {
              if(SelectorIsMember(G, ai->selEntry, domain)) {
                rec->model = modelCnt;
                rec->atom = a;
                rec++;
                included_one = true;
              } else {
                excluded_one = true;
              }
              ai++;
            }
            if(included_one && excluded_one)
              I->SeleBaseOffsetsValid = false;  /* partial objects in domain, so
                                                   base offsets are invalid */
          }
        } else {                /* specific states */
          CoordSet *cs;
          int idx;
          if(domain < 0) {
            for(a = 0; a < n_atom; a++) {
              /* does coordinate exist for this atom in the requested state? */
              if(state < obj->NCSet)
                cs = obj->CSet[state];
              else
                cs = NULL;
              if(cs) {
                idx = cs->atmToIdx(a);
                if(idx >= 0) {
                  rec->model = modelCnt;
                  rec->atom = a;
                  rec++;
                }
              }
            }
          } else {
            const AtomInfoType *ai = obj->AtomInfo.data();
            for(a = 0; a < n_atom; a++) {
              /* does coordinate exist for this atom in the requested state? */
              if(state < obj->NCSet)
                cs = obj->CSet[state];
              else
                cs = NULL;
              if(cs) {
                idx = cs->atmToIdx(a);
                if(idx >= 0) {
                  if(SelectorIsMember(G, ai->selEntry, domain)) {
                    rec->model = modelCnt;
                    rec->atom = a;
                    rec++;
                  }
                }
              }
              ai++;
            }
          }
        }
        if(rec != start_rec) {  /* skip excluded models */
          modelCnt++;
          obj->SeleBase = c;    /* make note of where this object starts */
          c += (rec - start_rec);
        } else {
          obj->SeleBase = 0;
        }
      }
    }
  }
  I->NModel = modelCnt;
  I->NAtom = c;
  I->Flag1 = pymol::malloc<int>(c);
  ErrChkPtr(G, I->Flag1);
  I->Flag2 = pymol::malloc<int>(c);
  ErrChkPtr(G, I->Flag2);
  I->Vertex = pymol::malloc<float>(c * 3);
  ErrChkPtr(G, I->Vertex);
  /* printf("selector update table state=%d, natom=%d\n",req_state,c); */
  return (true);
}


/*========================================================================*/
static sele_array_t SelectorSelect(PyMOLGlobals * G, const char *sele, int state, int domain, int quiet)
{
  PRINTFD(G, FB_Selector)
    "SelectorSelect-DEBUG: sele = \"%s\"\n", sele ENDFD;
  SelectorUpdateTable(G, state, domain);
  auto parsed = SelectorParse(G, sele);
  if (!parsed.empty()) {
    return SelectorEvaluate(G, parsed, state, quiet);
  }
  return {};
}


/*========================================================================*/
static int SelectorModulate1(PyMOLGlobals * G, EvalElem * base, int state)
{
  CSelector *I = G->Selector;
  int a, d, e;
  int c = 0;
  float dist;
  int nbond;
  float *v2;
  CoordSet *cs;
  int ok = true;
  int nCSet;
  MapType *map;
  int i, j, h, k, l;
  int n1, at, idx;
  ObjectMolecule *obj;

  if(state < 0) {
    switch (state) {
    case -2:
    case -3:
      state = SceneGetState(G);
      break;
    }
  }

  base[1].sele = std::move(base[0].sele);  /* base1 has the mask */
  base->sele_calloc(I->NAtom);
  base->sele_check_ok(ok);
  if (!ok)
    return false;
  switch (base[1].code) {
  case SELE_ARD_:
  case SELE_EXP_:
    if(!sscanf(base[2].text(), "%f", &dist))
      ok = ErrMessage(G, "Selector", "Invalid distance.");
    if(ok) {
      for(d = 0; d < I->NCSet; d++) {
        if((state < 0) || (d == state)) {
          n1 = 0;
          for(a = 0; a < I->NAtom; a++) {
            I->Flag1[a] = false;
            at = I->Table[a].atom;
            obj = I->Obj[I->Table[a].model];
            if(d < obj->NCSet)
              cs = obj->CSet[d];
            else
              cs = NULL;
            if(cs) {
              if(CoordSetGetAtomVertex(cs, at, I->Vertex + 3 * a)) {
                I->Flag1[a] = true;
                n1++;
              }
            }
          }
          if(n1) {
            map = MapNewFlagged(G, -dist, I->Vertex, I->NAtom, NULL, I->Flag1);
            if(map) {
              ok &= MapSetupExpress(map);
              nCSet = SelectorGetArrayNCSet(G, base[1].sele, false);
              for(e = 0; ok && e < nCSet; e++) {
                if((state < 0) || (e == state)) {
                  for(a = 0; ok && a < I->NAtom; a++) {
                    if(base[1].sele[a]) {
                      at = I->Table[a].atom;
                      obj = I->Obj[I->Table[a].model];
                      if(e < obj->NCSet)
                        cs = obj->CSet[e];
                      else
                        cs = NULL;
                      if(cs) {
                        idx = cs->atmToIdx(at);
                        if(idx >= 0) {
                          v2 = cs->Coord + (3 * idx);
                          MapLocus(map, v2, &h, &k, &l);
                          i = *(MapEStart(map, h, k, l));
                          if(i) {
                            j = map->EList[i++];
                            while(j >= 0) {
                              if((!base[0].sele[j]) && ((base[1].code == SELE_EXP_)
                                                        || (!base[1].sele[j]))) {   
                                /*exclude current selection */
                                if(within3f(I->Vertex + 3 * j, v2, dist))
                                  base[0].sele[j] = true;
                              }
                              j = map->EList[i++];
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
              MapFree(map);
            }
          }
        }
      }
    }
    break;

  case SELE_EXT_:
    if(sscanf(base[2].text(), "%d", &nbond) != 1)
      ok = ErrMessage(G, "Selector", "Invalid bond count.");
    if(ok) {
      ObjectMolecule *lastObj = NULL;
      int a, n, a0, a1, a2;
      std::copy_n(base[1].sele_data(), I->NAtom, base[0].sele_data());
      while((nbond--) > 0) {
        std::swap(base[1].sele, base[0].sele);
        for(a = cNDummyAtoms; a < I->NAtom; a++) {
          if(base[1].sele[a]) {
            if(I->Obj[I->Table[a].model] != lastObj) {
              lastObj = I->Obj[I->Table[a].model];
              ObjectMoleculeUpdateNeighbors(lastObj);
            }
            a0 = I->Table[a].atom;
            n = lastObj->Neighbor[a0];
            n++;
            while(1) {
              a1 = lastObj->Neighbor[n];
              if(a1 < 0)
                break;
              if((a2 = SelectorGetObjAtmOffset(I, lastObj, a1)) >= 0) {
                base[0].sele[a2] = 1;
                n += 2;
              }
            }
          }
        }
      }
      base[1].sele_free();
    }
    break;

  case SELE_GAP_:
    if(!sscanf(base[2].text(), "%f", &dist))
      ok = ErrMessage(G, "Selector", "Invalid distance.");
    if(ok) {
      for(a = 0; a < I->NAtom; a++) {
        obj = I->Obj[I->Table[a].model];
        at = I->Table[a].atom;
        I->Table[a].f1 = obj->AtomInfo[at].vdw;
        base[0].sele[a] = true; /* start selected, subtract off */
        c = I->NAtom;
      }
      for(d = 0; d < I->NCSet; d++) {
        if((state < 0) || (d == state)) {
          n1 = 0;
          for(a = 0; a < I->NAtom; a++) {
            obj = I->Obj[I->Table[a].model];
            I->Flag1[a] = false;
            at = I->Table[a].atom;
            if(d < obj->NCSet)
              cs = obj->CSet[d];
            else
              cs = NULL;
            if(cs) {
              if(CoordSetGetAtomVertex(cs, at, I->Vertex + 3 * a)) {
                I->Flag1[a] = true;
                n1++;
              }
            }
          }
          if(n1) {
            map =
              MapNewFlagged(G, -(dist + 2 * MAX_VDW), I->Vertex, I->NAtom, NULL,
                            I->Flag1);
            if(map) {

              ok &= MapSetupExpress(map);
              nCSet = SelectorGetArrayNCSet(G, base[1].sele, false);
              for(e = 0; ok && e < nCSet; e++) {
                if((state < 0) || (e == state)) {
                  for(a = 0; a < I->NAtom; a++) {
                    if(base[1].sele[a]) {
                      at = I->Table[a].atom;
                      obj = I->Obj[I->Table[a].model];
                      if(e < obj->NCSet)
                        cs = obj->CSet[e];
                      else
                        cs = NULL;
                      if(cs) {
                        idx = cs->atmToIdx(at);

                        if(idx >= 0) {
                          v2 = cs->Coord + (3 * idx);
                          MapLocus(map, v2, &h, &k, &l);
                          i = *(MapEStart(map, h, k, l));
                          if(i) {
                            j = map->EList[i++];
                            while(j >= 0) {
                              if((base[0].sele[j]) && (!base[1].sele[j])) {     /*exclude current selection */
                                if(within3f(I->Vertex + 3 * j, v2, dist +       /* eliminate atoms w/o gap */
                                            I->Table[a].f1 + I->Table[j].f1)) {
                                  base[0].sele[j] = false;
                                  c--;
                                }
                              } else if(base[1].sele[j]) {
                                base[0].sele[j] = false;
                                c--;
                              }
                              j = map->EList[i++];
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
              MapFree(map);
            }
          }
        }
      }
    }
    break;
  }
  base[1].sele_free();
  if(Feedback(G, FB_Selector, FB_Debugging)) {
    c = 0;
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      if(base[0].sele[a])
        c++;
    fprintf(stderr, "SelectorModulate0: %d atoms selected.\n", c);
  }
  return (ok);

}


/*========================================================================*/
static int SelectorSelect0(PyMOLGlobals * G, EvalElem * passed_base)
{
  CSelector *I = G->Selector;
  TableRec *i_table = I->Table;
  ObjectMolecule **i_obj = I->Obj;
  int a, b, flag;
  EvalElem *base = passed_base;
  int c = 0;
  int state;
  ObjectMolecule *obj, *cur_obj = NULL;
  CoordSet *cs;

  base->type = STYP_LIST;
  base->sele_calloc(I->NAtom);
  base->sele_err_chk_ptr(G);

  switch (base->code) {
  case SELE_HBAs:
  case SELE_HBDs:
  case SELE_DONz:
  case SELE_ACCz:

    {
      /* first, verify chemistry for all atoms... */
      ObjectMolecule *lastObj = NULL, *obj;
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        obj = i_obj[i_table[a].model];
        if(obj != lastObj) {
          ObjectMoleculeUpdateNeighbors(obj);
          ObjectMoleculeVerifyChemistry(obj, -1);
          lastObj = obj;
        }
      }
    }
    switch (base->code) {
    case SELE_HBAs:
    case SELE_ACCz:
      for(a = cNDummyAtoms; a < I->NAtom; a++)
        base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].hb_acceptor;
      break;
    case SELE_HBDs:
    case SELE_DONz:
      for(a = cNDummyAtoms; a < I->NAtom; a++)
        base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].hb_donor;
      break;

    }
    break;
  case SELE_NONz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] = false;
    break;
  case SELE_BNDz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].bonded;
    break;
  case SELE_HETz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].hetatm;
    break;
  case SELE_HYDz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].isHydrogen();
    break;
  case SELE_METz:
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].isMetal();
    }
    break;
  case SELE_BB_z:
  case SELE_SC_z:
    {
      AtomInfoType *ai;
      flag = (base->code == SELE_BB_z);
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        ai = i_obj[i_table[a].model]->AtomInfo + i_table[a].atom;
        if(!(ai->flags & cAtomFlag_polymer)) {
          base[0].sele[a] = 0;
          continue;
        }
        base[0].sele[a] = !flag;
        for(b = 0; backbone_names[b][0]; b++) {
          if(!(strcmp(LexStr(G, ai->name), backbone_names[b]))) {
            base[0].sele[a] = flag;
            break;
          }
        }
      }
    }
    break;
  case SELE_FXDz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_fix;
    break;
  case SELE_RSTz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_restrain;
    break;
  case SELE_POLz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_polymer;
    break;
  case SELE_PROz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_protein;
    break;
  case SELE_NUCz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_nucleic;
    break;
  case SELE_SOLz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_solvent;
    break;
  case SELE_PTDz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].protekted;
    break;
  case SELE_MSKz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].masked;
    break;
  case SELE_ORGz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_organic;
    break;
  case SELE_INOz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_inorganic;
    break;
  case SELE_GIDz:
    for(a = cNDummyAtoms; a < I->NAtom; a++)
      base[0].sele[a] =
        i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & cAtomFlag_guide;
    break;

  case SELE_PREz:
    flag = false;
    cs = NULL;
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      base[0].sele[a] = false;
      obj = i_obj[i_table[a].model];
      if(obj != cur_obj) {      /* different object */
        state = obj->getState();
        if(state >= obj->NCSet)
          flag = false;
        else if(state < 0)
          flag = false;
        else if(!obj->CSet[state])
          flag = false;
        else {
          cs = obj->CSet[state];
          flag = true;          /* valid state */
        }
        cur_obj = obj;
      }
      if(flag && cs) {
        if(cs->atmToIdx(i_table[a].atom) >= 0) {
          base[0].sele[a] = true;
          c++;
        }
      }
    }
    break;
  case SELE_ALLz:
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      base[0].sele[a] = true;
      c++;
    }
    break;
  case SELE_ORIz:
    for(a = 0; a < I->NAtom; a++) {
      base[0].sele[a] = false;
      c++;
    }
    if(I->Origin)
      ObjectMoleculeDummyUpdate(I->Origin, cObjectMoleculeDummyOrigin);
    base[0].sele[cDummyOrigin] = true;
    break;
  case SELE_CENz:
    for(a = 0; a < I->NAtom; a++) {
      base[0].sele[a] = false;
      c++;
    }
    if(I->Center)
      ObjectMoleculeDummyUpdate(I->Center, cObjectMoleculeDummyCenter);
    base[0].sele[cDummyCenter] = true;
    break;
  case SELE_VISz:
    {
      ObjectMolecule *last_obj = NULL;
      AtomInfoType *ai;
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        flag = false;
        obj = i_obj[i_table[a].model];
        if(obj->Enabled) {
          ai = obj->AtomInfo + i_table[a].atom;

          if(last_obj != obj) {
            ObjectMoleculeUpdateNeighbors(obj);
            ObjectMoleculeVerifyChemistry(obj, -1);
            last_obj = obj;
          }

          flag = ai->isVisible();
        }
        base[0].sele[a] = flag;
        if(flag)
          c++;
      }
    }
    break;
  case SELE_ENAz:
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      flag = (i_obj[i_table[a].model]->Enabled);
      base[0].sele[a] = flag;
      if(flag)
        c++;
    }
    break;
  }
  PRINTFD(G, FB_Selector)
    " %s: %d atoms selected.\n", __func__, c ENDFD;

  return (1);
}


/*========================================================================*/
static int SelectorSelect1(PyMOLGlobals * G, EvalElem * base, int quiet)
{
  CSelector *I = G->Selector;
  CWordMatcher *matcher = NULL;
  int a, b, c = 0, hit_flag;
  ObjectMolecule **i_obj = I->Obj, *obj, *last_obj;
  TableRec *i_table = I->Table, *table_a;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);
  int ignore_case_chain = SettingGetGlobal_b(G, cSetting_ignore_case_chain);
  int I_NAtom = I->NAtom;
  int *base_0_sele_a;

  int model, sele, s, col_idx;
  int flag;
  int ok = true;
  int index, state;
  int rep_mask;
  const char *wildcard = SettingGetGlobal_s(G, cSetting_wildcard);

  ObjectMolecule *cur_obj = NULL;
  CoordSet *cs = NULL;

  base->type = STYP_LIST;
  base->sele_calloc(I_NAtom);    /* starting with zeros */
  base->sele_err_chk_ptr(G);
  switch (base->code) {
  case SELE_PEPs:
    if(base[1].text()[0]) {
      AtomInfoType *last_ai0 = NULL, *ai0;
      for(a = cNDummyAtoms; a < I_NAtom; a++) {
        ai0 = i_obj[i_table[a].model]->AtomInfo + i_table[a].atom;
        if(!AtomInfoSameResidueP(G, ai0, last_ai0)) {   /* new starting residue */
          int match_found = false;
          const char *ch = base[1].text();      /* sequence argument */
          AtomInfoType *ai1, *last_ai1 = NULL;
          for(b = a; b < I_NAtom; b++) {
            ai1 = i_obj[i_table[b].model]->AtomInfo + i_table[b].atom;
            if(!AtomInfoSameResidueP(G, ai1, last_ai1)) {
              if(*ch != '-') {  /* if not skipping this residue */
                if(!((*ch == '+') || (SeekerGetAbbr(G, LexStr(G, ai1->resn), 'O', 0) == *ch))) {   /* if a mismatch */
                  break;
                }
              }
              ch++;
              if(!*ch) {        /* end of sequence pattern */
                match_found = true;
                break;
              }
              last_ai1 = ai1;
            }
          }
          if(match_found) {
            const char *ch = base[1].text();    /* sequence argument */
            AtomInfoType *ai1, *last_ai1 = NULL, *ai2;
            for(b = a; b < I_NAtom; b++) {
              ai1 = i_obj[i_table[b].model]->AtomInfo + i_table[b].atom;
              if(!AtomInfoSameResidueP(G, ai1, last_ai1)) {
                if(*ch != '-') {        /* if not skipping this residue */
                  if((*ch == '+') || (SeekerGetAbbr(G, LexStr(G, ai1->resn), 'O', 0) == *ch)) {    /* if matched */
                    int d;
                    for(d = b; d < I_NAtom; d++) {
                      ai2 = i_obj[i_table[d].model]->AtomInfo + i_table[d].atom;        /* complete residue */
                      if(AtomInfoSameResidue(G, ai1, ai2)) {
                        c++;
                        base[0].sele[d] = true;
                      }
                    }
                  }
                }
                ch++;
                if(!*ch) {      /* end of sequence pattern */
                  break;
                }
                last_ai1 = ai1;
              }
            }
          }
        }
      }
    }
    break;
  case SELE_IDXs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigInteger(&options);

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a = WordMatcherMatchInteger(matcher, table_a->atom + 1)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }

    }
    break;
  case SELE_ID_s:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigInteger(&options);

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchInteger(matcher,
                                      i_obj[table_a->model]->AtomInfo[table_a->atom].id)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_RNKs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigInteger(&options);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchInteger(matcher,
                                      i_obj[table_a->model]->AtomInfo[table_a->atom].
                                      rank)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_NAMs:
    {
      CWordMatchOptions options;
      const char *atom_name_wildcard = SettingGetGlobal_s(G, cSetting_atom_name_wildcard);

      if(!atom_name_wildcard[0])
        atom_name_wildcard = wildcard;

      WordMatchOptionsConfigAlphaList(&options, atom_name_wildcard[0], ignore_case);

      matcher = WordMatcherNew(G, base[1].text(), &options, false);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];
      last_obj = NULL;
      for(a = cNDummyAtoms; a < I_NAtom; a++) {
        obj = i_obj[table_a->model];
        if(obj != last_obj) {

          /* allow objects to have their own atom_name_wildcards...this is a tricky workaround
             for handling nucleic acid structures that use "*" in atom names */

          const char *atom_name_wildcard =
            SettingGet_s(G, obj->Setting, NULL, cSetting_atom_name_wildcard);

          if(!atom_name_wildcard[0])
            atom_name_wildcard = wildcard;

          if(options.wildcard != atom_name_wildcard[0]) {
            options.wildcard = atom_name_wildcard[0];
            if(matcher)
              WordMatcherFree(matcher);
            matcher = WordMatcherNew(G, base[1].text(), &options, false);
            if(!matcher)
              WordPrimeCommaMatch(G, &base[1].m_text[0] /* replace '+' with ',' */);
          }
          last_obj = obj;
        }

        const char * name = LexStr(G, obj->AtomInfo[table_a->atom].name);
        if(matcher)
          hit_flag =
            WordMatcherMatchAlpha(matcher,
                                  name);
        else
          hit_flag = (WordMatchCommaExact(G, base[1].text(),
                                          name,
                                          ignore_case) < 0);

        if((*base_0_sele_a = hit_flag))
          c++;
        table_a++;
        base_0_sele_a++;
      }
      if(matcher)
        WordMatcherFree(matcher);
    }
    break;
  case SELE_TTYs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
	AtomInfoType * ai;
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

#ifndef NO_MMLIBS
#endif
        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          ai = i_obj[table_a->model]->AtomInfo + table_a->atom;
#ifndef NO_MMLIBS
#endif
          if((*base_0_sele_a = WordMatcherMatchAlpha(matcher, LexStr(G, ai->textType))))
	    c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_ELEs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchAlpha(matcher,
                                    i_obj[table_a->model]->AtomInfo[table_a->atom].elem)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_STRO:
    {
      CWordMatchOptions options;
      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];


      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];
#ifndef NO_MMLIBS
#endif
        for(a = cNDummyAtoms; a < I_NAtom; a++) {
#ifndef NO_MMLIBS
#endif
          const char * mmstereotype =
            AtomInfoGetStereoAsStr(i_obj[table_a->model]->AtomInfo + table_a->atom);
          if((*base_0_sele_a =
              WordMatcherMatchAlpha(matcher,mmstereotype)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_REPs:
    rep_mask = 0;
    WordPrimeCommaMatch(G, &base[1].m_text[0] /* replace '+' with ',' */);
    for(a = 0; rep_names[a].word[0]; a++) {
      if(WordMatchComma(G, base[1].text(), rep_names[a].word, ignore_case) < 0)
        rep_mask |= rep_names[a].value;
    }
    for(SelectorAtomIterator iter(I); iter.next();) {
      if(iter.getAtomInfo()->visRep & rep_mask) {
        base[0].sele[iter.a] = true;
        c++;
      } else {
        base[0].sele[iter.a] = false;
      }
    }
    break;
  case SELE_COLs:
    col_idx = ColorGetIndex(G, base[1].text());
    for(a = cNDummyAtoms; a < I_NAtom; a++) {
      base[0].sele[a] = false;
      if(i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].color == col_idx) {
        base[0].sele[a] = true;
        c++;
      }
    }
    break;
  case SELE_CCLs:
  case SELE_RCLs:
    // setting index
    index = (base->code == SELE_CCLs) ? cSetting_cartoon_color : cSetting_ribbon_color;
    col_idx = ColorGetIndex(G, base[1].text());
    for(a = cNDummyAtoms; a < I_NAtom; a++) {
      base[0].sele[a] = false;
      {
        AtomInfoType *ai = i_obj[i_table[a].model]->AtomInfo + i_table[a].atom;
        int value;
        if (AtomSettingGetIfDefined(G, ai, index, &value)) {
            if(value == col_idx) {
              base[0].sele[a] = true;
              c++;
            }
        }
      }
    }
    break;
  case SELE_CHNs:
  case SELE_SEGs:
  case SELE_CUST:
  case SELE_LABs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case_chain);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      int offset = 0;
      switch (base->code) {
        case SELE_CHNs:
          offset = offsetof(AtomInfoType, chain);
          break;
        case SELE_SEGs:
          offset = offsetof(AtomInfoType, segi);
          break;
        case SELE_CUST:
          offset = offsetof(AtomInfoType, custom);
          break;
        case SELE_LABs:
          offset = offsetof(AtomInfoType, label);
          break;
        default:
          printf("coding error: missing case\n");
      }

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchAlpha(matcher, LexStr(G,
                  *reinterpret_cast<lexidx_t*>
                  (((char*)(i_obj[table_a->model]->AtomInfo + table_a->atom)) + offset)
                  ))))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_SSTs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchAlpha(matcher,
                                    i_obj[table_a->model]->AtomInfo[table_a->atom].
                                    ssType)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_STAs:
    sscanf(base[1].text(), "%d", &state);
    state = state - 1;
    obj = NULL;

    if (state < 0 && state != cSelectorUpdateTableCurrentState) {
      PRINTFB(G, FB_Selector, FB_Errors)
        " Selector-Error: state %d unsupported (must be -1 (current) or >=1)\n",
        state + 1 ENDFB(G);
      ok = false;
    } else {
      auto state_arg = state;
      for(a = cNDummyAtoms; a < I_NAtom; a++) {
        base[0].sele[a] = false;
        obj = i_obj[i_table[a].model];
        if(obj != cur_obj) {    /* different object */
          if (state_arg == cSelectorUpdateTableCurrentState)
            state = obj->getState();
          if(state >= obj->NCSet)
            flag = false;
          else if(state < 0)
            flag = false;
          else if(!obj->CSet[state])
            flag = false;
          else {
            cs = obj->CSet[state];
            flag = true;        /* valid state */
          }
          cur_obj = obj;
        }
        if(flag && cs) {
          if(cs->atmToIdx(i_table[a].atom) >= 0) {
            base[0].sele[a] = true;
            c++;
          }
        }
      }
    }
    break;
  case SELE_ALTs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchAlpha(matcher,
                                    i_obj[table_a->model]->AtomInfo[table_a->atom].alt)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_FLGs:
    sscanf(base[1].text(), "%d", &flag);
    flag = (1 << flag);
    for(a = cNDummyAtoms; a < I_NAtom; a++) {
      if(i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].flags & flag) {
        base[0].sele[a] = true;
        c++;
      } else
        base[0].sele[a] = false;
    }
    break;
  case SELE_NTYs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigInteger(&options);

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          if((*base_0_sele_a =
              WordMatcherMatchInteger(matcher,
                                      i_obj[table_a->model]->AtomInfo[table_a->atom].
                                      customType)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_RSIs:
    {
      CWordMatchOptions options;
      AtomInfoType *ai;

      WordMatchOptionsConfigMixed(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          ai = i_obj[table_a->model]->AtomInfo + table_a->atom;
          char resi[8];
          AtomResiFromResv(resi, sizeof(resi), ai);
          if((*base_0_sele_a = WordMatcherMatchMixed(matcher, resi, ai->resv)))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_RSNs:
    {
      CWordMatchOptions options;

      WordMatchOptionsConfigAlphaList(&options, wildcard[0], ignore_case);

      table_a = i_table + cNDummyAtoms;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];

      if((matcher = WordMatcherNew(G, base[1].text(), &options, true))) {
        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];

        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          auto& resn = i_obj[table_a->model]->AtomInfo[table_a->atom].resn;
          if((*base_0_sele_a =
              WordMatcherMatchAlpha(matcher, LexStr(G, resn))))
            c++;
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      }
    }
    break;
  case SELE_SELs:
    {
      const char *word = base[1].text();
      WordType activeselename = "";
      int enabled_only = false;
      CWordMatchOptions options;

      if(word[0] == '?') {
        word++;
        if(word[0] == '?') {
          enabled_only = true;
          word++;
        }
      }
      WordMatchOptionsConfigAlpha(&options, wildcard[0], ignore_case);

      if((matcher = WordMatcherNew(G, word, &options, false))) {

        SelectorWordType *list = I->Name;
        int idx = 0;

        for(a = 0; a < I_NAtom; a++)    /* zero out first before iterating through selections */
          base[0].sele[a] = false;

        while(list[idx][0]) {
          if(WordMatcherMatchAlpha(matcher, list[idx])) {
            if((idx >= 0) &&
               ((!enabled_only) ||
                ExecutiveGetActiveSeleName(G, list[idx], false, false))) {
              MemberType *I_Member = I->Member;
              sele = I->Info[idx].ID;
              for(a = cNDummyAtoms; a < I_NAtom; a++) {
                s = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].selEntry;
                while(s) {
                  if(I_Member[s].selection == sele) {
                    if(!base[0].sele[a]) {
                      base[0].sele[a] = I_Member[s].tag;
                      c++;
                    }
                  }
                  s = I_Member[s].next;
                }
              }
            }
          }
          idx++;
        }
        WordMatcherFree(matcher);

        /* must also allow for group name pattern matches */

        {
          int group_list_id;
          if((group_list_id = ExecutiveGetExpandedGroupListFromPattern(G, word))) {
            int last_was_member = false;
            last_obj = NULL;
            for(a = cNDummyAtoms; a < I_NAtom; a++) {
              if(last_obj != i_obj[i_table[a].model]) {
                last_obj = i_obj[i_table[a].model];
                last_was_member = ExecutiveCheckGroupMembership(G,
                                                                group_list_id,
                                                                (CObject *) last_obj);
              }
              if(last_was_member && !base[0].sele[a]) {
                base[0].sele[a] = true;
                c++;
              }
            }
          }
          ExecutiveFreeGroupList(G, group_list_id);
        }

      } else if((!enabled_only) || ExecutiveGetActiveSeleName(G, activeselename, false, false)) {
        if (activeselename[0]) {
          // TODO not sure if this is intentional. If the active selection is
          // "foo", then the expression "??bar" will evaluate to "foo". I assume
          // the intention was to evaluate to the empty selection if "bar" is
          // not active, and to "bar" in case it's active.
          // Used with cmd.select(..., merge=2)
          base[1].m_text = activeselename;
          word = base[1].text();
        }
        sele = SelectGetNameOffset(G, word, 1, ignore_case);
        if(sele >= 0) {
          MemberType *I_Member = I->Member;
          sele = I->Info[sele].ID;
          for(a = cNDummyAtoms; a < I_NAtom; a++) {
            base[0].sele[a] = false;
            s = i_obj[i_table[a].model]->AtomInfo[i_table[a].atom].selEntry;
            while(s) {
              if(I_Member[s].selection == sele) {
                base[0].sele[a] = I_Member[s].tag;
                c++;
              }
              s = I_Member[s].next;
            }
          }
        } else {
          int group_list_id;
          if((group_list_id = ExecutiveGetExpandedGroupList(G, word))) {
            int last_was_member = false;
            last_obj = NULL;
            for(a = 0; a < I_NAtom; a++)        /* zero out first before iterating through selections */
              base[0].sele[a] = false;
            for(a = cNDummyAtoms; a < I_NAtom; a++) {
              if(last_obj != i_obj[i_table[a].model]) {
                last_obj = i_obj[i_table[a].model];
                last_was_member = ExecutiveCheckGroupMembership(G,
                                                                group_list_id,
                                                                (CObject *) last_obj);
              }
              if((base[0].sele[a] = last_was_member))
                c++;
            }
            ExecutiveFreeGroupList(G, group_list_id);
          } else if(base[1].m_text[0] == '?') {   /* undefined ?sele allowed */
            for(a = cNDummyAtoms; a < I_NAtom; a++)
              base[0].sele[a] = false;
          } else {
	      PRINTFB(G, FB_Selector, FB_Errors)
		"Selector-Error: Invalid selection name \"%s\".\n", word ENDFB(G);
            ok = false;
          }
        }
      }
    }
    break;
  case SELE_MODs:

    /* need to change this to handle wildcarded model names */

    /* first, trim off and record the atom index if one exists */

    index = -1;
    auto pos = base[1].m_text.find('`');
    if (pos != std::string::npos) {
      const char* np = base[1].text() + pos;
      if(sscanf(np + 1, "%d", &index) != 1)
        index = -1;
      else
        index--;
      base[1].m_text.resize(pos);
    }
    model = 0;

    {
      CWordMatchOptions options;
      WordMatchOptionsConfigAlpha(&options, wildcard[0], ignore_case);

      if((matcher = WordMatcherNew(G, base[1].text(), &options, false))) {

        int obj_matches = false;

        for(a = 0; a < I_NAtom; a++)    /* zero out first before iterating through selections */
          base[0].sele[a] = false;

        table_a = i_table + cNDummyAtoms;
        base_0_sele_a = &base[0].sele[cNDummyAtoms];
        last_obj = NULL;
        for(a = cNDummyAtoms; a < I_NAtom; a++) {
          obj = i_obj[table_a->model];
          if(obj != last_obj) {

            obj_matches = WordMatcherMatchAlpha(matcher, i_obj[table_a->model]->Name);
            last_obj = obj;
          }
          if(obj_matches) {
            if((index < 0) || (table_a->atom == index)) {
              *base_0_sele_a = true;
              c++;
            }
          }
          table_a++;
          base_0_sele_a++;
        }
        WordMatcherFree(matcher);
      } else {

        obj = (ObjectMolecule *) ExecutiveFindObjectByName(G, base[1].text());
        if(obj) {
          for(a = cNDummyModels; a < I->NModel; a++)
            if(i_obj[a] == obj) {
              model = a + 1;
              break;
            }
        }
        if(!model)
          if(sscanf(base[1].text(), "%i", &model) == 1) {
            if(model <= 0)
              model = 0;
            else if(model > I->NModel)
              model = 0;
            else if(!i_obj[model])
              model = 0;
          }
        if(model) {
          model--;
          if(index >= 0) {
            for(a = cNDummyAtoms; a < I_NAtom; a++) {
              if(i_table[a].model == model)
                if(i_table[a].atom == index) {
                  base[0].sele[a] = true;
                  c++;
                } else {
                  base[0].sele[a] = false;
              } else
                base[0].sele[a] = false;
            }
          } else {
            for(a = cNDummyAtoms; a < I_NAtom; a++) {
              if(i_table[a].model == model) {
                base[0].sele[a] = true;
                c++;
              } else
                base[0].sele[a] = false;
            }
          }
        } else {
          PRINTFB(G, FB_Selector, FB_Errors)
            " Selector-Error: invalid model \"%s\".\n", base[1].text() ENDFB(G);
          ok = false;
        }
      }
    }
    break;
  }
  PRINTFD(G, FB_Selector)
    " %s:  %d atoms selected.\n", __func__, c ENDFD;
  return (ok);
}


/*========================================================================*/
static int SelectorSelect2(PyMOLGlobals * G, EvalElem * base, int state)
{
  int a;
  int c = 0;
  int ok = true;
  int oper;
  float comp1;
  int exact;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);

  AtomInfoType *at1;
  CSelector *I = G->Selector;
  base->type = STYP_LIST;
  base->sele_calloc(I->NAtom);
  base->sele_err_chk_ptr(G);
  switch (base->code) {
  case SELE_XVLx:
  case SELE_YVLx:
  case SELE_ZVLx:
    oper = WordKey(G, AtOper, base[1].text(), 4, ignore_case, &exact);
    switch (oper) {
    case SCMP_GTHN:
    case SCMP_LTHN:
    case SCMP_EQAL:
      if(sscanf(base[2].text(), "%f", &comp1) != 1)
        ok = ErrMessage(G, "Selector", "Invalid Number");
      break;
    default:
      ok = ErrMessage(G, "Selector", "Invalid Operator.");
      break;
    }
    if(ok) {
      ObjectMolecule *obj;
      CoordSet *cs;
      int at, idx, s, s0 = 0, sN = I->NCSet;

      if(state != -1) {
        s0 = (state < -1) ? SceneGetState(G) : state;
        sN = s0 + 1;
      }

      for(a = cNDummyAtoms; a < I->NAtom; a++)
        base[0].sele[a] = false;

      for(s = s0; s < sN; s++) {
        for(a = cNDummyAtoms; a < I->NAtom; a++) {
          if(base[0].sele[a])
            continue;

          obj = I->Obj[I->Table[a].model];
          if(s >= obj->NCSet)
            continue;

          at = I->Table[a].atom;
          cs = obj->CSet[s];
          idx = cs->atmToIdx(at);
          if(idx < 0)
            continue;

          idx *= 3;
          switch (base->code) {
          case SELE_ZVLx:
            idx++;
          case SELE_YVLx:
            idx++;
          }

          base[0].sele[a] = fcmp(cs->Coord[idx], comp1, oper);
        }
      }
    }
    break;
  case SELE_PCHx:
  case SELE_FCHx:
  case SELE_BVLx:
  case SELE_QVLx:
    oper = WordKey(G, AtOper, base[1].text(), 4, ignore_case, &exact);
    if(!oper)
      ok = ErrMessage(G, "Selector", "Invalid Operator.");
    if(ok) {
      switch (oper) {
      case SCMP_GTHN:
      case SCMP_LTHN:
      case SCMP_EQAL:
        if(sscanf(base[2].text(), "%f", &comp1) != 1)
          ok = ErrMessage(G, "Selector", "Invalid Number");
        break;
      }
      if(ok) {
        switch (oper) {
        case SCMP_GTHN:
          switch (base->code) {
          case SELE_BVLx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->b > comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_QVLx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->q > comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_PCHx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->partialCharge > comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_FCHx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->formalCharge > comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          }
          break;
        case SCMP_LTHN:
          switch (base->code) {
          case SELE_BVLx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->b < comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_QVLx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->q < comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_PCHx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->partialCharge < comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_FCHx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(at1->formalCharge < comp1) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          }
          break;
        case SCMP_EQAL:
          switch (base->code) {
          case SELE_BVLx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(fabs(at1->b - comp1) < R_SMALL4) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_QVLx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(fabs(at1->q - comp1) < R_SMALL4) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_PCHx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(fabs(at1->partialCharge - comp1) < R_SMALL4) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          case SELE_FCHx:
            for(a = cNDummyAtoms; a < I->NAtom; a++) {
              at1 = &I->Obj[I->Table[a].model]->AtomInfo[I->Table[a].atom];
              if(fabs(at1->formalCharge - comp1) < R_SMALL4) {
                base[0].sele[a] = true;
                c++;
              } else {
                base[0].sele[a] = false;
              }
            }
            break;
          }
          break;
        }
        break;
      }
    }
  }

  PRINTFD(G, FB_Selector)
    " %s: %d atoms selected.\n", __func__, c ENDFD;
  return (ok);
}

/*========================================================================*/
static int SelectorSelect3(PyMOLGlobals * G, EvalElem * base, int state)
{
  switch (base->code) {
  case SELE_PROP:
    ErrMessage(G, "Selector", "properties (p.) not supported in Open-Source PyMOL");
    return false;
  }
  return true;
ok_except1:
  return false;
}

/*========================================================================*/

/*
 * Ring finder subroutine
 */
class SelectorRingFinder {
  CSelector * I;
  EvalElem * base;
  ObjectMolecule * obj;
  std::vector<int> indices;

  void recursion(int atm, int depth) {
    int atm_neighbor, offset, j;

    indices[depth] = atm;

    ITERNEIGHBORATOMS(obj->Neighbor, atm, atm_neighbor, j) {
      // check bond order
      if (obj->Bond[obj->Neighbor[j + 1]].order < 1)
        continue;

      // check if closing a ring of size >= 3
      if (depth > 1 && atm_neighbor == indices[0]) {
        // found ring, add it to the selection
        for (int i = 0; i <= depth; ++i)
          if ((offset = SelectorGetObjAtmOffset(I, obj, indices[i])) >= 0)
            base->sele[offset] = 1;
      } else if (depth < indices.size() - 1) {
        // check for undesired ring with start != 0
        int i = depth;
        while ((--i) >= 0)
          if (atm_neighbor == indices[i])
            break; // stop recursion
        if (i == -1) {
          recursion(atm_neighbor, depth + 1);
        }
      }
    }
  }

public:
  SelectorRingFinder(CSelector * I, EvalElem * base, int maxringsize=7) :
    I(I), base(base), obj(NULL), indices(maxringsize) {}

  /*
   * Does a depth-first search for all paths of length in range [3, maxringsize],
   * which lead back to `atm` and don't visit any atom twice.
   *
   * Modifies base[0].sele
   */
  void apply(ObjectMolecule * obj_, int atm) {
    if (obj != obj_) {
      obj = obj_;
      ObjectMoleculeUpdateNeighbors(obj);
    }

    recursion(atm, 0);
  }
};

/*========================================================================*/
static int SelectorLogic1(PyMOLGlobals * G, EvalElem * inp_base, int state)
{
  /* some cases in this function still need to be optimized
     for performance (see BYR1 for example) */

  CSelector *I = G->Selector;
  int a, b, tag;
  int c = 0;
  int flag;
  EvalElem *base = inp_base;
  AtomInfoType *at1, *at2;
  TableRec *i_table = I->Table, *table_a;
  ObjectMolecule **i_obj = I->Obj;
  int n_atom = I->NAtom;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);
  int n;
  int a0, a1, a2;
  ObjectMolecule *lastObj = NULL;

  base[0].sele = std::move(base[1].sele);
  base[0].type = STYP_LIST;
  switch (base->code) {
  case SELE_NOT1:
    {
      int *base_0_sele_a;

      base_0_sele_a = base[0].sele_data();
      for(a = 0; a < n_atom; a++) {
        if((*base_0_sele_a = !*base_0_sele_a))
          c++;
        base_0_sele_a++;
      }
    }
    break;
  case SELE_RING:
    {
      std::vector<bool> selemask(base[0].sele_data(), base[0].sele_data() + n_atom);
      SelectorRingFinder ringfinder(I, base);

      std::fill_n(base[0].sele_data(), n_atom, 0);

      for (SelectorAtomIterator iter(I); iter.next();) {
        if (selemask[iter.a])
          ringfinder.apply(iter.obj, iter.getAtm());
      }
    }
    break;
  case SELE_NGH1:
    base[1].sele = std::move(base[0].sele);
    base[0].sele_calloc(n_atom);

    table_a = i_table + cNDummyAtoms;
    for(a = cNDummyAtoms; a < n_atom; a++) {
      if((tag = base[1].sele[a])) {
        if(i_obj[table_a->model] != lastObj) {
          lastObj = i_obj[table_a->model];
          ObjectMoleculeUpdateNeighbors(lastObj);
        }
        a0 = table_a->atom;
        n = lastObj->Neighbor[a0];
        n++;
        while(1) {
          a1 = lastObj->Neighbor[n];
          if(a1 < 0)
            break;
          if((a2 = SelectorGetObjAtmOffset(I, lastObj, a1)) >= 0) {
            if(!base[1].sele[a2])
              base[0].sele[a2] = tag;
          }
          n += 2;
        }
      }
      table_a++;
    }
    base[1].sele_free();
    break;
  case SELE_BON1:
    base[1].sele = std::move(base[0].sele);
    base[0].sele_calloc(n_atom);
    table_a = i_table + cNDummyAtoms;
    for(a = cNDummyAtoms; a < n_atom; a++) {
      if((tag = base[1].sele[a])) {
        if(i_obj[table_a->model] != lastObj) {
          lastObj = i_obj[table_a->model];
          ObjectMoleculeUpdateNeighbors(lastObj);
        }
        a0 = table_a->atom;
        n = lastObj->Neighbor[a0];
        n++;
        while(1) {
          a1 = lastObj->Neighbor[n];
          if(a1 < 0)
            break;
          if((a2 = SelectorGetObjAtmOffset(I, lastObj, a1)) >= 0) {
            if(!base[0].sele[a2])
              base[0].sele[a2] = 1;
            n += 2;
          }
        }
      }
      table_a++;
    }
    base[1].sele_free();
    break;
  case SELE_BYO1:
    base[1].sele = std::move(base[0].sele);
    base[0].sele_calloc(n_atom);
    for(a = cNDummyAtoms; a < n_atom; a++) {
      if(base[1].sele[a]) {
        if(i_obj[i_table[a].model] != lastObj) {
          lastObj = i_obj[i_table[a].model];
          b = a;
          while(b >= 0) {
            if(i_obj[i_table[b].model] != lastObj)
              break;
            base[0].sele[b] = 1;
            b--;
          }
          b = a + 1;
          while(b < n_atom) {
            if(i_obj[i_table[b].model] != lastObj)
              break;
            base[0].sele[b] = 1;
            b++;
          }
        }
      }
    }
    base[1].sele_free();
    break;
  case SELE_BYR1:              /* ASSUMES atoms are sorted & grouped by residue */
  case SELE_CAS1:
    {
      int *base_0_sele = base[0].sele_data();
      int break_atom = -1;
      int last_tag = 0;
      table_a = i_table + cNDummyAtoms;
      for(a = cNDummyAtoms; a < n_atom; a++) {
        if((tag = base_0_sele[a]) && ((a >= break_atom) || (base_0_sele[a] != last_tag))) {
          at1 = &i_obj[table_a->model]->AtomInfo[table_a->atom];
          b = a - 1;
          while(b >= 0) {
            if(!base_0_sele[b]) {
              flag = false;
              if(table_a->model == i_table[b].model) {
                at2 = &i_obj[i_table[b].model]->AtomInfo[i_table[b].atom];
                if(AtomInfoSameResidue(G, at1, at2)) {
                            base_0_sele[b] = tag;
                            c++;
                            flag = 1;
                          }
              }
              if(!flag) {
                break;
              }
            }
            b--;
          }
          b = a + 1;
          while(b < n_atom) {
            if(!base_0_sele[b]) {
              flag = false;
              if(table_a->model == i_table[b].model) {
                at2 = &i_obj[i_table[b].model]->AtomInfo[i_table[b].atom];
                if(AtomInfoSameResidue(G, at1, at2)) {
                            base_0_sele[b] = tag;
                            c++;
                            flag = 1;
                          }
              }
              if(!flag) {
                break_atom = b - 1;
                last_tag = tag;
                break;
              }
            }
            b++;
          }
        }
        table_a++;
      }
      if(base->code == SELE_CAS1) {
        c = 0;
        table_a = i_table + cNDummyAtoms;
        for(a = cNDummyAtoms; a < n_atom; a++) {
          if(base_0_sele[a]) {
            base_0_sele[a] = false;

            if(i_obj[table_a->model]->AtomInfo[table_a->atom].protons == cAN_C)
              if(WordMatchExact(G, G->lex_const.CA,
                                     i_obj[table_a->model]->AtomInfo[table_a->atom].name,
                                     ignore_case)) {
                base_0_sele[a] = true;
                c++;
              }
          }
          table_a++;
        }
      }
    }
    break;
  case SELE_BYC1:              /* ASSUMES atoms are sorted & grouped by chain */
    {
      int *base_0_sele = base[0].sele_data();
      int break_atom_high = -1;
      int break_atom_low = 0;
      int last_tag = 0;
      table_a = i_table + cNDummyAtoms;
      for(a = cNDummyAtoms; a < n_atom; a++) {
        if((tag = base_0_sele[a])
           && ((a >= break_atom_high) || (base_0_sele[a] != last_tag))) {
          if(tag != last_tag)
            break_atom_low = 0;
          at1 = &i_obj[table_a->model]->AtomInfo[table_a->atom];
          b = a - 1;
          while(b >= break_atom_low) {
            if(!base_0_sele[b]) {
              flag = false;
              if(table_a->model == i_table[b].model) {
                at2 = &i_obj[i_table[b].model]->AtomInfo[i_table[b].atom];
                if(at1->chain == at2->chain)
                  if(at1->segi == at2->segi) {
                    base_0_sele[b] = tag;
                    c++;
                    flag = 1;
                  }
              }
              if(!flag) {
                break_atom_low = b + 1;
                break;
              }
            }
            b--;
          }
          if(b < 0)
            break_atom_low = 0;
          b = a + 1;
          while(b < n_atom) {
            if(!base_0_sele[b]) {
              flag = false;
              if(table_a->model == i_table[b].model) {
                at2 = &i_obj[i_table[b].model]->AtomInfo[i_table[b].atom];
                if(at1->chain == at2->chain)
                  if(at1->segi == at2->segi) {
                    base_0_sele[b] = tag;
                    c++;
                    flag = 1;
                  }
              }
              if(!flag) {
                break_atom_high = b - 1;
                last_tag = tag;
                break;
              }
            }
            b++;
          }
        }
        table_a++;
      }
    }
    break;
  case SELE_BYS1:              /* ASSUMES atoms are sorted & grouped by segi */
    {
      int *base_0_sele = base[0].sele_data();
      int break_atom_high = -1;
      int break_atom_low = 0;
      int last_tag = 0;
      table_a = i_table + cNDummyAtoms;
      for(a = cNDummyAtoms; a < n_atom; a++) {
        if((tag = base_0_sele[a])
           && ((a >= break_atom_high) || (base_0_sele[a] != last_tag))) {
          if(tag != last_tag)
            break_atom_low = 0;
          at1 = &i_obj[table_a->model]->AtomInfo[table_a->atom];
          b = a - 1;
          while(b >= break_atom_low) {
            if(!base_0_sele[b]) {
              flag = false;
              if(table_a->model == i_table[b].model) {
                at2 = &i_obj[i_table[b].model]->AtomInfo[i_table[b].atom];
                if(at1->segi == at2->segi) {
                  base_0_sele[b] = tag;
                  c++;
                  flag = 1;
                }
              }
              if(!flag) {
                break_atom_low = b + 1;
                break;
              }
            }
            b--;
          }
          b = a + 1;
          while(b < n_atom) {
            if(!base_0_sele[b]) {
              flag = false;
              if(table_a->model == i_table[b].model) {
                at2 = &i_obj[i_table[b].model]->AtomInfo[i_table[b].atom];
                if(at1->segi == at2->segi) {
                  base_0_sele[b] = tag;
                  c++;
                  flag = 1;
                }
              }
              if(!flag) {
                break_atom_high = b - 1;
                last_tag = tag;
                break;
              }
            }
            b++;
          }
        }
        table_a++;
      }
    }
    break;
  case SELE_BYF1:              /* first, identify all atom by fragment selection */
    {
      /* NOTE: this algorithm looks incompatible with selection
         tags...need to do some more thinking & work... */

      int n_frag = EditorGetNFrag(G);

      base[1].sele = std::move(base[0].sele);
      base[0].sele_calloc(n_atom);

      if(n_frag) {
        int a, f, at, s;
        int *fsele;
        WordType name;
        ObjectMolecule *obj;

        fsele = pymol::malloc<int>(n_frag + 1);

        for(f = 0; f < n_frag; f++) {
          sprintf(name, "%s%1d", cEditorFragPref, f + 1);
          fsele[f] = SelectorIndexByName(G, name);
        }

        /* mark atoms by fragment */
        for(a = 0; a < n_atom; a++) {
          at = i_table[a].atom;
          obj = i_obj[i_table[a].model];
          s = obj->AtomInfo[at].selEntry;
          for(f = 0; f < n_frag; f++) {
            if(SelectorIsMember(G, s, fsele[f])) {
              base[0].sele[a] = f + 1;
            }
          }
        }

        /* mark fragments we keep */
        for(f = 0; f <= n_frag; f++) {
          fsele[f] = 0;
        }
        for(a = 0; a < n_atom; a++) {
          int f = base[0].sele[a];
          if(base[1].sele[a] && f)
            fsele[f] = 1;
        }

        /* now set flags */
        for(a = 0; a < n_atom; a++) {
          c += (base[0].sele[a] = fsele[base[0].sele[a]]);
        }

        FreeP(fsele);
      }
      base[1].sele_free();
    }
    break;
  case SELE_BYM1:
    {
      int s;
      int c = 0;
      int a, at, a1, aa;
      ObjectMolecule *obj, *lastObj = NULL;
      int *stk;
      int stkDepth = 0;
      base[1].sele = std::move(base[0].sele);
      base[0].sele_calloc(n_atom);

      stk = VLAlloc(int, 50);

      for(a = 0; a < n_atom; a++) {
        if((tag = base[1].sele[a]) && (!base[0].sele[a])) {
          VLACheck(stk, int, stkDepth);
          stk[stkDepth] = a;
          stkDepth++;

          obj = i_obj[i_table[a].model];
          if(obj != lastObj) {
            lastObj = obj;
            ObjectMoleculeUpdateNeighbors(obj);
          }

          while(stkDepth) {     /* this will explore a tree */
            stkDepth--;
            a = stk[stkDepth];
            base[0].sele[a] = tag;
            c++;
            at = i_table[a].atom;       /* start walk from this location */

            /* add neighbors onto the stack */
            ITERNEIGHBORATOMS(obj->Neighbor, at, a1, s) {
              b = obj->Neighbor[s + 1];
              if (obj->Bond[b].order > 0) {
                if((aa = SelectorGetObjAtmOffset(I, obj, a1)) >= 0) {
                  if(!base[0].sele[aa]) {
                    VLACheck(stk, int, stkDepth);
                    stk[stkDepth] = aa; /* add index in selector space */
                    stkDepth++;
                  }
                }
              }
            }
          }
        }
      }
      base[1].sele_free();
      VLAFreeP(stk);
    }
    break;
  case SELE_BYX1:              /* by cell */
    base[1].sele = std::move(base[0].sele);
    base[0].sele_calloc(n_atom);
    {
      ObjectMolecule *obj;
      CoordSet *cs;
      int d, n1, at;
      for(d = 0; d < I->NCSet; d++) {
        if((state < 0) || (d == state)) {
          n1 = 0;
          for(a = 0; a < I->NAtom; a++) {
            I->Flag1[a] = false;
            at = I->Table[a].atom;
            obj = I->Obj[I->Table[a].model];
            if(d < obj->NCSet)
              cs = obj->CSet[d];
            else
              cs = NULL;
            if(cs) {
              CCrystal *cryst = cs->PeriodicBox.get();
              if((!cryst) && (obj->Symmetry))
                cryst = &obj->Symmetry->Crystal;
              if(cryst) {
                int idx;
                idx = cs->atmToIdx(at);
                if(idx >= 0) {
                  transform33f3f(cryst->RealToFrac, cs->Coord + (3 * idx),
                                 I->Vertex + 3 * a);
                  I->Flag1[a] = true;
                  n1++;
                }
              }
            }
          }
          if(n1) {
            MapType *map = MapNewFlagged(G, -1.1, I->Vertex, I->NAtom, NULL, I->Flag1);
            if(map) {
              int e, nCSet;
              MapSetupExpress(map);
              nCSet = SelectorGetArrayNCSet(G, base[1].sele, false);
              for(e = 0; e < nCSet; e++) {
                if((state < 0) || (e == state)) {
                  for(a = 0; a < I->NAtom; a++) {
                    if(base[1].sele[a]) {
                      at = I->Table[a].atom;
                      obj = I->Obj[I->Table[a].model];
                      if(e < obj->NCSet)
                        cs = obj->CSet[e];
                      else
                        cs = NULL;
                      if(cs) {
                        CCrystal *cryst = cs->PeriodicBox.get();
                        if((!cryst) && (obj->Symmetry))
                          cryst = &obj->Symmetry->Crystal;
                        if(cryst) {
                          int idx;
                          idx = cs->atmToIdx(at);
                          if(idx >= 0) {
                            float probe[3], probe_i[3];
                            int h, i, j, k, l;

                            transform33f3f(cryst->RealToFrac, cs->Coord + (3 * idx),
                                           probe);
                            MapLocus(map, probe, &h, &k, &l);
                            i = *(MapEStart(map, h, k, l));
                            if(i) {
                              j = map->EList[i++];

                              probe_i[0] = (int) floor(probe[0]);
                              probe_i[1] = (int) floor(probe[1]);
                              probe_i[2] = (int) floor(probe[2]);

                              while(j >= 0) {
                                if(!base[0].sele[j]) {
                                  float *tst = I->Vertex + 3 * j;
                                  base[0].sele[j] = ((probe_i[0] == (int) floor(tst[0]))
                                                     && (probe_i[1] ==
                                                         (int) floor(tst[1]))
                                                     && (probe_i[2] ==
                                                         (int) floor(tst[2])));
                                }
                                j = map->EList[i++];
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
              MapFree(map);
            }
          }
        }
      }
    }
    base[1].sele_free();
    break;
  case SELE_FST1:
    base[1].sele = std::move(base[0].sele);
    base[0].sele_calloc(n_atom);
    for(a = cNDummyAtoms; a < n_atom; a++) {
      if(base[1].sele[a]) {
        base[0].sele[a] = base[1].sele[a];      /* preserve tag */
        break;
      }
    }
    base[1].sele_free();
    break;
  case SELE_LST1:
    {
      int last = -1;
      base[1].sele = std::move(base[0].sele);
      base[0].sele_calloc(n_atom);
      for(a = cNDummyAtoms; a < n_atom; a++) {
        if(base[1].sele[a]) {
          last = a;
        }
      }
      if(last >= 0)
        base[0].sele[last] = base[1].sele[last];        /* preserve tag */
    }
    base[1].sele_free();
    break;
  }
  PRINTFD(G, FB_Selector)
    " %s: %d atoms selected.\n", __func__, c ENDFD;
  return (1);
}


/*========================================================================*/
static int SelectorLogic2(PyMOLGlobals * G, EvalElem * base)
{
  CSelector *I = G->Selector;
  int a, b, tag;
  int c = 0;
  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);
  int ignore_case_chain = SettingGetGlobal_b(G, cSetting_ignore_case_chain);
  int *base_0_sele_a, *base_2_sele_a;
  TableRec *i_table = I->Table, *table_a, *table_b;
  ObjectMolecule **i_obj = I->Obj;
  int n_atom = I->NAtom;

  AtomInfoType *at1, *at2;

  switch (base[1].code) {

  case SELE_OR_2:
  case SELE_IOR2:
    {
      base_0_sele_a = base[0].sele_data();
      base_2_sele_a = base[2].sele_data();

      for(a = 0; a < n_atom; a++) {
        if(((*base_0_sele_a) =
            (((*base_0_sele_a) >
              (*base_2_sele_a)) ? (*base_0_sele_a) : (*base_2_sele_a)))) {
          /* use higher tag */
          c++;
        }
        base_0_sele_a++;
        base_2_sele_a++;
      }
    }
    break;
  case SELE_AND2:

    base_0_sele_a = base[0].sele_data();
    base_2_sele_a = base[2].sele_data();

    for(a = 0; a < n_atom; a++) {
      if((*base_0_sele_a) && (*base_2_sele_a)) {
        (*base_0_sele_a) =
          (((*base_0_sele_a) > (*base_2_sele_a)) ? (*base_0_sele_a) : (*base_2_sele_a));
        /* use higher tag */
        c++;
      } else {
        (*base_0_sele_a) = 0;
      }
      base_0_sele_a++;
      base_2_sele_a++;
    }
    break;
  case SELE_ANT2:
    base_0_sele_a = base[0].sele_data();
    base_2_sele_a = base[2].sele_data();

    for(a = 0; a < n_atom; a++) {
      if((*base_0_sele_a) && !(*base_2_sele_a)) {
        c++;
      } else {
        (*base_0_sele_a) = 0;
      }
      base_0_sele_a++;
      base_2_sele_a++;
    }
    break;
  case SELE_IN_2:
    {
      int *base_2_sele_b;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];
      table_a = i_table + cNDummyAtoms;
      for(a = cNDummyAtoms; a < n_atom; a++) {
        if((tag = *base_0_sele_a)) {
          at1 = &i_obj[table_a->model]->AtomInfo[table_a->atom];
          *base_0_sele_a = 0;
          table_b = i_table + cNDummyAtoms;
          base_2_sele_b = &base[2].sele[cNDummyAtoms];
          for(b = cNDummyAtoms; b < n_atom; b++) {
            if(*base_2_sele_b) {
              at2 = &i_obj[table_b->model]->AtomInfo[table_b->atom];
              if(at1->resv == at2->resv)
                if(WordMatchExact(G, at1->chain, at2->chain, ignore_case_chain))
                  if(WordMatchExact(G, at1->name, at2->name, ignore_case))
                    if(WordMatchExact(G, at1->inscode, at2->inscode, ignore_case))
                      if(WordMatchExact(G, at1->resn, at2->resn, ignore_case))
                        if(WordMatchExact(G, at1->segi, at2->segi, ignore_case_chain)) {
                          *base_0_sele_a = tag;
                          break;
                        }
            }
            base_2_sele_b++;
            table_b++;
          }
        }
        if(*(base_0_sele_a++))
          c++;
        table_a++;
      }
    }
    break;
  case SELE_LIK2:
    {
      int *base_2_sele_b;
      base_0_sele_a = &base[0].sele[cNDummyAtoms];
      table_a = i_table + cNDummyAtoms;
      for(a = cNDummyAtoms; a < n_atom; a++) {
        if((tag = *base_0_sele_a)) {
          at1 = &i_obj[table_a->model]->AtomInfo[table_a->atom];
          *base_0_sele_a = 0;
          table_b = i_table + cNDummyAtoms;
          base_2_sele_b = &base[2].sele[cNDummyAtoms];
          for(b = cNDummyAtoms; b < n_atom; b++) {
            if(*base_2_sele_b) {
              at2 = &i_obj[table_b->model]->AtomInfo[table_b->atom];
              if(at1->resv == at2->resv)
                if(WordMatchExact(G, at1->name, at2->name, ignore_case))
                  if(WordMatchExact(G, at1->inscode, at2->inscode, ignore_case)) {
                    *base_0_sele_a = tag;
                    break;
                  }
            }
            base_2_sele_b++;
            table_b++;
          }
        }
        if(*(base_0_sele_a++))
          c++;
        table_a++;
      }
    }
    break;
  }
  base[2].sele_free();
  PRINTFD(G, FB_Selector)
    " %s: %d atoms selected.\n", __func__, c ENDFD;
  return (1);
}


/*========================================================================*/
int SelectorOperator22(PyMOLGlobals * G, EvalElem * base, int state)
{
  int c = 0;
  int a, d, e;
  CSelector *I = G->Selector;
  ObjectMolecule *obj;

  float dist;
  float *v2;
  CoordSet *cs;
  int ok = true;
  int nCSet;
  MapType *map;
  int i, j, h, k, l;
  int n1, at, idx;
  int code = base[1].code;

  if(state < 0) {
    switch (state) {
    case -2:
    case -3:
      state = SceneGetState(G);
      break;
    }
  }

  switch (code) {
  case SELE_WIT_:
  case SELE_BEY_:
  case SELE_NTO_:
    if(!sscanf(base[2].text(), "%f", &dist))
      ok = ErrMessage(G, "Selector", "Invalid distance.");
    if(ok) {
      if(dist < 0.0)
        dist = 0.0;

      /* copy starting mask */
      for(a = 0; a < I->NAtom; a++) {
        I->Flag2[a] = base[0].sele[a];
        base[0].sele[a] = false;
      }

      for(d = 0; d < I->NCSet; d++) {
        if((state < 0) || (d == state)) {
          n1 = 0;
          for(a = 0; a < I->NAtom; a++) {
            I->Flag1[a] = false;
            at = I->Table[a].atom;
            obj = I->Obj[I->Table[a].model];
            if(d < obj->NCSet)
              cs = obj->CSet[d];
            else
              cs = NULL;
            if(cs) {
              if(CoordSetGetAtomVertex(cs, at, I->Vertex + 3 * a)) {
                I->Flag1[a] = true;
                n1++;
              }
            }
          }
          if(n1) {
            map = MapNewFlagged(G, -dist, I->Vertex, I->NAtom, NULL, I->Flag1);
            if(map) {
              ok &= MapSetupExpress(map);
              nCSet = SelectorGetArrayNCSet(G, base[4].sele, false);
              for(e = 0; ok && e < nCSet; e++) {
                if((state < 0) || (e == state)) {
                  for(a = 0; a < I->NAtom; a++) {
                    if(base[4].sele[a]) {
                      at = I->Table[a].atom;
                      obj = I->Obj[I->Table[a].model];
                      if(e < obj->NCSet)
                        cs = obj->CSet[e];
                      else
                        cs = NULL;
                      if(cs) {
                        idx = cs->atmToIdx(at);
                        if(idx >= 0) {
                          v2 = cs->Coord + (3 * idx);
                          MapLocus(map, v2, &h, &k, &l);
                          i = *(MapEStart(map, h, k, l));
                          if(i) {
                            j = map->EList[i++];

                            while(j >= 0) {
                              if(!base[0].sele[j])
                                if(I->Flag2[j])
                                  if(within3f(I->Vertex + 3 * j, v2, dist)) {
                                    if((code != SELE_NTO_) || (!base[4].sele[j]))
                                      base[0].sele[j] = true;
                                  }
                              j = map->EList[i++];
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
              MapFree(map);
            }
          }
        }
      }
      if(code == SELE_BEY_) {
        for(a = 0; a < I->NAtom; a++) {
          if(I->Flag2[a])
            base[0].sele[a] = !base[0].sele[a];
        }
      }
      for(a = cNDummyAtoms; a < I->NAtom; a++)
        if(base[0].sele[a])
          c++;
    }
    break;
  }
  base[4].sele_free();
  PRINTFD(G, FB_Selector)
    " %s: %d atoms selected.\n", __func__, c ENDFD;
  return (1);
}

/**
 * Removes matching quotes from a string, at string start as well as after word
 * list separators ("+" and ","). Does not consider backslash escaping.
 *
 * Examples (not sure if all of these are intentional):
 * @verbatim
   "foo bar" -> foo bar
   'foo bar' -> foo bar
   "foo"+'bar' -> foo+bar
   "foo bar\" -> foo bar\       # backslash has no escape function
   "foo" "bar" -> foo "bar"     # second pair of quotes not after separator
   foo''+''bar -> foo''+bar     # first pair of quotes not after separator
   "foo"bar" -> foobar"         # third quote unmatched
   foo'+'bar -> foo'+'bar       # no matching quotes after separator
   @endverbatim
 */
static void remove_quotes(std::string& str)
{
  /* nasty */

  char *st = &str[0];
  char *p, *q;
  char *quote_start = NULL;
  char active_quote = 0;
  p = st;
  q = st;

  while(*p) {
    if(((*p) == 34) || ((*p) == 39)) {
      if(quote_start && (active_quote == *p)) { /* eliminate quotes... */
        while(quote_start < (q - 1)) {
          *(quote_start) = *(quote_start + 1);
          quote_start++;
        }
        q--;
        quote_start = NULL;
        p++;
        continue;
      } else if(quote_start) {
      } else {
        if(p == st) {           /* at start => real quote */
          quote_start = q;
          active_quote = *p;
        } else if((*(p - 1) == '+') || (*(p - 1) == ',')) {     /* after separator => real quote */
          quote_start = q;
          active_quote = *p;
        }
      }
    }
    if (q < p) {
      *q = *p;
    }
    ++q;
    ++p;
  }
  if (q < p) {
    str.resize(q - st);
  }
}

#define STACK_PUSH_VALUE(value) { \
  depth++; \
  VecCheck(Stack, depth); \
  e = Stack.data() + depth; \
  e->level = (level << 4) + 1; \
  e->imp_op_level = (imp_op_level << 4) + 1; \
  imp_op_level = level; \
  e->type = STYP_VALU; \
  e->m_text = value; \
  remove_quotes(e->m_text); \
}

#define STACK_PUSH_OPERATION(ocode) { \
  depth++; \
  VecCheck(Stack, depth); \
  e = Stack.data() + depth; \
  e->code = ocode; \
  e->level = (level << 4) + ((e->code & 0xF0) >> 4); \
  e->imp_op_level = (imp_op_level << 4) + 1; \
  imp_op_level = level; \
  e->type = (e->code & 0xF); \
}

/*========================================================================*/
sele_array_t SelectorEvaluate(PyMOLGlobals* G,
    std::vector<std::string>& word,
    int state, int quiet)
{
  int level = 0, imp_op_level = 0;
  int depth = 0;
  int a, b, c = 0;
  int ok = true;
  unsigned int code = 0;
  int valueFlag = 0;            /* are we expecting? */
  int opFlag, maxLevel;
  int totDepth = 0;
  int exact = 0;

  int ignore_case = SettingGetGlobal_b(G, cSetting_ignore_case);
  /* CFGs can efficiently be parsed by stacks; use a clean stack w/space
   * for 10 (was: 100) elements */
  EvalElem *e;
  auto Stack = std::vector<EvalElem>(10);

  /* converts all keywords into code, adds them into a operation list */
  while(ok && c < word.size()) {
    if(word[c][0] == '#') {
      if((!valueFlag) && (!level)) {
        word.resize(c);         /* terminate selection if we encounter a comment */
        break;
      }
    }
    switch (word[c][0]) {
    case 0:
      break;
    case '(':
      /* increase stack depth on open parens: (selection ((and token) blah)) */
      if(valueFlag)
        ok = ErrMessage(G, "Selector", "Misplaced (.");
      if(ok)
        level++;
      break;
    case ')':
      /* decrease stack depth */
      if(valueFlag)
        ok = ErrMessage(G, "Selector", "Misplaced ).");
      if(ok) {
        level--;
        if(level < 0)
          ok = ErrMessage(G, "Selector", "Syntax error.");
        else
          imp_op_level = level;
      }
      if(ok && depth)
        Stack[depth].level--;
      break;
    default:
      if(valueFlag > 0) {       /* standard operand */
        STACK_PUSH_VALUE(word[c]);
        valueFlag--;
      } else if(valueFlag < 0) {        /* operation parameter i.e. around X<-- */
        depth++;
        VecCheck(Stack, depth);
        e = Stack.data() + depth;
        e->level = (level << 4) + 1;
        e->imp_op_level = (imp_op_level << 4) + 1;
        imp_op_level = level;
        e->type = STYP_PVAL;
        e->m_text = word[c];
        valueFlag++;
      } else {                  /* possible keyword... */
        code = WordKey(G, Keyword, word[c].c_str(), 4, ignore_case, &exact);
        if(!code) {
          b = word[c].size() - 1;
          if((b > 2) && (word[c][b] == ';')) {
            /* kludge to accomodate unnec. ';' usage */
            word[c].resize(b);
            code = WordKey(G, Keyword, word[c].c_str(), 4, ignore_case, &exact);
          } else if(!word[c].compare(0, 2, "p.")) {
            // kludge to parse p.propertyname without space after p.
            code = SELE_PROP;
            exact = 1;
            word[c].erase(0, 2);
            c--;
          }
        }
        PRINTFD(G, FB_Selector)
          " Selector: code %x\n", code ENDFD;
        if((code > 0) && (!exact))
          if(SelectorIndexByName(G, word[c].c_str()) >= 0)
            code = 0;           /* favor selections over partial keyword matches */
        if(code) {
          /* this is a known operation */
          STACK_PUSH_OPERATION(code);
          switch (e->type) {
          case STYP_SEL0:
            valueFlag = 0;
            break;
          case STYP_SEL1:
            valueFlag = 1;
            break;
          case STYP_SEL2:
            valueFlag = 2;
            break;
          case STYP_SEL3:
            valueFlag = 3;
            break;
          case STYP_OPR1:
            valueFlag = 0;
            break;
          case STYP_OPR2:
            valueFlag = 0;
            break;
          case STYP_PRP1:
            valueFlag = -1;
            break;
          case STYP_OP22:
            valueFlag = -2;
            break;
          }
        } else {
          if((a = std::count(word[c].begin(), word[c].end(),
                  '/'))) { /* handle slash notation */
            if(a > 5) {
              ok = ErrMessage(G, "Selector", "too many slashes in macro");
              break;
            }

            // macro codes (some special cases apply! see code below)
            const int macrocodes[] = {SELE_SELs, SELE_SEGs, SELE_CHNs, SELE_RSNs, SELE_NAMs};

            // two code/value pairs to support resn`resi and name`alt
            int codes[] = {0, 0, 0}; // null-terminated
            char * values[2];

            std::string tmpKW = word[c];
            char* q = &tmpKW[0];

            // if macro starts with "/" then read from left, otherwise
            // read from right
            if(*q == '/') {
              a = 4;
              q++;
            }

            // loop over macro elements
            for(b = 0; q && *q; a--) {
              values[0] = q;

              // null-terminate current element
              if((q = strchr(q, '/')))
                *(q++) = '\0';

              // skip empty elements
              if(!*values[0])
                continue;

              codes[0] = macrocodes[4 - a];
              codes[1] = 0;

              // resn`resi or name`alt
              if (codes[0] == SELE_RSNs || codes[0] == SELE_NAMs) {
                char * backtick = strchr(values[0], '`');
                if (backtick) {
                  if (codes[0] == SELE_RSNs) {
                    codes[1] = SELE_RSIs;
                  } else {
                    codes[1] = SELE_ALTs;
                  }
                  values[1] = backtick + 1;
                  *backtick = '\0';
                } else if (codes[0] == SELE_RSNs
                    && values[0][0] >= '0'
                    && values[0][0] <= '9') {
                  // numeric -> resi
                  codes[0] = SELE_RSIs;
                }
              }

              for (int i = 0; codes[i]; ++i) {
                // skip empty and "*" values
                if (*values[i] && strcmp(values[i], "*")) {
                  if(b++)
                    STACK_PUSH_OPERATION(SELE_AND2);

                  STACK_PUSH_OPERATION(codes[i]);
                  STACK_PUSH_VALUE(values[i]);
                }
              }
            }

            // all-empty slash macro equals "all"
            if(!b)
              STACK_PUSH_OPERATION(SELE_ALLz);

          } else if(word[c].find('`') != std::string::npos) { /* handle <object`index> syntax */
            STACK_PUSH_OPERATION(SELE_MODs);
            valueFlag = 1;
            c--;
          } else {              /* handle <selection-name> syntax */
            STACK_PUSH_OPERATION(SELE_SELs);
            valueFlag = 1;
            c--;
          }
        }
      }
      break;
    }
    if(ok)
      c++;                      /* go onto next word */
  }
  if(level > 0){
      ok = ErrMessage(G, "Selector", "Malformed selection.");
  }
  if(ok) {                      /* this is the main operation loop */
    totDepth = depth;
    opFlag = true;
    maxLevel = -1;
    for(a = 1; a <= totDepth; a++) {
      if(Stack[a].level > maxLevel)
        maxLevel = Stack[a].level;
    }
    level = maxLevel;
    PRINTFD(G, FB_Selector)
      " Selector: maxLevel %d %d\n", maxLevel, totDepth ENDFD;
    if(level >= 0)
      while(ok) {               /* loop until all ops at all levels have been tried */

        /* order & efficiency of this algorithm could be improved... */

        PRINTFD(G, FB_Selector)
          " Selector: new cycle...\n" ENDFD;
        depth = 1;
        opFlag = true;
        while(ok && opFlag) {   /* loop through all entries looking for ops at the current level */
          opFlag = false;

          if(Stack[depth].level >= level) {
            Stack[depth].level = level; /* trim peaks */
          }
          if(ok)
            if(depth > 0)
              if((!opFlag) && (Stack[depth].type == STYP_SEL0)) {
                opFlag = true;
                ok = SelectorSelect0(G, &Stack[depth]);
              }
          if(ok)
            if(depth > 1)
              if(Stack[depth - 1].level >= Stack[depth].level) {
                if(ok && (!opFlag) && (Stack[depth - 1].type == STYP_SEL1)
                   && (Stack[depth].type == STYP_VALU)) {
                  /* 1 argument selection operator */
                  opFlag = true;
                  ok = SelectorSelect1(G, &Stack[depth - 1], quiet);
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 1] = std::move(Stack[a]);
                  totDepth--;
                } else if(ok && (!opFlag) && (Stack[depth - 1].type == STYP_OPR1)
                          && (Stack[depth].type == STYP_LIST)) {
                  /* 1 argument logical operator */
                  opFlag = true;
                  ok = SelectorLogic1(G, &Stack[depth - 1], state);
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 1] = std::move(Stack[a]);
                  totDepth--;
                } else if((Stack[depth - 1].type == STYP_LIST) &&
                          (Stack[depth].type == STYP_LIST) &&
                          (!((Stack[depth - 1].level & 0xF) ||
                             (Stack[depth].level & 0xF)))) {
                  /* two adjacent lists at zeroth priority level
                     for the scope (lowest nibble of level is
                     zero) is an implicit OR action */
                  VecCheck(Stack, totDepth + 1);
                  for(a = totDepth; a >= depth; a--)
                    Stack[a + 1] = std::move(Stack[a]);
                  totDepth++;
                  Stack[depth].type = STYP_OPR2;
                  Stack[depth].code = SELE_IOR2;
                  Stack[depth].level = Stack[depth].imp_op_level;
                  Stack[depth].m_text.clear();
                  if(level < Stack[depth].level)
                    level = Stack[depth].level;
                  opFlag = true;
                }
              }
          if(ok)
            if(depth > 2)
              if((Stack[depth - 1].level >= Stack[depth].level) &&
                 (Stack[depth - 1].level >= Stack[depth - 2].level)) {

                if(ok && (!opFlag) && (Stack[depth - 1].type == STYP_OPR2)
                   && (Stack[depth].type == STYP_LIST)
                   && (Stack[depth - 2].type == STYP_LIST)) {
                  /* 2 argument logical operator */
                  ok = SelectorLogic2(G, &Stack[depth - 2]);
                  opFlag = true;
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 2] = std::move(Stack[a]);
                  totDepth -= 2;
                } else if(ok && (!opFlag) && (Stack[depth - 1].type == STYP_PRP1)
                          && (Stack[depth].type == STYP_PVAL)
                          && (Stack[depth - 2].type == STYP_LIST)) {
                  /* 2 argument logical operator */
                  ok = SelectorModulate1(G, &Stack[depth - 2], state);
                  opFlag = true;
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 2] = std::move(Stack[a]);
                  totDepth -= 2;
                }
              }
          if(ok)
            if(depth > 2)
              if((Stack[depth - 2].level >= Stack[depth - 1].level) &&
                 (Stack[depth - 2].level >= Stack[depth].level)) {

                if(ok && (!opFlag) && (Stack[depth - 2].type == STYP_SEL2)
                   && (Stack[depth - 1].type == STYP_VALU)
                   && (Stack[depth].type == STYP_VALU)) {
                  /* 2 argument value operator */
                  ok = SelectorSelect2(G, &Stack[depth - 2], state);
                  opFlag = true;
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 2] = std::move(Stack[a]);
                  totDepth -= 2;
                }
              }
          if(ok)
            if(depth > 3)
              if((Stack[depth - 3].level >= Stack[depth].level) &&
                 (Stack[depth - 3].level >= Stack[depth - 1].level) &&
                 (Stack[depth - 3].level >= Stack[depth - 2].level)) {

                if(ok && (!opFlag) && (Stack[depth - 3].type == STYP_SEL3)
                   && (Stack[depth].type == STYP_VALU)
                   && (Stack[depth - 1].type == STYP_VALU)
                   && (Stack[depth - 2].type == STYP_VALU)) {
                  /* 2 argument logical operator */
                  ok = SelectorSelect3(G, &Stack[depth-3], state);
                  opFlag = true;
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 3] = std::move(Stack[a]);
                  totDepth -= 3;
                }
              }
          if(ok)
            if(depth > 4)
              if((Stack[depth - 3].level >= Stack[depth].level) &&
                 (Stack[depth - 3].level >= Stack[depth - 1].level) &&
                 (Stack[depth - 3].level >= Stack[depth - 2].level) &&
                 (Stack[depth - 3].level >= Stack[depth - 4].level)) {

                if(ok && (!opFlag) && (Stack[depth - 3].type == STYP_OP22)
                   && (Stack[depth - 1].type == STYP_VALU)
                   && (Stack[depth - 2].type == STYP_VALU)
                   && (Stack[depth].type == STYP_LIST)
                   && (Stack[depth - 4].type == STYP_LIST)) {

                  ok = SelectorOperator22(G, &Stack[depth - 4], state);
                  opFlag = true;
                  for(a = depth + 1; a <= totDepth; a++)
                    Stack[a - 4] = std::move(Stack[a]);
                  totDepth -= 4;
                }

              }
          if(opFlag) {
            depth = 1;          /* start back at the left hand side */
          } else {
            depth = depth + 1;
            opFlag = true;
            if(depth > totDepth)
              break;
          }
        }
        if(level)
          level--;
        else
          break;
      }
    depth = totDepth;
  }
  if(ok) {
    if(depth != 1) {
	ok = ErrMessage(G, "Selector", "Malformed selection.");
    } else if(Stack[depth].type != STYP_LIST)
	ok = ErrMessage(G, "Selector", "Invalid selection.");
    else
      return std::move(Stack[totDepth].sele); /* return the selection list */
  }
  if(!ok) {
    for (a = 0; a <= c && a < word.size(); a++) {
      const char* space = (a && word[a][0]) ? " " : "";
      PRINTFB(G, FB_Selector, FB_Errors)
        "%s%s", space, word[a].c_str() ENDFB(G);
    }
    PRINTFB(G, FB_Selector, FB_Errors)
      "<--\n" ENDFB(G);
  }
  return {};
}


/*========================================================================*/
/**
 * Break a selection down into tokens and return them in a vector.
 * E.g. "(name CA+CB)" -> {"(", "name", "CA+CB", ")"}.
 * @param s selection expression to parse
 * @return tokens
 */
std::vector<std::string> SelectorParse(PyMOLGlobals * G, const char *s)
{
  int w_flag = false;
  int quote_flag = false;
  char quote_char = '"';
  const char *p = s;
  std::string* q = nullptr;
  std::vector<std::string> r;
  while(*p) {
    if(w_flag) {                /* currently in a word, thus q is a valid pointer */
      if(quote_flag) {
        if(*p != quote_char) {
          *q += *p;
        } else {
          quote_flag = false;
          *q += *p;
        }
      } else
        switch (*p) {
        case ' ':
          w_flag = false;
          break;
        case ';':              /* special word terminator */
          *q += *p;
          w_flag = false;
          break;
        case '!':              /* single words */
        case '&':
        case '|':
        case '(':
        case ')':
        case '>':
        case '<':
        case '=':
        case '%':
          r.emplace_back(1, *p); /* add new word */
          q = &r.back();
          w_flag = false;
          break;
        case '"':
          quote_flag = true;
          *q += *p;
          break;
        default:
          *q += *p;
          break;
        }
    } else {                    /*outside a word -- q is undefined */

      switch (*p) {
      case '!':                /* single words */
      case '&':
      case '|':
      case '(':
      case ')':
      case '>':
      case '<':
      case '=':
      case '%':
        r.emplace_back(1, *p); /* add new word */
        q = &r.back();
        break;
      case ' ':
        break;
      case '"':
        quote_flag = true;
        quote_char = *p;
        w_flag = true;
        r.emplace_back(1, *p); /* add new word */
        q = &r.back();
        break;
      default:
        w_flag = true;
        r.emplace_back(1, *p); /* add new word */
        q = &r.back();
        break;
      }
    }
    p++;
  }

  if(Feedback(G, FB_Selector, FB_Debugging)) {
    for (auto& word : r) {
      fprintf(stderr, "word: %s\n", word.c_str());
    }
  }
  return (r);
}


/*========================================================================*/
void SelectorFree(PyMOLGlobals * G)
{
  SelectorFreeImpl(G, G->Selector, 1);
}

void SelectorFreeImpl(PyMOLGlobals * G, CSelector *I, short init2)
{
  SelectorCleanImpl(G, I);
  if(I->Origin)
    DeleteP(I->Origin);
  if(I->Center)
    DeleteP(I->Center);
  if (init2){
    VLAFreeP(I->Member);
    VLAFreeP(I->Name);
    VLAFreeP(I->Info);

    OVLexicon_DEL_AUTO_NULL(I->Lex);
    OVOneToAny_DEL_AUTO_NULL(I->Key);
    OVOneToOne_DEL_AUTO_NULL(I->NameOffset);
  }
  FreeP(I);
}


/*========================================================================*/

void SelectorMemoryDump(PyMOLGlobals * G)
{
  CSelector *I = G->Selector;
  printf(" SelectorMemory: NSelection %d\n", I->NSelection);
  printf(" SelectorMemory: NActive %d\n", I->NActive);
  printf(" SelectorMemory: TmpCounter %d\n", I->TmpCounter);
  printf(" SelectorMemory: NMember %d\n", I->NMember);
}

static void SelectorInit2(PyMOLGlobals * G, CSelector *I)
{
  I->NSelection = 0;
  I->NActive = 0;
  I->TmpCounter = 0;
  I->NCSet = 0;

  I->Lex = OVLexicon_New(G->Context->heap);
  I->Key = OVOneToAny_New(G->Context->heap);
  I->NameOffset = OVOneToOne_New(G->Context->heap);

  {                             /* create placeholder "all" selection, which is selection 0
                                   and "none" selection, which is selection 1 */
    int n;

    n = I->NActive;
    VLACheck(I->Name, SelectorWordType, n + 1);
    VLACheck(I->Info, SelectionInfoRec, n + 1);
    strcpy(I->Name[n], cKeywordAll);    /* "all" selection = 0 */
    I->Name[n + 1][0] = 0;
    SelectorAddName(G, n);
    SelectionInfoInit(I->Info + n);
    I->Info[n].ID = I->NSelection++;
    I->NActive++;

    n = I->NActive;
    VLACheck(I->Name, SelectorWordType, n + 1);
    VLACheck(I->Info, SelectionInfoRec, n + 1);
    strcpy(I->Name[n], cKeywordNone);   /* "none" selection = 1 */
    I->Name[n + 1][0] = 0;
    SelectorAddName(G, n);
    SelectionInfoInit(I->Info + n);
    I->Info[n].ID = I->NSelection++;
    I->NActive++;
  }

  if(I->Lex && I->Key) {
    int a = 0;
    OVreturn_word result;
    while(1) {
      if(!Keyword[a].word[0])
        break;
      if(OVreturn_IS_OK((result = OVLexicon_GetFromCString(I->Lex, Keyword[a].word)))) {
        OVOneToAny_SetKey(I->Key, result.word, Keyword[a].value);
      }
      a++;
    }

  }
}

void SelectorReinit(PyMOLGlobals * G)
{
  CSelector *I = G->Selector;
  SelectorClean(G);

  OVLexicon_DEL_AUTO_NULL(I->Lex);
  OVOneToAny_DEL_AUTO_NULL(I->Key);
  OVOneToOne_DEL_AUTO_NULL(I->NameOffset);

  SelectorInit2(G, I);
}


/*========================================================================*/
int SelectorInit(PyMOLGlobals * G)
{
  return SelectorInitImpl(G, &G->Selector, 1);
}

int SelectorInitImpl(PyMOLGlobals * G, CSelector **Iarg, short init2){
  CSelector *I = NULL;
  ok_assert(1, I = pymol::calloc<CSelector>(1));

    *Iarg = I;

    I->Vertex = NULL;
    I->Origin = NULL;
    I->Table = NULL;
    I->Obj = NULL;
    I->Flag1 = NULL;
    I->Flag2 = NULL;

    if (init2){
      I->Member = (MemberType *) VLAMalloc(100, sizeof(MemberType), 5, true);
      I->NMember = 0;
      I->FreeMember = 0;
      I->Name = VLAlloc(SelectorWordType, 10);
      I->Info = VLAlloc(SelectionInfoRec, 10);
      SelectorInit2(G, I);
    } else {
      CSelector *GI = G->Selector;
      I->Member = GI->Member;
      I->NMember = GI->NMember;
      I->FreeMember = GI->FreeMember;
      I->NSelection = GI->NSelection;
      I->NActive = GI->NActive ;
      I->TmpCounter = GI->TmpCounter;
      I->NCSet = GI->NCSet;
      I->Lex = GI->Lex;
      I->Key = GI->Key;
      I->NameOffset = GI->NameOffset;
      I->Name = GI->Name;
      I->Info = GI->Info;      
    }
    return 1;
ok_except1:
 return 0;
}
 

/*========================================================================*/

DistSet *SelectorGetDistSet(PyMOLGlobals * G, DistSet * ds,
                            int sele1, int state1, int sele2, int state2,
                            int mode, float cutoff, float *result)
{
  CSelector *I = G->Selector;
  int *vla = NULL;
  int c;
  float dist;
  int a1, a2;
  AtomInfoType *ai1, *ai2;
  int at, at1, at2;
  CoordSet *cs1, *cs2;
  ObjectMolecule *obj, *obj1, *obj2, *lastObj;
  int idx1, idx2;
  int a;
  int nv = 0;
  float *vv = NULL, *vv0, *vv1;
  float dist_sum = 0.0;
  int dist_cnt = 0;
  int s;
  int a_keeper = false;
  int *zero = NULL, *scratch = NULL;
  std::vector<bool> coverage;
  HBondCriteria hbcRec, *hbc;
  int exclusion = 0;
  int bonds_only = 0;
  int from_proton = SettingGetGlobal_b(G, cSetting_h_bond_from_proton);
  AtomInfoType *h_ai;
  CMeasureInfo *atom1Info=NULL;

  /* if we're creating hydrogen bonds, then set some distance cutoffs */
  switch (mode) {
  case 1:
    bonds_only = 1;
    break;
  case 2:
    exclusion = SettingGetGlobal_i(G, cSetting_h_bond_exclusion);
    break;
  case 3:
    exclusion = SettingGetGlobal_i(G, cSetting_distance_exclusion);
    break;
  }

  hbc = &hbcRec;
  *result = 0.0;
  /* if the dist set exists, get info from it, otherwise get a new one */
  if(!ds) {
    ds = DistSetNew(G);
  } else {
    vv = ds->Coord;  /* vertices */
    nv = ds->NIndex; /* number of vertices */
  }
  /* make sure we have memory to hold the vertex info for this distance set */
  if(!vv) {
    vv = VLAlloc(float, 10);
  }

  /* update states: if the two are the same, update that one state, else update all states */
  if((state1 < 0) || (state2 < 0) || (state1 != state2)) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, state1, -1);
  }

  /* find and prepare (neighbortables) in any participating Molecular objects */
  if((mode == 1) || (mode == 2) || (mode == 3)) {       /* fill in all the neighbor tables */
    int max_n_atom = I->NAtom;
    lastObj = NULL;
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      /* foreach atom in the session, get its identifier and ObjectMolecule to which it belongs */
      at = I->Table[a].atom;  /* grab the atom ID from the Selectors->Table */
      obj = I->Obj[I->Table[a].model];		/* -- JV -- quick way to get an object from an atom */
      s = obj->AtomInfo[at].selEntry;  /* grab the selection entry# from this Atoms Info */
      if(obj != lastObj) {
        if(max_n_atom < obj->NAtom)
          max_n_atom = obj->NAtom;
	/* if the current atom is in sele1 or sele2 then update it's object's neighbor table */
        if(SelectorIsMember(G, s, sele1) || SelectorIsMember(G, s, sele2)) {
          ObjectMoleculeUpdateNeighbors(obj);
	  /* if hbonds (so, more than just distance) */
          if(mode == 2)
            ObjectMoleculeVerifyChemistry(obj, -1);
          lastObj = obj;
        }
      }
    }
    /* prepare these for the next round */
    zero = pymol::calloc<int>(max_n_atom);
    scratch = pymol::malloc<int>(max_n_atom);
  }

  /* if we're hydrogen bonding, setup the cutoff */
  if(mode == 2) {
    ObjectMoleculeInitHBondCriteria(G, hbc);
    if(cutoff < 0.0F) {
      cutoff = hbc->maxDistAtMaxAngle;
      if(cutoff < hbc->maxDistAtZero) {
        cutoff = hbc->maxDistAtZero;
      }
    }
  }
  if(cutoff < 0)
    cutoff = 1000.0;
	
  if (mode == 4) {
    // centroid distance
    float centroid1[3], centroid2[3];
    ObjectMoleculeOpRec op;

    // get centroid 1
    ObjectMoleculeOpRecInit(&op);
    op.code = OMOP_CSetSumVertices;
    op.cs1 = state1;
    ExecutiveObjMolSeleOp(G, sele1, &op);

    if (op.i1 > 0) {
      scale3f(op.v1, 1.f / op.i1, centroid1);

      // get centroid 2
      ObjectMoleculeOpRecInit(&op);
      op.code = OMOP_CSetSumVertices;
      op.cs1 = state2;
      ExecutiveObjMolSeleOp(G, sele2, &op);

      if (op.i1 > 0) {
        scale3f(op.v1, 1.f / op.i1, centroid2);

        // store positions in measurement object
        VLACheck(vv, float, (nv * 3) + 6);
        vv0 = vv + (nv * 3);
        nv += 2;
        copy3f(centroid1, vv0);
        copy3f(centroid2, vv0 + 3);

        // for the return value
        dist_cnt = 1;
        dist_sum = diff3f(centroid1, centroid2);
      }
    }

    // skip searching for pairwise distances
    c = 0;
  } else {
    /* coverage determines if a given atom appears in sel1 and sel2 */
    coverage.resize(I->NAtom);

    for (SelectorAtomIterator iter(I); iter.next();) {
      s = iter.getAtomInfo()->selEntry;
      if (SelectorIsMember(G, s, sele1) &&
          SelectorIsMember(G, s, sele2))
        coverage[iter.a] = true;
    }

    /* this creates an interleaved list of ints for mapping ids to states within a given neighborhood */
    c = SelectorGetInterstateVLA(G, sele1, state1, sele2, state2, cutoff, &vla);
  }

  /* for each state */
  for(a = 0; a < c; a++) {
    atom1Info = NULL;
    /* get the interstate atom identifier for the two atoms to distance */
    a1 = vla[a * 2];
    a2 = vla[a * 2 + 1];

    /* check their coverage to avoid duplicates */
    if(a1 < a2 || (a1 != a2 && !(coverage[a1] && coverage[a2]))
        || (state1 != state2)) {  /* eliminate reverse duplicates */
      /* get the object-local atom ID */
      at1 = I->Table[a1].atom;
      at2 = I->Table[a2].atom;
      /* get the object for this global atom ID */
      obj1 = I->Obj[I->Table[a1].model];
      obj2 = I->Obj[I->Table[a2].model];

      /* the states are valid for these two atoms */
      if((state1 < obj1->NCSet) && (state2 < obj2->NCSet)) {
	/* get the coordinate sets for both atoms */
        cs1 = obj1->CSet[state1];
        cs2 = obj2->CSet[state2];
        if(cs1 && cs2) {
	  /* for bonding */
          float *don_vv = NULL;
          float *acc_vv = NULL;

	  /* grab the appropriate atom information for this object-local atom */
          ai1 = obj1->AtomInfo + at1;
          ai2 = obj2->AtomInfo + at2;
	  
          idx1 = cs1->atmToIdx(at1);
          idx2 = cs2->atmToIdx(at2);

          if((idx1 >= 0) && (idx2 >= 0)) {
	    /* actual distance calculation from ptA to ptB */
            dist = (float) diff3f(cs1->Coord + 3 * idx1, cs2->Coord + 3 * idx2);

	    /* if we pass the boding cutoff */
            if(dist < cutoff) {
              float h_crd[3];
              h_ai = NULL;

              a_keeper = true;
              if(exclusion && (obj1 == obj2)) {
                a_keeper = !SelectorCheckNeighbors(G, exclusion,
                                                   obj1, at1, at2, zero, scratch);
              } else if(bonds_only) {
                a_keeper = SelectorCheckNeighbors(G, 1, obj1, at1, at2, zero, scratch);
              }
              if(a_keeper && (mode == 2)) {
		/* proton comes from ai1 */
                if(ai1->hb_donor && ai2->hb_acceptor) {
                  a_keeper = ObjectMoleculeGetCheckHBond(&h_ai, h_crd,
							 obj1, at1, state1, 
							 obj2, at2, state2,
							 hbc);
                  if(a_keeper) {
                    if(h_ai && from_proton) {
                      don_vv = h_crd;
                      ai1 = h_ai;
		    }
                    else {
                      don_vv = cs1->Coord + 3 * idx1;
		    }
                    acc_vv = cs2->Coord + 3 * idx2;
                  }
                } else if(ai1->hb_acceptor && ai2->hb_donor) {
		  /* proton comes from ai2 */
                  a_keeper = ObjectMoleculeGetCheckHBond(&h_ai, h_crd,
							 obj2, at2, state2,
							 obj1, at1, state1, 
							 hbc);

                  if(a_keeper) {
                    if(h_ai && from_proton) {
                      don_vv = h_crd;
                      ai2 = h_ai;
		    }
                    else {
                      don_vv = cs2->Coord + 3 * idx2;
		    }
		    acc_vv = cs1->Coord + 3 * idx1;
                  }
                } else {
                  a_keeper = false;
                }
	      }
              if((sele1 == sele2) && (at1 > at2))
                a_keeper = false;

              if(a_keeper) {

		/* Insert DistInfo records for updating distances */
		/* Init/Add the elem to the DistInfo list */
                atom1Info = pymol::malloc<CMeasureInfo>(1);

                // TH
                atom1Info->id[0] = AtomInfoCheckUniqueID(G, ai1);
                atom1Info->id[1] = AtomInfoCheckUniqueID(G, ai2);

		atom1Info->offset = nv;  /* offset into this DSet's Coord */
		atom1Info->state[0] = state1;  /* state1 of sel1 */
		atom1Info->state[1] = state2;
		atom1Info->measureType = cRepDash; /* DISTANCE-dash */
		ListPrepend(ds->MeasureInfo, atom1Info, next);

		/* we have a distance we want to keep */
                dist_cnt++;
                dist_sum += dist;
		/* see if vv has room at another 6 floats */
                VLACheck(vv, float, (nv * 3) + 6);
                vv0 = vv + (nv * 3);
		
                if((mode == 2) && (don_vv) && (acc_vv)) {
                  *(vv0++) = *(don_vv++);
                  *(vv0++) = *(don_vv++);
                  *(vv0++) = *(don_vv++);
                  *(vv0++) = *(acc_vv++);
                  *(vv0++) = *(acc_vv++);
                  *(vv0++) = *(acc_vv++);
                } else {
                  vv1 = cs1->Coord + 3 * idx1;
                  *(vv0++) = *(vv1++);
                  *(vv0++) = *(vv1++);
                  *(vv0++) = *(vv1++);
                  vv1 = cs2->Coord + 3 * idx2;
                  *(vv0++) = *(vv1++);
                  *(vv0++) = *(vv1++);
                  *(vv0++) = *(vv1++);
                }

                nv += 2;
              }
            }
          }
        }
      }
    }
  }
  if(dist_cnt)
    (*result) = dist_sum / dist_cnt;
  VLAFreeP(vla);
  FreeP(zero);
  FreeP(scratch);
  if(vv)
    VLASize(vv, float, (nv + 1) * 3);
  ds->NIndex = nv;
  ds->Coord = vv;
  return (ds);
}

DistSet *SelectorGetAngleSet(PyMOLGlobals * G, DistSet * ds,
                             int sele1, int state1,
                             int sele2, int state2,
                             int sele3, int state3,
                             int mode, float *angle_sum, int *angle_cnt)
{
  CSelector *I = G->Selector;
  float *vv = NULL;
  int nv = 0;
  std::vector<bool> coverage;

  if(!ds) {
    ds = DistSetNew(G);
  } else {
    vv = ds->AngleCoord;
    nv = ds->NAngleIndex;
  }
  if(!vv)
    vv = VLAlloc(float, 10);

  if((state1 < 0) || (state2 < 0) || (state3 < 0) || (state1 != state2)
     || (state1 != state3)) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, state1, -1);
  }

  /* which atoms are involved? */

  {
    int a, s, at;
    ObjectMolecule *obj;

    coverage.resize(I->NAtom);
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      at = I->Table[a].atom;
      obj = I->Obj[I->Table[a].model];
      s = obj->AtomInfo[at].selEntry;
      if (SelectorIsMember(G, s, sele1) &&
          SelectorIsMember(G, s, sele3))
        coverage[a] = true;
    }
  }

  {                             /* fill in neighbor tables */
    int a, s, at;
    ObjectMolecule *obj, *lastObj = NULL;
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      at = I->Table[a].atom;
      obj = I->Obj[I->Table[a].model];
      s = obj->AtomInfo[at].selEntry;
      if(obj != lastObj) {
        if(SelectorIsMember(G, s, sele1) ||
           SelectorIsMember(G, s, sele2) || SelectorIsMember(G, s, sele3)) {
          ObjectMoleculeUpdateNeighbors(obj);
          lastObj = obj;
        }
      }
    }
  }

  {
    int a, s, at;
    ObjectMolecule *obj;
    int *list1 = VLAlloc(int, 1000);
    int *list2 = VLAlloc(int, 1000);
    int *list3 = VLAlloc(int, 1000);
    int n1 = 0;
    int n2 = 0;
    int n3 = 0;
    int bonded12, bonded23;

    /* now generate three lists of atoms, one for each selection set */

    if(list1 && list2 && list3) {
      for(a = cNDummyAtoms; a < I->NAtom; a++) {
        at = I->Table[a].atom;
        obj = I->Obj[I->Table[a].model];
        s = obj->AtomInfo[at].selEntry;
        if(SelectorIsMember(G, s, sele1)) {
          VLACheck(list1, int, n1);
          list1[n1++] = a;
        }
        if(SelectorIsMember(G, s, sele2)) {
          VLACheck(list2, int, n2);
          list2[n2++] = a;
        }
        if(SelectorIsMember(G, s, sele3)) {
          VLACheck(list3, int, n3);
          list3[n3++] = a;
        }
      }

      /* for each set of 3 atoms in each selection... */

      {
        int i1, i2, i3;
        int a1, a2, a3;
        int at1, at2, at3;

        /*        AtomInfoType *ai1,*ai2,ai3; */
        CoordSet *cs1, *cs2, *cs3;
        ObjectMolecule *obj1, *obj2, *obj3;

        int idx1, idx2, idx3;
        float angle;
        float d1[3], d2[3];
        float *v1, *v2, *v3, *vv0;

        CMeasureInfo *atom1Info=NULL;

        for(i1 = 0; i1 < n1; i1++) {
          a1 = list1[i1];
          at1 = I->Table[a1].atom;
          obj1 = I->Obj[I->Table[a1].model];

          if(state1 < obj1->NCSet) {
            cs1 = obj1->CSet[state1];

            if(cs1) {
              idx1 = cs1->atmToIdx(at1);

              if(idx1 >= 0) {

                for(i2 = 0; i2 < n2; i2++) {
                  a2 = list2[i2];
                  at2 = I->Table[a2].atom;
                  obj2 = I->Obj[I->Table[a2].model];

                  if(state2 < obj2->NCSet) {

                    cs2 = obj2->CSet[state2];

                    if(cs2) {
                      idx2 = cs2->atmToIdx(at2);

                      if(idx2 >= 0) {
			/* neighbor table like BPRec */
                        bonded12 = ObjectMoleculeAreAtomsBonded2(obj1, at1, obj2, at2);

                        for(i3 = 0; i3 < n3; i3++) {
			  atom1Info = NULL;
                          a3 = list3[i3];

                          if( (a1 != a2 || state1 != state2) &&
                              (a2 != a3 || state2 != state3) &&
                              (a1 != a3 || state1 != state3)) {
                            if(!(coverage[a1] && coverage[a3])
                               || (a1 < a3)
                               || (state1 != state3)) {  /* eliminate alternate-order duplicates */

                              at3 = I->Table[a3].atom;
                              obj3 = I->Obj[I->Table[a3].model];

                              if(state3 < obj3->NCSet) {

                                cs3 = obj3->CSet[state3];

                                if(cs3) {
                                  idx3 = cs3->atmToIdx(at3);

                                  if(idx3 >= 0) {

                                    bonded23 =
                                      ObjectMoleculeAreAtomsBonded2(obj2, at2, obj3, at3);

                                    if(!mode || ((mode == 1) && (bonded12 && bonded23))) {
                                      /* store the 3 coordinates */

                                      v1 = cs1->Coord + 3 * idx1;
                                      v2 = cs2->Coord + 3 * idx2;
                                      v3 = cs3->Coord + 3 * idx3;

                                      subtract3f(v1, v2, d1);
                                      subtract3f(v3, v2, d2);

				      /* Insert DistInfo records for updating distances */
				      /* Init/Add the elem to the DistInfo list */
				      atom1Info = pymol::malloc<CMeasureInfo>(1);

                                      // TH
                                      atom1Info->id[0] = AtomInfoCheckUniqueID(G, obj1->AtomInfo + at1);
                                      atom1Info->id[1] = AtomInfoCheckUniqueID(G, obj2->AtomInfo + at2);
                                      atom1Info->id[2] = AtomInfoCheckUniqueID(G, obj3->AtomInfo + at3);

				      atom1Info->offset = nv;  /* offset into this DSet's Coord */
				      atom1Info->state[0] = state1;  /* state1 of sel1 */
				      atom1Info->state[1] = state2;
				      atom1Info->state[2] = state3;
				      atom1Info->measureType = cRepAngle;
				      ListPrepend(ds->MeasureInfo, atom1Info, next);

                                      angle = get_angle3f(d1, d2);

                                      (*angle_sum) += angle;
                                      (*angle_cnt)++;

                                      VLACheck(vv, float, (nv * 3) + 14);
                                      vv0 = vv + (nv * 3);
                                      *(vv0++) = *(v1++);
                                      *(vv0++) = *(v1++);
                                      *(vv0++) = *(v1++);
                                      *(vv0++) = *(v2++);
                                      *(vv0++) = *(v2++);
                                      *(vv0++) = *(v2++);
                                      *(vv0++) = *(v3++);
                                      *(vv0++) = *(v3++);
                                      *(vv0++) = *(v3++);
                                      *(vv0++) = (float) !bonded12;
                                      /* show line 1 flag */
                                      *(vv0++) = (float) !bonded23;
                                      *(vv0++) = 0.0F;
                                      *(vv0++) = 0.0F;  /* label x relative to v2 */
                                      *(vv0++) = 0.0F;  /* label y relative to v2 */
                                      *(vv0++) = 0.0F;  /* label z relative to v2 */
                                      nv += 5;
                                    }
                                  }
                                }
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    VLAFreeP(list1);
    VLAFreeP(list2);
    VLAFreeP(list3);
  }

  if(vv)
    VLASize(vv, float, (nv + 1) * 3);
  ds->NAngleIndex = nv;
  ds->AngleCoord = vv;
  return (ds);
}

DistSet *SelectorGetDihedralSet(PyMOLGlobals * G, DistSet * ds,
                                int sele1, int state1,
                                int sele2, int state2,
                                int sele3, int state3,
                                int sele4, int state4,
                                int mode, float *angle_sum, int *angle_cnt)
{
  CSelector *I = G->Selector;
  float *vv = NULL;
  int nv = 0;
  std::vector<bool> coverage14;
  std::vector<bool> coverage23;
  ObjectMolecule *just_one_object = NULL;
  int just_one_atom[4] = { -1, -1, -1, -1 };

  CMeasureInfo *atom1Info;

  if(!ds) {
    ds = DistSetNew(G);
  } else {
    vv = ds->DihedralCoord;
    nv = ds->NDihedralIndex;
  }
  if(!vv)
    vv = VLAlloc(float, 10);

  if((state1 < 0) || (state2 < 0) || (state3 < 0) || (state4 < 0) ||
     (state1 != state2) || (state1 != state3) || (state1 != state4)) {
    SelectorUpdateTable(G, cSelectorUpdateTableAllStates, -1);
  } else {
    SelectorUpdateTable(G, state1, -1);
  }

  /* which atoms are involved? */

  {
    int a, s, at;
    ObjectMolecule *obj;

    coverage14.resize(I->NAtom);
    coverage23.resize(I->NAtom);
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      bool coverage1 = false;
      bool coverage2 = false;
      at = I->Table[a].atom;
      obj = I->Obj[I->Table[a].model];
      if(!a)
        just_one_object = obj;
      s = obj->AtomInfo[at].selEntry;
      if(SelectorIsMember(G, s, sele1)) {
        if(obj != just_one_object)
          just_one_object = NULL;
        else if(just_one_atom[0] == -1)
          just_one_atom[0] = a;
        else
          just_one_atom[0] = -2;
        coverage1 = true;
      }
      if(SelectorIsMember(G, s, sele2)) {
        if(obj != just_one_object)
          just_one_object = NULL;
        else if(just_one_atom[1] == -1)
          just_one_atom[1] = a;
        else
          just_one_atom[1] = -2;
        coverage2 = true;
      }
      if(SelectorIsMember(G, s, sele3)) {
        if(obj != just_one_object)
          just_one_object = NULL;
        else if(just_one_atom[2] == -1)
          just_one_atom[2] = a;
        else
          just_one_atom[2] = -2;
        coverage23[a] = coverage2;
      }
      if(SelectorIsMember(G, s, sele4)) {
        if(obj != just_one_object)
          just_one_object = NULL;
        else if(just_one_atom[3] == -1)
          just_one_atom[3] = a;
        else
          just_one_atom[3] = -2;
        coverage14[a] = coverage1;
      }
    }
  }

  if(just_one_object) {
    ObjectMoleculeUpdateNeighbors(just_one_object);
  } else {                      /* fill in neighbor tables */
    int a, s, at;
    ObjectMolecule *obj, *lastObj = NULL;
    for(a = cNDummyAtoms; a < I->NAtom; a++) {
      at = I->Table[a].atom;
      obj = I->Obj[I->Table[a].model];
      s = obj->AtomInfo[at].selEntry;
      if(obj != lastObj) {
        if(SelectorIsMember(G, s, sele1) ||
           SelectorIsMember(G, s, sele2) ||
           SelectorIsMember(G, s, sele3) || SelectorIsMember(G, s, sele4)
          ) {
          ObjectMoleculeUpdateNeighbors(obj);
          lastObj = obj;
        }
      }
    }
  }

  {
    int a, s, at;
    ObjectMolecule *obj;
    int *list1 = VLAlloc(int, 1000);
    int *list2 = VLAlloc(int, 1000);
    int *list3 = VLAlloc(int, 1000);
    int *list4 = VLAlloc(int, 1000);
    int n1 = 0;
    int n2 = 0;
    int n3 = 0;
    int n4 = 0;
    int bonded12, bonded23, bonded34;

    /* now generate three lists of atoms, one for each selection set */

    if(list1 && list2 && list3 && list4) {

      if(just_one_object && (just_one_atom[0] >= 0) && (just_one_atom[1] >= 0)
         && (just_one_atom[2] >= 0) && (just_one_atom[3] >= 0)) {
        /* optimal case */

        list1[0] = just_one_atom[0];
        list2[0] = just_one_atom[1];
        list3[0] = just_one_atom[2];
        list4[0] = just_one_atom[3];

        n1 = n2 = n3 = n4 = 1;

      } else {

        for(a = cNDummyAtoms; a < I->NAtom; a++) {
          at = I->Table[a].atom;
          obj = I->Obj[I->Table[a].model];
          s = obj->AtomInfo[at].selEntry;
          if(SelectorIsMember(G, s, sele1)) {
            VLACheck(list1, int, n1);
            list1[n1++] = a;
          }
          if(SelectorIsMember(G, s, sele2)) {
            VLACheck(list2, int, n2);
            list2[n2++] = a;
          }
          if(SelectorIsMember(G, s, sele3)) {
            VLACheck(list3, int, n3);
            list3[n3++] = a;
          }
          if(SelectorIsMember(G, s, sele4)) {
            VLACheck(list4, int, n4);
            list4[n4++] = a;
          }
        }
      }

      /* for each set of 3 atoms in each selection... */

      {
        int i1, i2, i3, i4;
        int a1, a2, a3, a4;
        int at1, at2, at3, at4;

        /*        AtomInfoType *ai1,*ai2,ai3; */
        CoordSet *cs1, *cs2, *cs3, *cs4;
        ObjectMolecule *obj1, *obj2, *obj3, *obj4;

        int idx1, idx2, idx3, idx4;
        float angle;
        float *v1, *v2, *v3, *v4, *vv0;

        for(i1 = 0; i1 < n1; i1++) {
          a1 = list1[i1];
          at1 = I->Table[a1].atom;
          obj1 = I->Obj[I->Table[a1].model];
          if(state1 < obj1->NCSet) {
            cs1 = obj1->CSet[state1];

            if(cs1) {
              idx1 = cs1->atmToIdx(at1);

              if(idx1 >= 0) {

                for(i2 = 0; i2 < n2; i2++) {
                  a2 = list2[i2];
                  at2 = I->Table[a2].atom;
                  obj2 = I->Obj[I->Table[a2].model];

                  if(state2 < obj2->NCSet) {

                    cs2 = obj2->CSet[state2];

                    if(cs2) {
                      idx2 = cs2->atmToIdx(at2);

                      if(idx2 >= 0) {

                        bonded12 = ObjectMoleculeAreAtomsBonded2(obj1, at1, obj2, at2);

                        if(!mode || ((mode == 1) && bonded12))
                          for(i3 = 0; i3 < n3; i3++) {
                            a3 = list3[i3];
                            at3 = I->Table[a3].atom;
                            obj3 = I->Obj[I->Table[a3].model];

                            if(state3 < obj3->NCSet) {

                              cs3 = obj3->CSet[state3];

                              if(cs3) {
                                idx3 = cs3->atmToIdx(at3);

                                if(idx3 >= 0) {

                                  bonded23 =
                                    ObjectMoleculeAreAtomsBonded2(obj2, at2, obj3, at3);
                                  if(!mode || ((mode == 1) && bonded23))
                                    for(i4 = 0; i4 < n4; i4++) {
				      atom1Info = NULL;
                                      a4 = list4[i4];

                                      if((a1 != a2) && (a1 != a3) && (a1 != a4)
                                         && (a2 != a3) && (a2 != a4) && (a3 != a4)) {
                                        if (!(coverage14[a1] &&
                                              coverage14[a4] &&
                                              coverage23[a2] &&
                                              coverage23[a3])
                                           || (a1 < a4)) {
                                          /* eliminate alternate-order duplicates */

                                          at4 = I->Table[a4].atom;
                                          obj4 = I->Obj[I->Table[a4].model];

                                          if(state4 < obj4->NCSet) {

                                            cs4 = obj4->CSet[state4];

                                            if(cs4) {
                                              idx4 = cs3->atmToIdx(at4);

                                              if(idx4 >= 0) {

                                                bonded34 =
                                                  ObjectMoleculeAreAtomsBonded2(obj3, at3,
                                                                                obj4,
                                                                                at4);

                                                if(!mode || ((mode == 1) && bonded34)) {
                                                  /* store the 3 coordinates */

                                                  v1 = cs1->Coord + 3 * idx1;
                                                  v2 = cs2->Coord + 3 * idx2;
                                                  v3 = cs3->Coord + 3 * idx3;
                                                  v4 = cs4->Coord + 3 * idx4;

						  /* Insert DistInfo records for updating distances */
						  /* Init/Add the elem to the DistInfo list */
						  atom1Info = pymol::malloc<CMeasureInfo >(1);

                                                  // TH
                                                  atom1Info->id[0] = AtomInfoCheckUniqueID(G, obj1->AtomInfo + at1);
                                                  atom1Info->id[1] = AtomInfoCheckUniqueID(G, obj2->AtomInfo + at2);
                                                  atom1Info->id[2] = AtomInfoCheckUniqueID(G, obj3->AtomInfo + at3);
                                                  atom1Info->id[3] = AtomInfoCheckUniqueID(G, obj4->AtomInfo + at4);

						  atom1Info->offset = nv;  /* offset into this DSet's Coord */

						  atom1Info->state[0] = state1;  /* state1 of sel1 */
						  atom1Info->state[1] = state2;
						  atom1Info->state[2] = state3;
						  atom1Info->state[3] = state4;

						  atom1Info->measureType = cRepDihedral;

						  ListPrepend(ds->MeasureInfo, atom1Info, next);

                                                  angle = get_dihedral3f(v1, v2, v3, v4);

                                                  (*angle_sum) += angle;
                                                  (*angle_cnt)++;

                                                  VLACheck(vv, float, (nv * 3) + 17);
                                                  vv0 = vv + (nv * 3);
                                                  ObjectMoleculeGetAtomTxfVertex(obj1,
                                                                                 state1,
                                                                                 at1,
                                                                                 vv0);
                                                  ObjectMoleculeGetAtomTxfVertex(obj2,
                                                                                 state2,
                                                                                 at2,
                                                                                 vv0 + 3);
                                                  ObjectMoleculeGetAtomTxfVertex(obj3,
                                                                                 state3,
                                                                                 at3,
                                                                                 vv0 + 6);
                                                  ObjectMoleculeGetAtomTxfVertex(obj4,
                                                                                 state4,
                                                                                 at4,
                                                                                 vv0 + 9);
                                                  vv0 += 12;
                                                  *(vv0++) = (float) !bonded12;
                                                  *(vv0++) = (float) !bonded23;
                                                  *(vv0++) = (float) !bonded34;
                                                  *(vv0++) = 0.0F;      /* label x relative to v2+v3/2 */
                                                  *(vv0++) = 0.0F;      /* label y relative to v2+v3/2 */
                                                  *(vv0++) = 0.0F;      /* label z relative to v2+v3/2 */
                                                  nv += 6;
                                                }
                                              }
                                            }
                                          }
                                        }
                                      }
                                    }
                                }
                              }
                            }
                          }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    VLAFreeP(list1);
    VLAFreeP(list2);
    VLAFreeP(list3);
    VLAFreeP(list4);
  }

  if(vv)
    VLASize(vv, float, (nv + 1) * 3);
  ds->NDihedralIndex = nv;
  ds->DihedralCoord = vv;
  return (ds);
}


/*========================================================================*/


/*

example selections
cas
backbone
(model 1 and backbone)
(name ca)
(resi 123)
(resi 200:400 and chain A)
(model a)

 */


/* In order for selections to be robust during atom insertions
   deletions, they are stored not as lists of selected atoms, but
   rather in the inverse - as atoms with selection membership
   information */


/* Each atom points into the selection heap, a series of linked
   entries where each member is selection index */


/* Definition of the selection language:

	<sele> = [(] [<not>] <prop> <val-range> [<qual> [<qual-range>] ] [)] { <SEL1> <sele> }

	Example selections:
	
   name ca
	( name ca )
	name ca around 5 {all atoms within 5 angstroms of any ca atom) }
	( resi 10 ) 
	resi 10:50
	resi 10A:50A
	resi 10,50
	chain A
	segi A
	model 1 and segi B
	model a and segi B
	not name ca

*/


/* Selection processing, left to right by default, but with parenthesis for grouping
	stack based functional language processing
	each stack level has a full selection matrix
	logical binary operators (or,and) and negation SEL1ation solely on these matrices.
*/


/*

(not (name ca or name c) and (name s around 5 and (name c) )) around 6

0:
1: not
2: name 
2: ca

0:

1: not
2:<name ca>
not name ca around 5

force compute

0:
1: not

attrib b < 0 

*/
