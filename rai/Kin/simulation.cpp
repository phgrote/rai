/*  ------------------------------------------------------------------
    Copyright (c) 2011-2020 Marc Toussaint
    email: toussaint@tu-berlin.de

    This code is distributed under the MIT License.
    Please see <root-path>/LICENSE for details.
    --------------------------------------------------------------  */

#include "simulation.h"
#include "frame.h"
#include "proxy.h"
#include "kin_bullet.h"
#include "kin_physx.h"
#include "F_geometrics.h"
#include "switch.h"
#include "F_collisions.h"
#include "../Gui/opengl.h"
#include "../Algo/SplineCtrlFeed.h"

//#define BACK_BRIDGE

namespace rai {

//===========================================================================

struct Simulation_self {
  arr qdot;
  arr frameVelocities;
  std::shared_ptr<struct Simulation_DisplayThread> display;
  std::shared_ptr<CameraView> cameraview;
  std::shared_ptr<BulletInterface> bullet;
  std::shared_ptr<PhysXInterface> physx;
#ifdef BACK_BRIDGE
  std::shared_ptr<BulletBridge> bulletBridge;
  rai::Configuration bridgeC;
#endif

  SplineCtrlReference ref;

  void updateDisplayData(double _time, const Configuration& _C);
  void updateDisplayData(const byteA& _image, const floatA& _depth);
};

//===========================================================================

struct SimulationState {
  arr frameState;
  arr frameVels;

  SimulationState(const arr& _frameState, const arr& _frameVels) : frameState(_frameState), frameVels(_frameVels) {}
};

//===========================================================================

struct SimulationImp {
  enum When { _undefined, _beforeControl, _beforePhysics, _afterPhysics, _afterImages };

  Simulation::ImpType type;
  When when = _undefined;
  bool killMe = false;

  //-- imps overload these methods to modify/perturb something
  virtual void modControl(Simulation& S, arr& u_control, double& tau, Simulation::ControlMode u_mode) {}
  virtual void modConfiguration(Simulation& S, double tau) {}
  virtual void modImages(Simulation& S, byteA& image, floatA& depth) {}
};

//===========================================================================

struct Imp_CloseGripper : SimulationImp {
  Frame* gripper, *fing1, *fing2, *obj, *finger1, *finger2;
  std::unique_ptr<F_PairCollision> coll1;
  std::unique_ptr<F_PairCollision> coll2;
  double q;
  double speed;

  Imp_CloseGripper(Frame* _gripper, Frame* _fing1, Frame* _fing2, Frame* _obj, double _speed);
  virtual void modConfiguration(Simulation& S, double tau);
};

//===========================================================================

struct Imp_OpenGripper : SimulationImp {
  Frame* gripper, *fing1, *fing2;
  double q;
  double speed;

  Imp_OpenGripper(Frame* _gripper, Frame* _fing1, Frame* _fing2, double _speed);
  virtual void modConfiguration(Simulation& S, double tau);
};

//===========================================================================

struct Imp_ObjectImpulses : SimulationImp {
  Frame* obj;
  double timeToImpulse=0.;

  Imp_ObjectImpulses(Frame* _obj) : obj(_obj) { CHECK(obj, "");  when = _beforePhysics;  }
  virtual void modConfiguration(Simulation& S, double tau);
};

//===========================================================================

struct Imp_BlockJoints : SimulationImp {
  FrameL joints;
  arr qBlocked;

  Imp_BlockJoints(const FrameL& _joints, Simulation& S);
  virtual void modConfiguration(Simulation& S, double tau);
};

//===========================================================================

Simulation::Simulation(Configuration& _C, Simulation::SimulatorEngine _engine, int _verbose)
  : self(make_unique<Simulation_self>()),
    C(_C),
    time(0.),
    engine(_engine),
    verbose(_verbose) {
  if(engine==_physx) {
    self->physx = make_shared<PhysXInterface>(C, verbose-1);
  } else if(engine==_bullet) {
    self->bullet = make_shared<BulletInterface>(C, rai::Bullet_Options().set_verbose(verbose-1));
#ifdef BACK_BRIDGE
    self->bulletBridge = make_shared<BulletBridge>(self->bullet->getDynamicsWorld());
    self->bulletBridge->getConfiguration(self->bridgeC);
    self->bulletBridge->pullPoses(self->bridgeC, true);
#endif
  } else if(engine==_kinematic) {
    //nothing
  } else NIY;
  self->ref.initialize(C.getJointState(), NoArr, time);
  if(verbose>0) self->display = make_shared<Simulation_DisplayThread>(C);
}

Simulation::~Simulation() {
}

void Simulation::step(const arr& u_control, double tau, ControlMode u_mode) {
  //-- kill done imps
  for(uint i=imps.N; i--;) {
    if(imps.elem(i)->killMe) imps.remove(i);
  }

  arr ucontrol = u_control; //a copy to allow for perturbations

  //-- imps before control
  for(ptr<SimulationImp>& imp : imps) if(imp->when==SimulationImp::_beforeControl) {
      imp->modControl(*this, ucontrol, tau, u_mode);
    }

  //-- perform control using C
  time += tau;
  if(u_mode==_none) {
  } else if(u_mode==_position) {
    C.setJointState(ucontrol);
  } else if(u_mode==_velocity) {
    arr q = C.getJointState();
    q += tau * ucontrol;
    C.setJointState(q);
  } else if(u_mode==_spline) {
    arr q = C.getJointState();
    self->ref.getReference(q, NoArr, NoArr, q, NoArr, time);
    C.setJointState(q);
  } else NIY;

  //-- imps before physics
  for(ptr<SimulationImp>& imp : imps) if(imp->when==SimulationImp::_beforePhysics) {
      imp->modConfiguration(*this, tau);
    }

  //-- call the physics ending
  if(engine==_physx) {
    self->physx->pushKinematicStates(C.frames);
    self->physx->step(tau);
    self->physx->pullDynamicStates(C.frames, self->frameVelocities);
  } else if(engine==_bullet) {
    self->bullet->pushKinematicStates(C.frames);
    self->bullet->step(tau);
    self->bullet->pullDynamicStates(C.frames, self->frameVelocities);
#ifdef BACK_BRIDGE
    self->bulletBridge->pullPoses(self->bridgeC, true);
    self->bridgeC.watch(false, "bullet bridge");
#endif
  } else if(engine==_kinematic) {
  } else NIY;

  //-- imps after physics
  for(ptr<SimulationImp>& imp : imps) if(imp->when==SimulationImp::_afterPhysics) {
      imp->modConfiguration(*this, tau);
    }

  if(verbose>0) self->updateDisplayData(time, C);
}

void Simulation::setMoveTo(const arr& x, double t, bool append){
  arr path = x;
  if(x.nd==1) path.reshape(1,x.d0);

  arr times = {t};
  if(x.nd==2) times = range(0., t, x.d0-1);

  if(append) self->ref.append(path, times, time, true);
  else self->ref.overrideSmooth(~x, {t}, time);
}

void Simulation::move(const arr& path, const arr& t){
  self->ref.append(path, t, time, true);
}

bool getFingersForGripper(rai::Frame*& gripper, rai::Frame*& fing1, rai::Frame*& fing2, rai::Configuration& C, const char* gripperFrameName) {
  gripper = C.getFrame(gripperFrameName);
  if(!gripper) {
    LOG(-1) <<"you passed me a non-existing gripper name!";
    gripper=fing1=fing2=0;
    return false;
  }
  gripper = gripper->getUpwardLink();
  //browse all children of the gripper and find by name
  FrameL F;
  gripper->getSubtree(F);
  for(rai::Frame* f:F){
    if(f->name.endsWith("finger1")) fing1=f;
    if(f->name.endsWith("finger2")) fing2=f;
  }
//  fing1 = fing1->parent;
//  fing2 = fing2->parent;
  fing1 = fing1->getUpwardLink();
  fing2 = fing2->getUpwardLink();

  CHECK(fing1->joint, "");
  CHECK(fing2->joint, "");
  CHECK(!fing1->joint->active || !fing1->joint->dim, ""); //grippers need to be rigid joints! (to not be part of the dynamic/control system)
  CHECK(!fing2->joint->active || !fing2->joint->dim, "");

  //requirement: two of the children of need to be the finger geometries
//  fing1 = gripper->children(0); while(!fing1->shape && fing1->children.N) fing1 = fing1->children(0);
//  fing2 = gripper->children(1); while(!fing2->shape && fing2->children.N) fing2 = fing2->children(0);
  return true;
}

void Simulation::openGripper(const char* gripperFrameName, double width, double speed) {
  rai::Frame* gripper, *fing1, *fing2;
  getFingersForGripper(gripper, fing1, fing2, C, gripperFrameName);
  if(!gripper) return;

  //remove gripper from grasps list
  for(uint i=grasps.N; i--;) {
    if(grasps.elem(i)==gripper) grasps.remove(i);
  }

  //check if an object is attached
  rai::Frame* obj = gripper->children(-1);
  if(!obj || !obj->joint || obj->joint->type != rai::JT_rigid) {
    if(verbose>1) {
      LOG(1) <<"gripper '" <<gripper->name <<"' does not hold an object";
    }
    obj=0;
  }

  //reattach object to world frame, and make it physical
  if(obj) {
    C.attach(C.frames(0), obj);
    if(engine==_physx) {
      self->physx->changeObjectType(obj, rai::BT_dynamic);
    } else {
      self->bullet->changeObjectType(obj, rai::BT_dynamic);
    }
  }

  if(verbose>1) {
    LOG(1) <<"initiating opening gripper " <<gripper->name;
  }
  imps.append(make_shared<Imp_OpenGripper>(gripper, fing1, fing2, speed));
}

void Simulation::closeGripper(const char* gripperFrameName, double width, double speed, double force) {
  rai::Frame* gripper, *fing1, *fing2;
  getFingersForGripper(gripper, fing1, fing2, C, gripperFrameName);
  if(!gripper) return;

  rai::Frame *finger1 = fing1, *finger2=fing2;
  while(!finger1->shape || finger1->shape->type()!=ST_capsule) finger1=finger1->children.last();
  while(!finger2->shape || finger2->shape->type()!=ST_capsule) finger2=finger2->children.last();

  //collect objects close to fing1 and fing2
  C.stepSwift();
  FrameL fing1close;
  FrameL fing2close;
  for(rai::Proxy& p:C.proxies) {
    if(p.a == finger1) fing1close.setAppend(p.b);
    if(p.b == finger1) fing1close.setAppend(p.a);
    if(p.a == finger2) fing2close.setAppend(p.b);
    if(p.b == finger2) fing2close.setAppend(p.a);
  }

  //intersect
  FrameL objs = setSection(fing1close, fing2close);
//  cout <<"initiating ";
//  listWrite(objs);
//  cout <<endl;

  rai::Frame* obj = 0;
  if(objs.N) obj = objs.elem(0);

  //choose from multiple object candidates
  if(objs.N>1) {
    arr center = .5*(finger1->getPosition()+finger2->getPosition());
    double d = length(center - obj->getPosition());
    for(uint i=1; i<objs.N; i++) {
      double d2 = length(center - objs(i)->getPosition());
      if(d2<d) { obj = objs(i); d = d2; }
    }
  }

  if(verbose>1) {
    if(obj) {
      LOG(1) <<"initiating grasp of object " <<obj->name <<" (if this is not what you expect, did you setContact(1) for the object you want to grasp?)";
    } else {
      LOG(1) <<"closing gripper without near object (if this is not what you expect, did you setContact(1) for the object you want to grasp?)";
    }
  }

  imps.append(make_shared<Imp_CloseGripper>(gripper, fing1, fing2, obj, speed));
}

ptr<SimulationState> Simulation::getState() {
  arr qdot;
  if(engine==_physx) {
    self->physx->pullDynamicStates(C.frames, qdot);
  } else if(engine==_bullet) {
    self->bullet->pullDynamicStates(C.frames, qdot);
  } else NIY;
  return make_shared<SimulationState>(C.getFrameState(), qdot);
}

void Simulation::setState(const arr& frameState, const arr& frameVelocities) {
  C.setFrameState(frameState);
  pushConfigurationToSimulator(frameVelocities);
}

void Simulation::pushConfigurationToSimulator(const arr& frameVelocities) {
  if(engine==_physx) {
    self->physx->pushFullState(C.frames, frameVelocities);
  } else if(engine==_bullet) {
    self->bullet->pushFullState(C.frames, frameVelocities);
  } else NIY;
}

void Simulation::registerNewObjectWithEngine(Frame* f) {
  CHECK_EQ(&f->C, &C, "can't register frame that is not part of the simulated configuration");
  if(engine==_physx) {
    self->physx->postAddObject(f);
  } else if(engine==_bullet) {
    NIY;
  } else NIY;
}

void Simulation::restoreState(const ptr<SimulationState>& state) {
  setState(state->frameState, state->frameVels);
}

const arr& Simulation::get_qDot() {
  return self->qdot;
}

double Simulation::getTimeToMove(){
  return self->ref.getEndTime()-time;
}

double Simulation::getGripperWidth(const char* gripperFrameName) {
  rai::Frame* gripper, *fing1, *fing2;
  getFingersForGripper(gripper, fing1, fing2, C, gripperFrameName);
  if(!gripper) return -1.;
  CHECK(!fing1->joint->active || !fing1->joint->dim, "");
  return fing1->get_Q().pos.x;
}

bool Simulation::getGripperIsGrasping(const char* gripperFrameName) {
  rai::Frame* gripper, *fing1, *fing2;
  getFingersForGripper(gripper, fing1, fing2, C, gripperFrameName);
  for(Frame* g:grasps) if(g==gripper) return true;
  return false;
}

bool Simulation::getGripperIsClose(const char* gripperFrameName) {
  rai::Frame* gripper, *fing1, *fing2;
  getFingersForGripper(gripper, fing1, fing2, C, gripperFrameName);
  if(!gripper) return -1.;
  double q = fing1->get_Q().pos.x;
  if(q<=fing1->joint->limits(0)) return true;
  return false;
}

bool Simulation::getGripperIsOpen(const char* gripperFrameName) {
  rai::Frame *gripper, *fing1, *fing2;
  getFingersForGripper(gripper, fing1, fing2, C, gripperFrameName);
  if(!gripper) return false;
  double q = fing1->get_Q().pos.x;
  if(q>=fing1->joint->limits(1)) return true;
  return false;
}

CameraView& Simulation::cameraview() {
  if(!self->cameraview) {
    self->cameraview = make_shared<CameraView>(C, true, false);
  }
  return *self->cameraview;
}

void Simulation::addImp(Simulation::ImpType type, const StringA& frames, const arr& parameters) {
  if(type==_objectImpulses) {
    CHECK_EQ(frames.N, 1, "");
    rai::Frame* obj = C.getFrame(frames(0));
    imps.append(make_shared<Imp_ObjectImpulses>(obj));
  } else if(type==_blockJoints) {
    FrameL F = C.getFrames(frames);
    auto block = make_shared<Imp_BlockJoints>(F, *this);
    imps.append(block);
  } else {
    NIY;
  }
}

void Simulation::getImageAndDepth(byteA& image, floatA& depth) {
  cameraview().updateConfiguration(C);
  cameraview().renderMode = CameraView::visuals;
  cameraview().computeImageAndDepth(image, depth);

  //-- imps after images
  for(ptr<SimulationImp>& imp : imps) if(imp->when==SimulationImp::_afterImages) {
      imp->modImages(*this, image, depth);
    }

  if(verbose>0) self->updateDisplayData(image, depth);
}

//===========================================================================
//added-------------------------
struct MoveBallHereCallback:OpenGL::GLClickCall {

  MoveBallHereCallback() { }
  bool clickCallback(OpenGL& gl) {
    if(gl.mouse_button==1 && gl.mouseIsDown)
    {
      float d = gl.captureDepth(gl.mouseposy, gl.mouseposx);
      arr x = {double(gl.mouseposy), double(gl.mouseposy), d};
//      cout <<" image coords: " <<x;
      if(d<.01 || d==1.) {
        cout <<"NO SELECTION: SELECTION DEPTH = " <<d <<' ' <<gl.camera.glConvertToTrueDepth(d) <<endl;
      } else {
        std::cout << "pixel coords and depth " << x << std::endl;
        gl.camera.unproject_fromPixelsAndGLDepth(x, gl.width, gl.height);
      }

      std::cout << "translation in world coords is " << x << std::endl;


    }

    return true;
  }
};
//-------------------------
struct Simulation_DisplayThread : Thread, GLDrawer {
  Configuration Ccopy;
  OpenGL gl;
  //data
  Mutex mux;
  double time;
  byteA image;
  floatA depth;
  byteA segmentation;
  byteA screenshot;

  Simulation_DisplayThread(const Configuration& C)
    : Thread("Sim_DisplayThread", .05),
      Ccopy(C),
      gl("Simulation Display") {
    gl.add(*this);
    gl.camera.setDefault();
    gl.addClickCall(new MoveBallHereCallback());///added

    if(Ccopy["camera"]) gl.camera.X = Ccopy["camera"]->ensure_X();

    threadLoop();
    while(step_count<2) rai::wait(.05);
  }

  ~Simulation_DisplayThread() {
    gl.clear();
    threadClose(.5);
  }

  void step() {
    gl.update(STRING("t:" <<time), true);
  }

  void glDraw(OpenGL& gl) {
#ifdef RAI_GL
    mux.lock(RAI_HERE);
    glStandardScene(nullptr, gl);
    Ccopy.glDraw(gl);

    if(image.N && depth.N) {
      static byteA dep;
      resizeAs(dep, depth);
      float x;
      for(uint i=0; i<dep.N; i++) {
        x = 100.f * depth.elem(i); //this means that the RGB values are cm distance (up to 255cm distance)
        dep.elem(i) = (x<0.)?0:((x>255.)?255:x);
      }
      float scale = .3*float(gl.width)/image.d1;
      float top = 1. - scale*float(image.d0)/gl.height;

      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glOrtho(0, 1., 1., 0., -1., 1.); //only affects the offset - the rest is done with raster zooms
      glDisable(GL_DEPTH_TEST);
      glRasterImage(.0, top, image, scale);
      glRasterImage(.7, top, dep, scale);
    }

    screenshot.resize(gl.height, gl.width, 3);
    glReadPixels(0, 0, gl.width, gl.height, GL_RGB, GL_UNSIGNED_BYTE, screenshot.p);

    mux.unlock();
#else
    NICO
#endif
  }
};

void Simulation_self::updateDisplayData(double _time, const rai::Configuration& _C) {
  CHECK(display, "");
  display->mux.lock(RAI_HERE);
  display->time = _time;
  if(_C.frames.N!=display->Ccopy.frames.N) {
    display->Ccopy.copy(_C, false);
    //deep copy meshes!
    for(rai::Frame* f:display->Ccopy.frames) if(f->shape) {
        ptr<Mesh> org = f->shape->_mesh;
        f->shape->_mesh = make_shared<Mesh> (*org.get());
      }
  }
  display->Ccopy.setFrameState(_C.getFrameState());
  display->Ccopy.copyProxies(_C.proxies);
  display->mux.unlock();
}

void Simulation_self::updateDisplayData(const byteA& _image, const floatA& _depth) {
  CHECK(display, "");
  display->mux.lock(RAI_HERE);
  display->image = _image;
  display->depth= _depth;
  display->mux.unlock();
}

byteA Simulation::getScreenshot() {
  if(!self->display) return byteA();
  byteA ret;
  self->display->mux.lock(RAI_HERE);
  ret = self->display->screenshot;
  self->display->mux.unlock();
  return ret;
}

//===========================================================================

Imp_CloseGripper::Imp_CloseGripper(Frame* _gripper, Frame* _fing1, Frame* _fing2, Frame* _obj, double _speed)
  : gripper(_gripper), fing1(_fing1), fing2(_fing2), obj(_obj), finger1(fing1), finger2(fing2), speed(_speed) {
  when = _beforePhysics;
  type = Simulation::_closeGripper;

  while(!finger1->shape || finger1->shape->type()!=ST_capsule) finger1=finger1->children.last();
  while(!finger2->shape || finger2->shape->type()!=ST_capsule) finger2=finger2->children.last();

  if(obj) {
    coll1 = make_unique<F_PairCollision>(F_PairCollision::_negScalar, false);
    coll1->setFrameIDs({finger1->ID, obj->ID});
    coll2 = make_unique<F_PairCollision>(F_PairCollision::_negScalar, false);
    coll2->setFrameIDs({finger2->ID, obj->ID});
  }

  CHECK(!fing1->joint->active || !fing1->joint->dim, "");
  q = fing1->get_Q().pos.x;
}

void Imp_CloseGripper::modConfiguration(Simulation& S, double tau) {
  if(killMe) return;

  CHECK_EQ(&S.C, &fing1->C, "");
  CHECK_EQ(&S.C, &fing2->C, "");
  if(obj) {
    CHECK_EQ(&S.C, &obj->C, "");
  }

  //-- actually close gripper until both distances are < .001
  q -= 1e-1*speed*tau;
  fing1->set_Q()->pos.set(q, 0., 0.);
  fing2->set_Q()->pos.set(q, 0., 0.);

  if(q<fing1->joint->limits(0)) { //stop grasp by joint limits -> unsuccessful
    if(S.verbose>1) {
      LOG(1) <<"terminating closing gripper (limit) - nothing grasped";
    }
    killMe = true;
  } else if(obj) {
    //      step({}, .01, _none);
    auto d1 = coll1->eval(coll1->getFrames(S.C));
    auto d2 = coll2->eval(coll2->getFrames(S.C));
    //  cout <<q <<" d1: " <<d1 <<"d2: " <<d2 <<endl;
    if(-d1(0)<1e-3 && -d2(0)<1e-3) { //stop grasp by contact
      //evaluate stability
      F_GraspOppose oppose;
      arr y = oppose.eval({finger1, finger2, obj});

      if(sumOfSqr(y) < 0.1) { //good enough -> success!
        // kinematically attach object to gripper
        obj = obj->getUpwardLink();
        S.C.attach(gripper, obj);

        // tell engine that object is now kinematic, not dynamic
        if(S.engine==S._physx) {
          S.self->physx->changeObjectType(obj, BT_kinematic);
        } else {
          S.self->bullet->changeObjectType(obj, BT_kinematic);
        }

        //allows the user to know that gripper grasps something
        S.grasps.append(gripper);

        if(S.verbose>1) {
          LOG(1) <<"terminating grasp of object " <<obj->name <<" - SUCCESS";
        }
      } else { //unsuccessful
        if(S.verbose>1) {
          LOG(1) <<"terminating grasp of object " <<obj->name <<" - FAILURE";
        }
      }
      killMe = true;
    }
  }
}

//===========================================================================

Imp_OpenGripper::Imp_OpenGripper(Frame* _gripper, Frame* _fing1, Frame* _fing2, double _speed)
  : gripper(_gripper), fing1(_fing1), fing2(_fing2), speed(_speed) {
  when = _beforePhysics;
  type = Simulation::_openGripper;

  CHECK(!fing1->joint->active || !fing1->joint->dim, "");
  q = fing1->get_Q().pos.x;
}

void Imp_OpenGripper::modConfiguration(Simulation& S, double tau) {
  if(killMe) return;

  CHECK_EQ(&S.C, &gripper->C, "");
  CHECK_EQ(&S.C, &fing1->C, "");
  CHECK_EQ(&S.C, &fing2->C, "");

  //-- actually open gripper until limit
  q += 1e-1*speed*tau;
  fing1->set_Q()->pos.set(q, 0., 0.);
  fing2->set_Q()->pos.set(q, 0., 0.);
  if(q > fing1->joint->limits(1)) { //stop opening
    if(S.verbose>1) {
      LOG(1) <<"terminating opening gripper " <<gripper->name;
    }
    killMe = true;
  }
}

//===========================================================================

void Imp_ObjectImpulses::modConfiguration(Simulation& S, double tau) {
  timeToImpulse -= tau;
  if(timeToImpulse>0.) return;

  timeToImpulse = 1.;

  arr vel = randn(3);
  if(vel(2)<0.) vel(2)=0.;
  vel(0) *= .1;
  vel(1) *= .1;

  std::shared_ptr<SimulationState> state = S.getState();

  state->frameVels(obj->ID, 0, {}) = vel;

  S.restoreState(state);
}

//===========================================================================

Imp_BlockJoints::Imp_BlockJoints(const FrameL& _joints, Simulation& S)
  : joints(_joints) {
  when = _beforePhysics;
  qBlocked.resize(joints.N);
  arr q = S.C.getJointState();
  for(uint i=0; i<joints.N; i++) {
    rai::Joint* j = joints(i)->joint;
    CHECK(j, "");
    qBlocked(i) = q(j->qIndex);
  }
}

void Imp_BlockJoints::modConfiguration(Simulation& S, double tau) {
  CHECK_EQ(joints.N, qBlocked.N, "");
  arr q = S.C.getJointState();
  for(uint i=0; i<joints.N; i++) {
    rai::Joint* j = joints(i)->joint;
    CHECK(j, "");
    q(j->qIndex) = qBlocked(i);
  }
  S.C.setJointState(q);
}

} //namespace rai
