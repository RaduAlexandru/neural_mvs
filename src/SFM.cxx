#include "neural_mvs/SFM.h"

//c++
#include <string>

//easypbr
#include "easy_pbr/Gui.h"



//loguru
#define LOGURU_REPLACE_GLOG 1
#include <loguru.hpp> //needs to be added after torch.h otherwise loguru stops printing for some reason

//configuru
#define CONFIGURU_WITH_EIGEN 1
#define CONFIGURU_IMPLICIT_CONVERSIONS 1
#include <configuru.hpp>
using namespace configuru;
//Add this header after we add all cuda stuff because we need the profiler to have cudaDeviceSyncronize defined
#define ENABLE_CUDA_PROFILING 1
#include "Profiler.h" 


#include "string_utils.h"
#include "numerical_utils.h"
#include "eigen_utils.h"
#include "easy_pbr/Mesh.h"
#include "easy_pbr/Gui.h"

//opencv 
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/sfm/triangulation.hpp"
#include <opencv2/core/eigen.hpp>


//boost
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;




//CPU code that calls the kernels
SFM::SFM()
    {


}

SFM::~SFM(){
    LOG(WARNING) << "Deleting SFM";
}

std::pair< std::vector<cv::KeyPoint>,   cv::Mat > SFM::compute_keypoints_and_descriptor( const easy_pbr::Frame& frame ){


    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;

    //extract features
    // auto orb = cv::ORB::create(1000);
    //akaze
    auto feat_extractor = cv::AKAZE::create();
    // feat_extractor->setThreshold(0.00001);
    feat_extractor->detectAndCompute( frame.gray_8u, cv::noArray(), keypoints, descriptors);

    //for using flannbased matcher we need to conver the descriptor to float 32 
    descriptors.convertTo(descriptors, CV_32F);

    // keypoints_per_frame.push_back(keypoints);
    // descriptors_per_frame.push_back(descriptors);


    //draw extracted features
    // cv::Mat keypoints_img;
    // cv::drawKeypoints(frame.gray_8u, keypoints, keypoints_img);
    // easy_pbr::Gui::show(keypoints_img, "keypoints_img"+std::to_string(i) );
    // easy_pbr::Gui::show(frame.rgb_32f, "img"+std::to_string(i) );

    auto ret = std::make_pair( keypoints, descriptors);
    return ret;

}


std::vector<bool> SFM::filter_matches_lowe_ratio ( std::vector< std::vector<cv::DMatch> >& knn_matches ){

    // https://docs.opencv.org/3.4/d5/d6f/tutorial_feature_flann_matcher.html
    const float ratio_thresh = 0.7f;
    std::vector<bool> is_match_good(knn_matches.size(), false);
    for (size_t i = 0; i < knn_matches.size(); i++){
        if (knn_matches[i][0].distance < ratio_thresh * knn_matches[i][1].distance){
            // good_matches.push_back(knn_matches[i][0]);
            is_match_good[i] = true;
        }
    }

    return is_match_good;

}

std::vector<bool> SFM::filter_min_max_movement (  const std::vector< std::vector<cv::DMatch> >& knn_query_matches, const std::vector< std::vector<cv::DMatch> >& knn_target_matches, std::vector<cv::KeyPoint>& query_keypoints, 
std::vector<cv::KeyPoint>& target_keypoints, const int img_size, std::vector<bool>& is_query_match_good_prev  ){

    // https://docs.opencv.org/3.4/d5/d6f/tutorial_feature_flann_matcher.html
    const float max_movement_ratio = 0.1f;
    // const float min_movement_ratio = 0.01f;
    const float min_movement_ratio = 0.0f;
    std::vector<bool> is_match_good(knn_query_matches.size(), false);
    for (size_t i = 0; i < knn_query_matches.size(); i++){
        if(is_query_match_good_prev[i]){
            int query_idx=knn_query_matches[i][0].queryIdx;
            int target_idx= knn_target_matches[ knn_query_matches[i][0].trainIdx ][0].queryIdx;

            cv::Point2f point_query = query_keypoints[query_idx].pt;
            cv::Point2f point_target = target_keypoints[target_idx].pt;

            Eigen::Vector2f pt1, pt2;
            pt1 <<point_query.x, point_query.y;
            pt2 <<point_target.x, point_target.y;

            float dist= (pt1-pt2).norm();
            float dist_ratio= dist/img_size;

            if(dist_ratio>max_movement_ratio || dist_ratio<min_movement_ratio){
                //it's too far away or the points are too close together for a good triangulation in the other image os it;s a bad match
            }else{
                is_match_good[i]=true;
            }
        }

    }

    return is_match_good;

}

std::vector<bool> SFM::filter_cross_check( const std::vector< std::vector<cv::DMatch> >& knn_query_matches, const std::vector< std::vector<cv::DMatch> >& knn_target_matches, std::vector<bool>& is_query_match_good_prev ){

    // std::vector<cv::DMatch> good_query_matches;
    std::vector<bool> is_query_match_good(knn_query_matches.size(), false);

    // int nr_cross_check_passed=0;

    for (size_t i = 0; i < knn_query_matches.size(); i++){
        cv::DMatch query_match=knn_query_matches[i][0];
        int query_idx= query_match.queryIdx;
        int train_idx= query_match.trainIdx;

        //get the match that the target image says it has at the target points
        cv::DMatch target_match=knn_target_matches[train_idx][0];
        //if the cross check if correct
        // VLOG(1) << " query_idx is "<< query_idx << " train idx is " <<target_match.trainIdx;
        if (query_idx==target_match.trainIdx ){ //the target of the target should be this point
            // good_query_matches.push_back(query_match);
            if (is_query_match_good_prev[i]){
                is_query_match_good[i]=true;
                // nr_cross_check_passed++;
            }
        }
    }

    // VLOG(1) << "Cross check left " << nr_cross_check_passed << " out of a possible of " << knn_query_matches.size();


    return is_query_match_good;


}


easy_pbr::MeshSharedPtr SFM::compute_3D_keypoints_from_frames(const easy_pbr::Frame& frame_query, const easy_pbr::Frame& frame_target){

    easy_pbr::MeshSharedPtr mesh=easy_pbr::Mesh::create();




    //attempt 2 a bit nicer

    //get keypoints and descritpro
    auto keypoints_and_descriptors_query = compute_keypoints_and_descriptor(frame_query);
    auto keypoints_and_descriptors_target = compute_keypoints_and_descriptor(frame_target);
    std::vector<cv::KeyPoint> query_keypoints=keypoints_and_descriptors_query.first; 
    cv::Mat query_descriptors=keypoints_and_descriptors_query.second; 
    std::vector<cv::KeyPoint> target_keypoints=keypoints_and_descriptors_target.first; 
    cv::Mat target_descriptors=keypoints_and_descriptors_target.second; 


    //match 
    auto matcher = cv::DescriptorMatcher::create(cv::DescriptorMatcher::FLANNBASED);
    std::vector< std::vector<cv::DMatch> > knn_matches_query_towards_target;
    std::vector< std::vector<cv::DMatch> > knn_matches_target_towards_query;
    matcher->knnMatch( query_descriptors, target_descriptors, knn_matches_query_towards_target, 2 );
    matcher->knnMatch( target_descriptors, query_descriptors, knn_matches_target_towards_query, 2 );

    
    std::vector<bool> is_query_match_good = filter_matches_lowe_ratio ( knn_matches_query_towards_target );
    std::vector<bool> is_target_match_good = filter_matches_lowe_ratio ( knn_matches_target_towards_query );

    int img_size=std::max(frame_query.width, frame_query.height); 
    is_query_match_good=filter_min_max_movement(knn_matches_query_towards_target, knn_matches_target_towards_query, query_keypoints, target_keypoints,  img_size, is_query_match_good);
    is_target_match_good=filter_min_max_movement(knn_matches_target_towards_query, knn_matches_query_towards_target, query_keypoints, target_keypoints, img_size, is_target_match_good);

    is_query_match_good = filter_cross_check( knn_matches_query_towards_target, knn_matches_target_towards_query, is_query_match_good );


    //actually remove the matches that are not good
    std::vector<cv::DMatch> matches;
    for (size_t i = 0; i < knn_matches_query_towards_target.size(); i++){
        if (is_query_match_good[i]){
            matches.push_back(knn_matches_query_towards_target[i][0]);
        }
    }





     //draw matches 
    cv::Mat matches_img;
    cv::drawMatches (frame_query.gray_8u, query_keypoints, frame_target.gray_8u, target_keypoints, matches, matches_img);
    easy_pbr::Gui::show(matches_img, "matches" );




    //triangulate https://stackoverflow.com/a/16299909
    // https://answers.opencv.org/question/171898/sfm-triangulatepoints-input-array-assertion-failed/
    VLOG(1) << "traingulate";


    //attempt 2
    cv::Mat cam_0_points(2, matches.size(), CV_64FC1);
    cv::Mat cam_1_points(2, matches.size(), CV_64FC1);
    for(size_t m_idx=0; m_idx<matches.size(); m_idx++){
        cv::DMatch match=matches[m_idx];
        int query_idx=match.queryIdx;
        int target_idx=match.trainIdx;

        cam_0_points.at<double>(0, m_idx)= query_keypoints[query_idx].pt.x;
        cam_0_points.at<double>(1, m_idx)= query_keypoints[query_idx].pt.y;

        cam_1_points.at<double>(0, m_idx)= target_keypoints[target_idx].pt.x;
        cam_1_points.at<double>(1, m_idx)= target_keypoints[target_idx].pt.y;

    }
    


    // VLOG(1) << points1Mat.rows << " x " << points1Mat.cols;
    // VLOG(1) << "cam_0_points " << cam_0_points;
    //matrices of each cam
    cv::Mat cam_0_matrix;
    cv::Mat cam_1_matrix;
    // Eigen::MatrixXd cam_0_matrix_eigen= ( frame_query.K * frame_query.tf_cam_world.matrix().block<3,4>(0,0) ).cast<double>();
    // Eigen::MatrixXd cam_1_matrix_eigen= ( frame_target.K * frame_target.tf_cam_world.matrix().block<3,4>(0,0) ).cast<double>();

    //with cma query being at identity
    Eigen::Affine3f tf_cam_query_target=  frame_query.tf_cam_world* frame_target.tf_cam_world.inverse(); //target to world and world to query
    Eigen::Affine3f id=tf_cam_query_target;
    id.setIdentity();
    Eigen::MatrixXd cam_0_matrix_eigen= ( frame_query.K * id.matrix().block<3,4>(0,0) ).cast<double>();
    Eigen::MatrixXd cam_1_matrix_eigen= ( frame_target.K * tf_cam_query_target.matrix().block<3,4>(0,0) ).cast<double>();
    cv::eigen2cv(cam_0_matrix_eigen, cam_0_matrix);
    cv::eigen2cv(cam_1_matrix_eigen, cam_1_matrix);
    // VLOG(1) << "cam eigen is " << cam_0_matrix;
    // VLOG(1) << "cam mat is " << cam_0_matrix;
    std::vector<cv::Mat> cam_matrices;
    cam_matrices.push_back(cam_0_matrix);
    cam_matrices.push_back(cam_1_matrix);

    // std::vector<std::vector<cv::Point2f>> points2d_for_each_frame;
    std::vector<cv::Mat> points2d_for_each_frame;
    points2d_for_each_frame.push_back(cam_0_points);
    points2d_for_each_frame.push_back(cam_1_points);
    //DEBUG 
    // points2d_for_each_frame.push_back(points1Mat);
    // points2d_for_each_frame.push_back(points1Mat);


    // cv::sfm::triangulatePoints(dummy, dummy, dummy);
    cv::Mat dummy;
    // std::vector<cv::Point3d> points_3d;
    cv::Mat points_3d;
    cv::sfm::triangulatePoints(points2d_for_each_frame, cam_matrices, points_3d);

    //put the points to V 
    int nr_points=points_3d.cols;
    mesh->V.resize( nr_points, 3 );
    for(int p_idx=0; p_idx<nr_points; p_idx++){
        mesh->V(p_idx, 0) = points_3d.at<double>(0, p_idx);
        mesh->V(p_idx, 1) = -points_3d.at<double>(1, p_idx);
        mesh->V(p_idx, 2) = points_3d.at<double>(2, p_idx);
    }





    mesh->m_vis.m_show_points=true;
    mesh->set_model_matrix( frame_query.tf_cam_world.inverse().cast<double>() );
    mesh->apply_model_matrix_to_cpu(false);


    //DEBUG by projecting the 3d points into the frame
    //project in query img 
    debug_by_projecting(frame_query, mesh, "debug_query");
    debug_by_projecting(frame_target, mesh, "debug_target");
    // cv::Mat img =frame_query.rgb_32f;
    // Eigen::Affine3d trans=frame_query.tf_cam_world.cast<double>();
    // Eigen::Matrix3d K=frame_query.K.cast<double>();
    // for (int i = 0; i < mesh->V.rows(); i++){
    //     Eigen::Vector3d point_world= Eigen::Vector3d(mesh->V.row(i));
    //     Eigen::Vector3d point_cam = trans.linear()*point_world + trans.translation();
    //     point_cam.y()=-point_cam.y();
    //     Eigen::Vector3d point_img=K*point_cam;
    //     int x= point_img.x()/ point_img.z();
    //     int y= point_img.y()/ point_img.z();

    //     cv::Point pt = cv::Point(x, y);
    //     cv::circle(img, pt, 1, cv::Scalar(0, 255, 0));
    // }
    // easy_pbr::Gui::show(img, "img_debug");


    return mesh;
}



void SFM::debug_by_projecting(const easy_pbr::Frame& frame, std::shared_ptr<easy_pbr::Mesh> mesh, const std::string name){

    cv::Mat img =frame.rgb_32f;
    Eigen::Affine3d trans=frame.tf_cam_world.cast<double>();
    Eigen::Matrix3d K=frame.K.cast<double>();
    for (int i = 0; i < mesh->V.rows(); i++){
        Eigen::Vector3d point_world= Eigen::Vector3d(mesh->V.row(i));
        Eigen::Vector3d point_cam = trans.linear()*point_world + trans.translation();
        point_cam.y()=-point_cam.y();
        Eigen::Vector3d point_img=K*point_cam;
        int x= point_img.x()/ point_img.z();
        int y= point_img.y()/ point_img.z();

        cv::Point pt = cv::Point(x, y);
        cv::circle(img, pt, 1, cv::Scalar(0, 255, 0));
    }
    easy_pbr::Gui::show(img, name);

}



//attempt 2 
// compute_3D_keypoints_from_frames
// {

    //for each frame compute keypoints and descriptors

    //for each frame, get the potential mathing ones, which are the ones that have a frustum that points in a somewhat similar direction

    //for each frame, compute the matches towards the potential frames

    //for each frame 
    //  for each keypoint in the frame 
    //      if keypoint_has_no_3d associated{
    //        instantiate a 3D point at origin
    //        record that this keypoint for this frame has a 3D associated
    //        associate a reprojection error function for this 3D point
    //      }
    //      for each match 
    //          frame_target=frame_in which the match was found
    //          record that this keypoint for this frame has a 3D associated
    //          associate a reprojection error function for this 3D point
    //
    //bundle adjust with known poses          


// }



//attempt 3 with colmap 
//compute_3D_keypoints_from_frames 
// {
    
    //run colmap

// }

