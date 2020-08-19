/*  ------------------------------------------------------------------
    Copyright (c) 2011-2020 Marc Toussaint
    email: toussaint@tu-berlin.de

    This code is distributed under the MIT License.
    Please see <root-path>/LICENSE for details.
    --------------------------------------------------------------  */

#pragma once

#include "kin.h"
#include "../Geo/geo.h"

struct F_Link {
  int ID=-1;
  int type=-1;
  int qIndex=-1;
  int parent=-1;
  rai::Transformation X=0, Q=0;
  rai::Vector com=0, force=0, torque=0;
  double mass=0.;
  rai::Matrix inertia=0;
  uint dof();

  arr _h, _Q, _I, _f; //featherstone types

  F_Link() {}
  void setFeatherstones();
  void updateFeatherstones();
  void write(ostream& os) const {
    os <<"*type=" <<type <<" index=" <<qIndex <<" parent=" <<parent <<endl
       <<" XQ,Q=" <<X <<", " <<Q <<endl
       <<" cft=" <<com <<force <<torque <<endl
       <<" mass=" <<mass <<inertia <<endl;
  }
};
stdOutPipe(F_Link)

typedef rai::Array<F_Link> F_LinkTree;

struct FeatherstoneInterface {
  rai::Configuration& K;

  FrameL sortedFrames;

  rai::Array<F_Link> tree;

  FeatherstoneInterface(rai::Configuration& K):K(K) { sortedFrames = K.calc_topSort(); }

  void setGravity(double g=-9.81);
  void update();

  void equationOfMotion(arr& M, arr& F,  const arr& qd);
  void fwdDynamics_MF(arr& qdd, const arr& qd, const arr& u);
  void fwdDynamics_aba_nD(arr& qdd, const arr& qd, const arr& tau);
  void fwdDynamics_aba_1D(arr& qdd, const arr& qd, const arr& tau);
  void invDynamics(arr& tau, const arr& qd, const arr& qdd);
};
