#include "ros/ros.h"
#include "hybrid_controller/ControlInput.h"
#include "hybrid_controller/CriticalEvent.h"
#include "geometry_msgs/PoseStamped.h"
#include <boost/bind.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include "PPC.hpp"
#include <armadillo>
#include "arma_ros_std_conversions.h"
#include <iostream>

class ControllerNode{
	ros::Publisher control_input_pub, critical_event_pub;
	std::vector<ros::Subscriber> pose_subs;

	std::vector<geometry_msgs::PoseStamped> poses;
	arma::vec X;

	PPC prescribed_performance_controller;
	int robot_id;

	std::vector<bool> state_was_read;

public:
	ControllerNode(ros::NodeHandle nh, ros::NodeHandle priv_nh, PPC ppc, int n_robots, int robot_id, arma::vec u_max): prescribed_performance_controller(ppc), robot_id(robot_id){
		state_was_read = std::vector<bool>(n_robots, false);
		X = arma::vec(3*n_robots);

		std::string robot_id_str = std::to_string(robot_id);
		control_input_pub = nh.advertise<hybrid_controller::ControlInput>("/control_input_robot"+robot_id_str, 100);
		critical_event_pub = nh.advertise<hybrid_controller::CriticalEvent>("/critical_event"+robot_id_str, 100);

		poses = std::vector<geometry_msgs::PoseStamped>(n_robots);

		std::string topic_base = "/pose_robot";
		for(int i=0; i<n_robots; i++){
			pose_subs.push_back(
				nh.subscribe<geometry_msgs::PoseStamped>(
					topic_base + std::to_string(i),
					100,
					boost::bind(&ControllerNode::poseCallback, this, _1, i)));
		}
	}

	void poseCallback(const geometry_msgs::PoseStamped::ConstPtr& msg, int i){
		poses[i] = *msg;
		X(arma::span(3*i, 3*i+2)) = pose_to_vec(poses[i]);

		if(!state_was_read[i]){
			arma::vec x;
			if(i == robot_id){
				x = pose_to_vec(poses[robot_id]);
			}
			state_was_read[i] = true;
			if(std::all_of(state_was_read.cbegin(), state_was_read.cend(), [](bool v){return v;})){
				prescribed_performance_controller.init(ros::Time::now().toSec(), x, X);
			}
		}
	}

	void update(){
		if(std::any_of(state_was_read.cbegin(), state_was_read.cend(), [](bool v){return !v;})) return;
		arma::vec u = prescribed_performance_controller.u(arma::conv_to<std::vector<double>>::from(X), pose_to_std_vec(poses[robot_id]), ros::Time::now().toSec());

		hybrid_controller::ControlInput u_msg = vec_to_control_input(u);
		control_input_pub.publish(u_msg);
	}

	void setCriticalEventCallback(void (*callback)(CriticalEventParam)){
		prescribed_performance_controller.criticalEventCallback = callback;
	}

	void publishCriticalEvent(CriticalEventParam critical_event_param){
		hybrid_controller::CriticalEvent ce_msg;
		critical_event_pub.publish(ce_msg);
	}
};

class CriticalEvent{
public:
	static ControllerNode* controller_node;
	static void criticalEventCallback(CriticalEventParam critical_event_param){
		controller_node->publishCriticalEvent(critical_event_param);
	}
};
ControllerNode* CriticalEvent::controller_node;

void readParameters(ros::NodeHandle nh, ros::NodeHandle priv_nh, int& n_robots, int& robot_id, int& K, int& freq,
		std::vector<std::string>& formula, std::vector<std::string>& formula_type,
		std::vector<std::vector<std::string>>& dformula,
		std::vector<int>& cluster,
		std::vector<double>& a, std::vector<double>& b, std::vector<double>& rho_opt,
		arma::vec& u_max){
	nh.param<int>("control_freq", freq, 100);
	nh.param("n_robots", n_robots, 1);
	priv_nh.getParam("robot_id", robot_id);

	formula = std::vector<std::string>(n_robots);
	formula_type = std::vector<std::string>(n_robots);
	cluster = std::vector<int>(n_robots);
	a = std::vector<double>(n_robots);
	b = std::vector<double>(n_robots);
	rho_opt = std::vector<double>(n_robots);

	for(int i=0; i<n_robots; i++){
		std::string i_str = std::to_string(i);
		nh.getParam("formula"+i_str, formula[i]);
		nh.getParam("formula_type"+i_str, formula_type[i]);
		nh.getParam("cluster"+i_str, cluster[i]);
		
		std::vector<std::string> df;
		nh.getParam("dformula"+i_str, df);
		dformula.push_back(df);

		nh.getParam("a"+i_str, a[i]);
		nh.getParam("b"+i_str, b[i]);
		nh.getParam("rho_opt"+i_str, rho_opt[i]);
	}

	nh.getParam("K", K);

	std::vector<double> u_max_stdvec;
	nh.getParam("u_max", u_max_stdvec);
	u_max = arma::vec(u_max_stdvec);
}

int main(int argc, char* argv[]){
	ros::init(argc, argv, "hybrid_controller_node");
	ros::NodeHandle nh;
	ros::NodeHandle priv_nh("~");

	int n_robots, robot_id, K, freq;
	std::vector<std::string> formula, formula_type;
	std::vector<std::vector<std::string>> dformula;
	std::vector<int> cluster;
	std::vector<double> a, b, rho_opt;
	arma::vec u_max;

	readParameters(nh, priv_nh, n_robots, robot_id, K, freq, formula, formula_type, dformula, cluster, a, b, rho_opt, u_max);

	PPC ppc(robot_id, a[robot_id], b[robot_id], 
			formula_type[robot_id], formula[robot_id],
			dformula[robot_id], rho_opt[robot_id], K, u_max);

	ControllerNode controller_node(nh, priv_nh, ppc, n_robots, robot_id, u_max);

	CriticalEvent::controller_node = &controller_node;
	controller_node.setCriticalEventCallback(CriticalEvent::criticalEventCallback);

	ros::Rate rate(freq);
	while(ros::ok()){
		ros::spinOnce();
		controller_node.update();
		rate.sleep();
	}
}
