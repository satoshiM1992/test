#include "stereo_framepoint_generator.h"

namespace proslam {

StereoFramePointGenerator::StereoFramePointGenerator(StereoFramePointGeneratorParameters* parameters_): BaseFramePointGenerator(parameters_),
                                                                                                        _parameters(parameters_) {
  LOG_DEBUG(std::cerr << "StereoFramePointGenerator::StereoFramePointGenerator|construced" << std::endl)
}

//ds the stereo camera setup must be provided
void StereoFramePointGenerator::configure(){
  LOG_DEBUG(std::cerr << "StereoFramePointGenerator::configure|configuring" << std::endl)
  assert(_camera_right);

  //ds integrate configuration
  _parameters->number_of_cameras = 2;
  BaseFramePointGenerator::configure();
  _triangulation_success_ratios.clear();

  //ds configure current
  _baseline_pixelsmeters     = _camera_right->projectionMatrix()(0,3);
  _baseline_meters           = -_baseline_pixelsmeters/_focal_length_pixels;
  _maximum_depth_far_meters  = -_baseline_pixelsmeters/_parameters->minimum_disparity_pixels;
  _maximum_depth_near_meters = _maximum_depth_far_meters/10;
  _keypoints_with_descriptors_left.clear();
  _keypoints_with_descriptors_right.clear();

  //ds info
  LOG_INFO(std::cerr << "StereoFramePointGenerator::configure|baseline (m): " << _baseline_meters << std::endl)
  LOG_INFO(std::cerr << "StereoFramePointGenerator::configure|maximum depth tracking close (m): " << _maximum_depth_near_meters << std::endl)
  LOG_INFO(std::cerr << "StereoFramePointGenerator::configure|maximum depth tracking far (m): " << _maximum_depth_far_meters << std::endl)
  LOG_DEBUG(std::cerr << "StereoFramePointGenerator::configure|configured" << std::endl)
}

//ds cleanup of dynamic structures
StereoFramePointGenerator::~StereoFramePointGenerator() {
  LOG_DEBUG(std::cerr << "StereoFramePointGenerator::~StereoFramePointGenerator|destroying" << std::endl)
  _triangulation_success_ratios.clear();
  LOG_DEBUG(std::cerr << "StereoFramePointGenerator::~StereoFramePointGenerator|destroyed" << std::endl)
}

//ds computes framepoints stored in a image-like matrix (_framepoints_in_image) for provided stereo images
void StereoFramePointGenerator::compute(Frame* frame_) {

  //ds detect new features to generate frame points from (fixed thresholds)
  detectKeypoints(frame_->intensityImageLeft(), frame_->keypointsLeft());
  detectKeypoints(frame_->intensityImageRight(), frame_->keypointsRight());

  //ds adjust detector thresholds for next frame
  adjustDetectorThresholds();

  //ds overwrite with average
  _number_of_detected_keypoints = (frame_->keypointsLeft().size()+frame_->keypointsRight().size())/2.0;
  frame_->_number_of_detected_keypoints = _number_of_detected_keypoints;

  //ds extract descriptors for detected features
  extractDescriptors(frame_->intensityImageLeft(), frame_->keypointsLeft(), frame_->descriptorsLeft());
  extractDescriptors(frame_->intensityImageRight(), frame_->keypointsRight(), frame_->descriptorsRight());

  //ds prepare and execute stereo keypoint search
  CHRONOMETER_START(point_triangulation)
  initialize(frame_);
  findStereoKeypoints(frame_);
  CHRONOMETER_STOP(point_triangulation)
}

//ds initializes structures for the epipolar stereo keypoint search (called within compute)
void StereoFramePointGenerator::initialize(Frame* frame_) {

  //ds prepare keypoint with descriptors vectors for stereo keypoint search
  _keypoints_with_descriptors_left.resize(frame_->keypointsLeft().size());
  _keypoints_with_descriptors_right.resize(frame_->keypointsRight().size());

  //ds if we got more keypoints in the right image
  if (frame_->keypointsLeft().size() <= frame_->keypointsRight().size()) {

    //ds first add all left keypoints plus equally many from the right
    for (Index u = 0; u < frame_->keypointsLeft().size(); ++u) {
      _keypoints_with_descriptors_left[u].keypoint    = frame_->keypointsLeft()[u];
      _keypoints_with_descriptors_left[u].descriptor  = frame_->descriptorsLeft().row(u);
      _keypoints_with_descriptors_right[u].keypoint   = frame_->keypointsRight()[u];
      _keypoints_with_descriptors_right[u].descriptor = frame_->descriptorsRight().row(u);
    }

    //ds add the remaining points from the right image
    for (Index u = frame_->keypointsLeft().size(); u < frame_->keypointsRight().size(); ++u) {
      _keypoints_with_descriptors_right[u].keypoint   = frame_->keypointsRight()[u];
      _keypoints_with_descriptors_right[u].descriptor = frame_->descriptorsRight().row(u);
    }

  //ds if we got more keypoints in the left image
  } else {

    //ds first add all right keypoints plus equally many from the left
    for (Index u = 0; u < frame_->keypointsRight().size(); ++u) {
      _keypoints_with_descriptors_left[u].keypoint    = frame_->keypointsLeft()[u];
      _keypoints_with_descriptors_left[u].descriptor  = frame_->descriptorsLeft().row(u);
      _keypoints_with_descriptors_right[u].keypoint   = frame_->keypointsRight()[u];
      _keypoints_with_descriptors_right[u].descriptor = frame_->descriptorsRight().row(u);
    }

    //ds add the remaining points from the left image
    for (Index u = frame_->keypointsRight().size(); u < frame_->keypointsLeft().size(); ++u) {
      _keypoints_with_descriptors_left[u].keypoint   = frame_->keypointsLeft()[u];
      _keypoints_with_descriptors_left[u].descriptor = frame_->descriptorsLeft().row(u);
    }
  }
}

//ds computes all potential stereo keypoints (exhaustive in matching distance) and stores them as framepoints (called within compute)
void StereoFramePointGenerator::findStereoKeypoints(Frame* frame_) {

  //ds sort all input vectors by ascending row positions
  std::sort(_keypoints_with_descriptors_left.begin(), _keypoints_with_descriptors_left.end(),
            [](const IntensityFeature& a_, const IntensityFeature& b_){return ((a_.keypoint.pt.y < b_.keypoint.pt.y) ||
                                                                                           (a_.keypoint.pt.y == b_.keypoint.pt.y && a_.keypoint.pt.x < b_.keypoint.pt.x));});
  std::sort(_keypoints_with_descriptors_right.begin(), _keypoints_with_descriptors_right.end(),
            [](const IntensityFeature& a_, const IntensityFeature& b_){return ((a_.keypoint.pt.y < b_.keypoint.pt.y) ||
                                                                                           (a_.keypoint.pt.y == b_.keypoint.pt.y && a_.keypoint.pt.x < b_.keypoint.pt.x));});

  //ds number of stereo matches
  _number_of_available_points = 0;

  //ds adjust triangluation distance depending on frame status
  //ds when localizing we must be very careful since the motion model is not initialized yet) - narrow distance TODO adjust for floating point descriptors
  real maximum_matching_distance_triangulation = _parameters->maximum_matching_distance_triangulation;
  if (frame_->status() == Frame::Localizing) {
    maximum_matching_distance_triangulation = std::min(maximum_matching_distance_triangulation, 0.1*SRRG_PROSLAM_DESCRIPTOR_SIZE_BITS);
  }

  //ds running variable
  uint32_t index_R = 0;

  //ds loop over all left keypoints
  for (uint32_t index_L = 0; index_L < _keypoints_with_descriptors_left.size(); index_L++) {

        //ds if there are no more points on the right to match against - stop
        if (index_R == _keypoints_with_descriptors_right.size()) {break;}

        //ds the right keypoints are on an lower row - skip left
        while (_keypoints_with_descriptors_left[index_L].keypoint.pt.y < _keypoints_with_descriptors_right[index_R].keypoint.pt.y) {
          index_L++; if (index_L == _keypoints_with_descriptors_left.size()) {break;}
        }
        if (index_L == _keypoints_with_descriptors_left.size()) {break;}

        //ds the right keypoints are on an upper row - skip right
        while (_keypoints_with_descriptors_left[index_L].keypoint.pt.y > _keypoints_with_descriptors_right[index_R].keypoint.pt.y) {
          index_R++; if (index_R == _keypoints_with_descriptors_right.size()) {break;}
        }
        if (index_R == _keypoints_with_descriptors_right.size()) {break;}

        //ds search bookkeeping
        uint32_t index_search_R = index_R;
        real distance_best      = maximum_matching_distance_triangulation;
        uint32_t index_best_R   = 0;

        //ds scan epipolar line for current keypoint at idx_L - exhaustive
        while (_keypoints_with_descriptors_left[index_L].keypoint.pt.y == _keypoints_with_descriptors_right[index_search_R].keypoint.pt.y) {

          //ds invalid disparity stop condition
          if (_keypoints_with_descriptors_left[index_L].keypoint.pt.x-_keypoints_with_descriptors_right[index_search_R].keypoint.pt.x < _parameters->minimum_disparity_pixels) {break;}

          //ds compute descriptor distance for the stereo match candidates
          const real distance_hamming = cv::norm(_keypoints_with_descriptors_left[index_L].descriptor, _keypoints_with_descriptors_right[index_search_R].descriptor, SRRG_PROSLAM_DESCRIPTOR_NORM);
          if(distance_hamming < distance_best) {
            distance_best = distance_hamming;
            index_best_R  = index_search_R;
          }
          index_search_R++; if (index_search_R == _keypoints_with_descriptors_right.size()) {break;}
        }

        //ds check if something was found
        if (distance_best < maximum_matching_distance_triangulation) {

            //ds attempt the triangulation
            const cv::KeyPoint& keypoint_left  = _keypoints_with_descriptors_left[index_L].keypoint;
            const cv::KeyPoint& keypoint_right = _keypoints_with_descriptors_right[index_best_R].keypoint;
            const PointCoordinates point_in_camera_left(getCoordinatesInCameraLeft(keypoint_left.pt, keypoint_right.pt));

            //ds add to framepoint map
            const uint32_t& r = keypoint_left.pt.y;
            const uint32_t& c = keypoint_left.pt.x;
            _framepoints_in_image[r][c] = frame_->createFramepoint(keypoint_left,
                                                                   _keypoints_with_descriptors_left[index_L].descriptor,
                                                                   keypoint_right,
                                                                   _keypoints_with_descriptors_right[index_best_R].descriptor,
                                                                   point_in_camera_left);

            //ds reduce search space (this eliminates all structurally conflicting matches)
            index_R = index_best_R+1;
        }
  }
  _number_of_available_points = frame_->createdPoints().size();

  //ds check currently achieved stereo points to triangulation ratio
  const real triangulation_succcess_ratio = static_cast<real>(_number_of_available_points)/_keypoints_with_descriptors_left.size();
  if (triangulation_succcess_ratio < 0.25) {

    //ds raise threshold (tolerance)
    LOG_WARNING(std::cerr << "StereoFramePointGenerator::findStereoKeypoints|low triangulation success ratio: " << triangulation_succcess_ratio
              << " (" << _number_of_available_points << "/" << _keypoints_with_descriptors_left.size() << ")" << std::endl)
  }

  //ds update the average
  _triangulation_success_ratios.push_back(triangulation_succcess_ratio);
  _mean_triangulation_success_ratio = (_number_of_triangulations*_mean_triangulation_success_ratio+triangulation_succcess_ratio)/(_number_of_triangulations+1);
  ++_number_of_triangulations;
}

//ds computes 3D position of a stereo keypoint pair in the keft camera frame (called within findStereoKeypoints)
const PointCoordinates StereoFramePointGenerator::getCoordinatesInCameraLeft(const cv::Point2f& image_coordinates_left_, const cv::Point2f& image_coordinates_right_) const {

  //ds check for minimal disparity
  if (image_coordinates_left_.x-image_coordinates_right_.x < _parameters->minimum_disparity_pixels) {
    throw ExceptionTriangulation("disparity value to low");
  }

  //ds input validation
  assert(image_coordinates_right_.x < image_coordinates_left_.x);
  assert(image_coordinates_right_.y == image_coordinates_left_.y);

  //ds first compute depth (z in camera)
  const real depth_meters = _baseline_pixelsmeters/(image_coordinates_right_.x-image_coordinates_left_.x);
  assert(depth_meters >= 0);
  const real depth_meters_per_pixel = depth_meters/_focal_length_pixels;

  //ds set 3d point
  const PointCoordinates coordinates_in_camera(depth_meters_per_pixel*(image_coordinates_left_.x-_principal_point_offset_u_pixels),
                                               depth_meters_per_pixel*(image_coordinates_left_.y-_principal_point_offset_v_pixels),
                                               depth_meters);

  //ds return triangulated point
  return coordinates_in_camera;
}

const real StereoFramePointGenerator::standardDeviationTriangulationSuccessRatio() const {
  real standard_deviation_triangulation_success_ratio = 0;
  for (const double& triangulation_success_ratio: _triangulation_success_ratios) {
    standard_deviation_triangulation_success_ratio += (_mean_triangulation_success_ratio-triangulation_success_ratio)
                                                     *(_mean_triangulation_success_ratio-triangulation_success_ratio);
  }
  standard_deviation_triangulation_success_ratio /= _triangulation_success_ratios.size();
  standard_deviation_triangulation_success_ratio = std::sqrt(standard_deviation_triangulation_success_ratio);
  return standard_deviation_triangulation_success_ratio;
}
}
