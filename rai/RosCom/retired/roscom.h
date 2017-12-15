//===========================================================================
//
// variable declarations
//


//-- a basic message type for communication with the soft hand controller
struct SoftHandMsg{
  mlr::String soft_hand_cmd;
  SoftHandMsg(){}
  SoftHandMsg(const mlr::String soft_hand_cmd)
    :soft_hand_cmd(soft_hand_cmd){}
};
//inline void operator<<(ostream& os, const CtrlMsg& m){ os<<"BLA"; }
//inline void operator>>(istream& os, CtrlMsg& m){  }


//===========================================================================
//
// modules
//
//===========================================================================
/// This module only calls ros:spinOnce() in step() and loops full speed -- to sync the process with the ros server



// Helper function so sync ors with the real PR2
/**
 * This starts the initial sync of the world with ctrl_obs from the robot.
 *
 * This is verbose (helps debugging) and retries to connect to the robot multiple times.
 *
 * If useRos==false then nothing happens.
 */
void initialSyncJointStateWithROS(mlr::KinematicWorld& world, Access<CtrlMsg>& ctrl_obs, bool useRos);

/**
 * Sync the world with ctrl_obs from the robot.
 *
 * If useRos==false then nothing happens.
 */
void syncJointStateWitROS(mlr::KinematicWorld& world, Access<CtrlMsg>& ctrl_obs, bool useRos);

//===========================================================================

struct PerceptionObjects2Ors : Thread {
  Access<visualization_msgs::MarkerArray> perceptionObjects;
  Access<mlr::KinematicWorld> modelWorld;
  PerceptionObjects2Ors()
    : Thread("PerceptionObjects2Ors"),
    perceptionObjects(this, "perceptionObjects", true),
    modelWorld(this, "modelWorld"){}
  void open(){}
  void step();
  void close(){}
};

