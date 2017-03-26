#include "frame.h"
#include "local_map.h"

namespace proslam {

  Count Frame::_instances = 0;

  Frame::Frame(const WorldMap* context_,
               Frame* previous_,
               Frame* next_,
               const TransformMatrix3D& robot_to_world_,
               const real& maximum_depth_close_): _identifier(_instances),
                                                  _maximum_depth_close(maximum_depth_close_) {
    ++_instances;
    setPrevious(previous_);
    setNext(next_);
    setRobotToWorld(robot_to_world_);
    _points.clear();
  }

  //ds deep copy constructor, used for local map generation
  Frame::Frame(Frame* frame_): _identifier(frame_->identifier()),
                               _maximum_depth_close(frame_->maximumDepthClose()),
                               _local_map(frame_->localMap()) {
    setPrevious(frame_->previous());
    setNext(frame_->next());
    _status = frame_->status();

    //ds take over point references
    _points.clear();
    _points.insert(_points.end(), frame_->points().begin(), frame_->points().end());
    for (FramePoint* frame_point: _points) {
      frame_point->setFrame(this);
    }

    //ds release points from old frame
    frame_->points().clear();

    _robot_to_world          = frame_->robotToWorld();
    _world_to_robot          = frame_->worldToRobot();
    _robot_to_world_ground_truth = frame_->robotToWorldGroundTruth();

    _camera_left           = frame_->cameraLeft();
    _camera_right          = frame_->cameraRight();
    _intensity_image_left  = frame_->intensityImageLeft();
    _intensity_image_right = frame_->intensityImageRight();
  }

  Frame::~Frame() {
    for (const FramePoint* frame_point: _points) {
      delete frame_point;
    }
    _points.clear();
  }

  void Frame::setLocalMap(const LocalMap* local_map_) {
    _local_map          = local_map_;
    _frame_to_local_map = local_map_->worldToRobot()*this->robotToWorld();
    _local_map_to_frame = _frame_to_local_map.inverse();
  }

  const Count Frame::countPoints(const Count min_age_,
                                 const ThreeValued has_landmark_) const {
    Count count = 0;
    for (const FramePoint* frame_point: points()){
      if(frame_point->trackLength() < min_age_) {
        continue;
      }
      if(has_landmark_!= Unknown){
        if(has_landmark_ == True && !frame_point->landmark()) {
          continue;
        }
        if(has_landmark_ == False && frame_point->landmark()) {
          continue;
        }
      }
      ++count;
    }
    return count;
  }

  void Frame::setRobotToWorld(const TransformMatrix3D& robot_to_world_) {
    _robot_to_world = robot_to_world_;
    _world_to_robot = _robot_to_world.inverse();
  }

  //ds request new framepoint with optional link to a previous point (track)
  FramePoint* Frame::create(const cv::KeyPoint& keypoint_left_,
                                    const cv::Mat& descriptor_left_,
                                    const cv::KeyPoint& keypoint_right_,
                                    const cv::Mat& descriptor_right_,
                                    const PointCoordinates& camera_coordinates_left_,
                                    FramePoint* previous_point_) {

    //ds allocate a new point connected to the previous one
    FramePoint* frame_point = new FramePoint(keypoint_left_,
                                             descriptor_left_,
                                             keypoint_right_,
                                             descriptor_right_,
                                             this);
    frame_point->setCameraCoordinatesLeft(camera_coordinates_left_);
    frame_point->setRobotCoordinates(cameraLeft()->cameraToRobot()*camera_coordinates_left_);

    //ds if the point is not linked (no track)
    if (previous_point_ == 0) {

      //ds this point has no predecessor
      frame_point->setOrigin(frame_point);
    } else {

      //ds connect the framepoints
      frame_point->setPrevious(previous_point_);
    }

    //ds update depth based on quality
    frame_point->setDepthMeters(camera_coordinates_left_.z());
    if (frame_point->depthMeters() < _maximum_depth_close) {
      frame_point->setIsNear(true);
    }

    return frame_point;
  }

  Frame* FramePtrMap::get(int index) {
    FramePtrMap::iterator it=find(index);
    if (it==end())
      return 0;
    return it->second;
  }

  void Frame::releaseImages() {
    _intensity_image_left.release();
    _intensity_image_right.release();
  }

  void Frame::releasePoints() {
    for (const FramePoint* frame_point: _points) {
      delete frame_point;
    }
    _points.clear();
  }

  void FramePtrMap::put(Frame* frame) {
    FramePtrMap::iterator it=find(frame->identifier());
    if (it!=end()){
      throw std::runtime_error("FramePtrMap::put(...), double insertion");
    }
    insert(std::make_pair(frame->identifier(), frame));
  }

  void FramePtrMap::replace(Frame* frame) {
    FramePtrMap::iterator it = find(frame->identifier());
    if (it!=end()) {
      Frame* frame_to_be_replaced = it->second;

      //ds update parent/child
      frame_to_be_replaced->previous()->setNext(frame);
      frame->setPrevious(frame_to_be_replaced->previous());
      if (0 != frame_to_be_replaced->next()) {
        frame->setNext(frame_to_be_replaced->next());
      }

      //ds free old frame and set new
      delete frame_to_be_replaced;
      it->second = frame;
    } else {
      throw std::runtime_error("cannot replace inexisting frame");
    }
  }
}
