#pragma once

#include <Core/thread.h>
#include <Kin/kin.h>

enum RobotType { ROB_sim=0, ROB_pr2, ROB_baxter, ROB_kukaWSG };

struct RobotAbstraction{
    virtual ~RobotAbstraction(){}
    //-- basic info
    virtual StringA getJointNames() = 0;
    virtual arr getHomePose() = 0;
    //-- execution
    virtual bool executeMotion(const StringA& joints, const arr& path, const arr& times, double timeScale=1., bool append=false) = 0;
    virtual void execGripper(const rai::String& gripperName, double position, double force=40.) = 0;
    virtual void attach(const char *a, const char *b){}
    //-- feedback
    virtual arr getJointPositions(const StringA& joints={}) = 0;
    virtual double timeToGo() = 0;
};

struct RobotIO{
    std::shared_ptr<RobotAbstraction> self;
    rai::Enum<RobotType> type;

    RobotIO(const rai::KinematicWorld& _K, RobotType type);
    ~RobotIO();

    //-- just call virtuals
    bool executeMotion(const StringA& joints, const arr& path, const arr& times, double timeScale=1., bool append=false){
        if(!path.N && !times.N){
            LOG(-1) <<"you send an empty path to execute - perhaps the path computation didn't work";
            return false;
        }
        return self->executeMotion(joints, path, times, timeScale, append);
    }
    void execGripper(const rai::String& gripper, double position, double force=40.){
        return self->execGripper(gripper, position, force); }
    arr getJointPositions(const StringA& joints={}){
        return self->getJointPositions(joints); }
    StringA getJointNames(){
        return self->getJointNames(); }
    arr getHomePose(){
        return self->getHomePose();
    }
    void attach(const char *a, const char *b){
        return self->attach(a,b);
    }

    void waitForCompletion(){
        while(self->timeToGo()>0.){
            cout <<"ttg:" <<self->timeToGo() <<endl;
            rai::wait(.1);
        }
    }

};

