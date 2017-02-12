// Copyright (c) <2016>, <Nanyang Technological University> All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.

// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.

// 3. Neither the name of the copyright holder nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "localization.h"

Localization::Localization(ros::NodeHandle n, std::vector<int> nodesId, std::vector<double> nodesPos)
{
    pose_pub = n.advertise<geometry_msgs::PoseStamped>("optimized/pose", 1);

    path_pub = n.advertise<nav_msgs::Path>("optimized/path", 1);

    solver = new Solver();

    solver->setBlockOrdering(false);

    se3blockSolver = new SE3BlockSolver(solver);

    optimizationsolver = new g2o::OptimizationAlgorithmLevenberg(se3blockSolver);

    optimizer.setAlgorithm(optimizationsolver);

    optimizer.setVerbose(false);

    start_time = 0;

    uwb_number = 0;

    judge = 0;

    last_last_vertex->setEstimate(g2o::SE3Quat(Eigen::Quaterniond(1,0,0,0), Eigen::Vector3d(0,0,0)));

    iteration_max = 30; // 3

    robot_max_velocity = 1; // 2

    g2o::ParameterSE3Offset* cameraOffset = new g2o::ParameterSE3Offset;
    cameraOffset->setId(0);
    optimizer.addParameter(cameraOffset);

    Quaterniond last_rotation = Quaterniond(1,0,0,0);

    self_id = nodesId.back();

    robots.emplace(self_id, Robot(self_id%100+3, false, optimizer));
    ROS_INFO("Init self robot ID: %d with moving option", self_id);

    for (size_t i = 0; i < nodesId.size()-1; ++i)
    {
        robots.emplace(nodesId[i], Robot(nodesId[i]%100, true));
        Eigen::Isometry3d pose = Eigen::Isometry3d::Identity();
        pose(0,3) = nodesPos[i*3];
        pose(1,3) = nodesPos[i*3+1];
        pose(2,3) = nodesPos[i*3+2];
        robots.at(nodesId[i]).init(optimizer, pose);
        ROS_INFO("Init robot ID: %d with position (%.2f,%.2f,%.2f)", nodesId[i], pose(0,3), pose(1,3), pose(2,3));
    }
}


void Localization::solve()
{
    timer.tic();

    optimizer.initializeOptimization();

    optimizer.optimize(iteration_max);

    // optimizer.save("/home/eee/after.g2o");

    // auto edges = optimizer.activeEdges();

    // if(edges.size()>100)
    // {
    //     for(auto edge:edges)
    //         if (edge->chi2() > 2.0 && edge->dimension () ==1)
    //         {
    //             edge->setLevel(1);
    //             ROS_WARN("Removed one Range Edge");
    //         }
    // }
    // optimizer.optimize(iteration_max);

    // ROS_INFO("Graph optimized with error: %f!", optimizer.chi2());

    timer.toc();
}


void Localization::publish()
{
    auto pose = robots.at(self_id).current_pose();

    pose_pub.publish(pose);

    // if(flag_save_file)
    //     save_file(pose);

    path_pub.publish(*robots.at(self_id).vertices2path());
}


void Localization::addPoseEdge(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& pose_cov_)
{
    geometry_msgs::PoseWithCovarianceStamped pose_cov(*pose_cov_);

    if (pose_cov.header.frame_id != robots.at(self_id).last_header(sensor_type.pose).frame_id)
        key_vertex = robots.at(self_id).last_vertex(sensor_type.pose);

    auto new_vertex = robots.at(self_id).new_vertex(sensor_type.pose, pose_cov.header, optimizer);

    g2o::EdgeSE3 *edge = new g2o::EdgeSE3();

    edge->vertices()[0] = key_vertex;

    edge->vertices()[1] = new_vertex;

    Eigen::Isometry3d measurement;

    tf::poseMsgToEigen(pose_cov.pose.pose, measurement);

    edge->setMeasurement(measurement);

    Eigen::Map<Eigen::MatrixXd> covariance(pose_cov.pose.covariance.data(), 6, 6);

    edge->setInformation(covariance.inverse());

    edge->setRobustKernel(new g2o::RobustKernelHuber());

    optimizer.addEdge(edge);

    ROS_INFO("Localization: added pose edge id: %d frame_id: %s;", pose_cov.header.seq, pose_cov.header.frame_id.c_str());

    solve();

    publish();
}


void Localization::addRangeEdge(const uwb_driver::UwbRange::ConstPtr& uwb)
{
    double dt_requester = uwb->header.stamp.toSec() - robots.at(uwb->requester_id).last_header().stamp.toSec();
    double dt_responder = uwb->header.stamp.toSec() - robots.at(uwb->responder_id).last_header().stamp.toSec();

    double cov_requester = pow(robot_max_velocity*dt_requester/3, 2); //3 sigma priciple

    auto vertex_last_requester = robots.at(uwb->requester_id).last_vertex();
    auto vertex_last_responder = robots.at(uwb->responder_id).last_vertex();
    auto vertex_responder = robots.at(uwb->responder_id).new_vertex(sensor_type.range, uwb->header, optimizer);

    auto frame_id = robots.at(uwb->requester_id).last_header().frame_id;

    if(frame_id == uwb->header.frame_id || frame_id == "none")
    {    
        auto vertex_requester = robots.at(uwb->requester_id).new_vertex(sensor_type.range, uwb->header, optimizer);

        auto edge = create_range_edge(vertex_requester, vertex_responder, uwb->distance, pow(uwb->distance_err, 2));

        auto edge_requester_range = create_range_edge(vertex_last_requester, vertex_requester, 0, cov_requester);

        optimizer.addEdge(edge_requester_range);

        optimizer.addEdge(edge);

        ROS_INFO("added two requester range edge with id: <%d>;",uwb->responder_id);
    }
    else
    {
        auto edge = create_range_edge(vertex_last_requester, vertex_responder, uwb->distance, pow(uwb->distance_err, 2) + cov_requester);

        optimizer.addEdge(edge);

        ROS_INFO("added requester edge with id: <%d>", uwb->responder_id);
    }

    if (!robots.at(uwb->responder_id).is_static())
    {
        double cov_responder = pow(robot_max_velocity*dt_responder/3, 2); //3 sigma priciple

        auto edge_responder_range = create_range_edge(vertex_last_responder, vertex_responder, 0, cov_responder);

        optimizer.addEdge(edge_responder_range);

        ROS_INFO("added responder trajectory edge;");
    }

    solve();

    publish();

}


void Localization::addTwistEdge(const geometry_msgs::TwistWithCovarianceStamped::ConstPtr& twist_)
{
    geometry_msgs::TwistWithCovarianceStamped twist(*twist_);

    double dt = twist.header.stamp.toSec() - robots.at(self_id).last_header().stamp.toSec();

    auto last_vertex = robots.at(self_id).last_vertex();

    auto new_vertex = robots.at(self_id).new_vertex(sensor_type.twist, twist.header, optimizer);

    auto edge = create_se3_edge_from_twist(last_vertex, new_vertex, twist.twist, dt);

    optimizer.addEdge(edge);

    ROS_INFO("Localization: added twist edge id: %d!", twist.header.seq);

    solve();
}


void Localization::addImuEdge(const uwb_driver::UwbRange::ConstPtr& uwb,const sensor_msgs::Imu::ConstPtr& Imu_)
{

    double dt_requester = uwb->header.stamp.toSec() - robots.at(uwb->requester_id).last_header(sensor_type.range).stamp.toSec();

    auto vertex_last_requester = robots.at(uwb->requester_id).last_vertex(sensor_type.range);

    uwb_number = uwb_number + 1;

    sensor_msgs::Imu Imu(*Imu_);
    double qx = Imu.orientation.x;
    double qy = Imu.orientation.y;
    double qz = Imu.orientation.z;
    double qw = Imu.orientation.w;

    // requester to responder edge
    auto vertex_requester = robots.at(uwb->requester_id).new_vertex(sensor_type.range, uwb->header, optimizer);

    Eigen::Isometry3d current_pose;
    current_pose.setIdentity(); // very important

    Quaterniond imu_rotation = Quaterniond(Imu.orientation.w,Imu.orientation.x,Imu.orientation.y,Imu.orientation.z);
    // Quaterniond imu_rotation = Quaterniond(1,0,0,0);
    current_pose.rotate(imu_rotation);
    current_pose.translate(g2o::Vector3D(0, 0, 0));

    vertex_requester->setEstimate(current_pose);

    auto vertex_responder = robots.at(uwb->responder_id).new_vertex(sensor_type.range, uwb->header, optimizer);

    auto edge = create_range_edge(vertex_requester, vertex_responder, uwb->distance, pow(uwb->distance_err, 2));
    optimizer.addEdge(edge);

    bool requester_not_static = robots.at(uwb->requester_id).not_static();



    Matrix3d T_matrix;
    T_matrix << 1-2*qy*qy-2*qz*qz, 2*qx*qy-2*qz*qw, 2*qx*qz+2*qy*qw,
                2*qx*qy + 2*qz*qw, 1-2*qx*qx-2*qz*qz, 2*qy*qz-2*qx*qw,                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              
                2*qx*qz-2*qy*qw, 2*qy*qz + 2*qx*qw, 1-2*qx*qx-2*qy*qy;

    Vector3d imu_acceleration = Vector3d(Imu.linear_acceleration.x,Imu.linear_acceleration.y,Imu.linear_acceleration.z);
    Vector3d gravity = Vector3d(0,0,-9.8);
    Vector3d acceleration = T_matrix.inverse()*imu_acceleration+gravity;

    Vector3d angular = Vector3d(Imu.angular_velocity.x,Imu.angular_velocity.y,Imu.angular_velocity.z);
    


    Vector3d  last_vertex_velocity;
    if(uwb_number < 2)
    {
        last_vertex_velocity = Vector3d::Zero();
    }
    else
    {
        last_vertex_velocity = vertex_last_requester->estimate().translation() - last_last_vertex->estimate().translation();
        judge = last_vertex_velocity.norm();

        // cout << "translation" << last_vertex_velocity <<endl;

        // cout << "judge" << '\n' <<  judge <<endl;

        last_vertex_velocity = last_vertex_velocity/dt_requester;
    }
    last_last_vertex = vertex_last_requester;


    if (uwb_number >10)
    {

    // Vector3d  translation = last_vertex_velocity*dt_requester+0.5*acceleration*pow(dt_requester,2);

    Vector3d  translation = last_vertex_velocity*dt_requester + 0.5*acceleration*pow(dt_requester,2);

    cout << "translation" << translation <<endl;

    Eigen::Isometry3d last_pose;
    last_pose.setIdentity(); // very important
    last_pose.rotate(last_rotation);
    last_pose.translate(g2o::Vector3D(0, 0, 0));


    // cout << "current" << '\n' << current_pose.matrix() << endl;

    // cout << "information" << '\n' << current_pose.rotation() << endl;

    // cout << "information" << '\n' << current_pose.translation() << endl;

    // calculate transform matrix
    Isometry3d transform_matrix = last_pose.inverse()*current_pose;
    last_rotation = imu_rotation;

    // Isometry3d transform_matrix = current_pose;

    translation = last_pose.rotation().inverse()*translation;
    // transform_matrix(0,3) = translation(0);
    // transform_matrix(1,3) = translation(1);
    // transform_matrix(2,3) = translation(2);


    // transform_matrix(0,3) = pow(translation(0),2);
    // transform_matrix(1,3) = pow(translation(1),2);
    // transform_matrix(2,3) = pow(translation(2),2);

    cout << "information" << '\n' <<  transform_matrix.matrix() <<endl;

    Eigen::Map<Eigen::MatrixXd> covariance_orientation(Imu.orientation_covariance.data(), 3, 3);
    Eigen::Map<Eigen::MatrixXd> covariance_translation(Imu.linear_acceleration_covariance.data(), 3, 3);   
    Eigen::Matrix3d zero_matrix = Eigen::Matrix3d::Zero();

    Eigen::MatrixXd SE3information(6,6);

    covariance_translation << abs(translation(0)),0,0,
                              0,abs(translation(1)),0,
                              0,0,abs(translation(2));

    // covariance_translation << pow(robot_max_velocity*dt_requester/3, 2),0,0,
    //                           0,pow(robot_max_velocity*dt_requester/3, 2),0,
    //                           0,0,pow(robot_max_velocity*dt_requester/3, 2);

    // cout << "covariance" << pow(robot_max_velocity*dt_requester/3, 2) <<endl;
    
    // double robot_max_angular = 1.5;
    // double kk = 1;

    // covariance_orientation << kk*pow(robot_max_angular*dt_requester/3, 2),0,0,
    //                           0,kk*pow(robot_max_angular*dt_requester/3, 2),0,
    //                           0,0,kk*pow(robot_max_angular*dt_requester/3, 2);

    // double cov_requester = pow(robot_max_velocity*dt_requester/3, 2);
    // covariance_translation << cov_requester,0,0,
    //                           0,cov_requester,0,
    //                           0,0,cov_requester;

    SE3information << covariance_translation,zero_matrix,
                      zero_matrix,covariance_orientation;

    // cout << "orientation" << '\n' <<  Imu.orientation_covariance[8] <<endl;

    // cout << "orientation" << '\n' <<  covariance_orientation <<endl;

    cout << "information" << '\n' <<  SE3information <<endl;
    cout << "matrix" << '\n' <<  transform_matrix.matrix() <<endl;

    // cout << "information inverse" << '\n' <<  SE3information.inverse().matrix() << endl;

    // requester to requester's last vertex edge

    if (requester_not_static)
    {

        ROS_WARN("IMU IMU IMU");

        g2o::EdgeSE3 *SE3edge = new g2o::EdgeSE3();
    
        SE3edge->vertices()[0] = vertex_last_requester;

        SE3edge->vertices()[1] = vertex_requester; 

        SE3edge->setMeasurement(transform_matrix);

        SE3edge->setInformation(SE3information.inverse());

        edge->setRobustKernel(new g2o::RobustKernelHuber());
    
        optimizer.addEdge( SE3edge );

    }


    // current_pose(0,3) = vertex_last_requester->estimate().translation()[0];
    // current_pose(1,3) = vertex_last_requester->estimate().translation()[1];
    // current_pose(2,3) = vertex_last_requester->estimate().translation()[2];

    if (requester_not_static)
    {

    g2o::EdgeSE3Prior* e1=new g2o::EdgeSE3Prior();
    Eigen::MatrixXd  pinfo_1(6,6);
    pinfo_1 << zero_matrix,zero_matrix,
               zero_matrix,zero_matrix;

    pinfo_1(0,0)= 0;
    pinfo_1(1,1)= 0;
    pinfo_1(2,2)= 0;

    pinfo_1(3,3)= 1e8;; //a very large number
    pinfo_1(4,4)= 1e8;;
    pinfo_1(5,5)= 1e8;; // X ,Y , Z 

    cout << "pin" << pinfo_1 <<endl;

    e1->setInformation(pinfo_1);
    e1->vertices()[0]= vertex_requester; //This is to fix the g2o graph on this point

    e1->setMeasurement(current_pose); 
    e1->setParameterId(0,0);
    optimizer.addEdge(e1); //e1 is to fix v1 at origin

    }


    // Isometry3d request_matrix = current_pose;
    // Eigen::MatrixXd information(6,6);

    // information <<    covariance_orientation.inverse(),zero_matrix,
    //                   zero_matrix,zero_matrix;

    // information(0,0)= 1e9;
    // information(1,1)= 1e9;
    // information(2,2)= 1e9;

    // information(3,3)= 1e9;
    // information(4,4)= 1e9;
    // information(5,5)= 1e9;


    // if (requester_not_static)
    // {

    //     ROS_WARN("rotation rotation rotation");

    //     g2o::EdgeSE3 *SE3edge = new g2o::EdgeSE3();
    
    //     SE3edge->vertices()[0] = vertex_responder;

    //     SE3edge->vertices()[1] = vertex_requester; 

    //     SE3edge->setMeasurement(request_matrix);

    //     SE3edge->setInformation(information);

    //     edge->setRobustKernel(new g2o::RobustKernelHuber());
    
    //     optimizer.addEdge( SE3edge );

    // }

    } 

    ROS_WARN("Localization: added range edge id: %d", uwb->header.seq);

    solve();

    publish();


}


inline Eigen::Isometry3d Localization::twist2transform(geometry_msgs::TwistWithCovariance& twist, Eigen::MatrixXd& covariance, double dt)
{
    tf::Vector3 translation, euler;

    tf::vector3MsgToTF(twist.twist.linear, translation);

    tf::vector3MsgToTF(twist.twist.angular, euler);

    tf::Quaternion quaternion;

    quaternion.setRPY(euler[0]*dt, euler[1]*dt, euler[2]*dt);

    tf::Transform transform(quaternion, translation * dt);

    Eigen::Isometry3d measurement;

    tf::transformTFToEigen(transform, measurement);

    Eigen::Map<Eigen::MatrixXd> cov(twist.covariance.data(), 6, 6);

    covariance = cov*dt*dt;

    return measurement;
}


inline g2o::EdgeSE3* Localization::create_se3_edge_from_twist(g2o::VertexSE3* vetex1, g2o::VertexSE3* vetex2, geometry_msgs::TwistWithCovariance& twist, double dt)
{
    g2o::EdgeSE3 *edge = new g2o::EdgeSE3();

    edge->vertices()[0] = vetex1;

    edge->vertices()[1] = vetex2;

    Eigen::MatrixXd covariance;

    auto measurement = twist2transform(twist, covariance, dt);

    edge->setMeasurement(measurement);

    edge->setInformation(covariance.inverse());

    edge->setRobustKernel(new g2o::RobustKernelHuber());

    return edge;
}


inline g2o::EdgeSE3Range* Localization::create_range_edge(g2o::VertexSE3* vertex1, g2o::VertexSE3* vertex2, double distance, double covariance)
{
    auto edge = new g2o::EdgeSE3Range();

    edge->vertices()[0] = vertex1;

    edge->vertices()[1] = vertex2;

    edge->setMeasurement(distance);

    Eigen::MatrixXd covariance_matrix = Eigen::MatrixXd::Zero(1, 1);

    covariance_matrix(0,0) = covariance;

    edge->setInformation(covariance_matrix.inverse());

    edge->setRobustKernel(new g2o::RobustKernelHuber());

    return edge;
}


inline void Localization::save_file(geometry_msgs::PoseStamped pose)
{
    file.open(filename.c_str(), ios::app);

    file<<boost::format("%.9f") % (pose.header.stamp.toSec())<<" "
        <<pose.pose.position.x<<" "
        <<pose.pose.position.y<<" "
        <<pose.pose.position.z<<" "
        <<pose.pose.orientation.x<<" "
        <<pose.pose.orientation.y<<" "
        <<pose.pose.orientation.z<<" "
        <<pose.pose.orientation.w<<endl;
    
    file.close();
}


void Localization::set_file(string name_prefix)
{
    flag_save_file = true;
    char s[30];
    struct tm tim;
    time_t now;
    now = time(NULL);
    tim = *(localtime(&now));
    strftime(s,30,"_%Y_%b_%d_%H_%M_%S.txt",&tim);
    filename = name_prefix + string(s);
    file.open(filename.c_str(), ios::trunc|ios::out);
    file<<"# "<<"iteration_max:"<<iteration_max<<"\n";
    file.close();
}
