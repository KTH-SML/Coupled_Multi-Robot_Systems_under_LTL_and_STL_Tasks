#include "ros/ros.h"
#include "hybrid_controller/CriticalEvent.h"
#include "hybrid_controller/Params.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/Twist.h"
#include <boost/bind.hpp>
#include <string>
#include <vector>
#include <cmath>
#include "PPC.hpp"
#include <armadillo>
#include "arma_ros_std_conversions.h"

class ControllerNode{
	ros::Publisher control_input_pub, critical_event_pub, collaboration_request_pub;
	std::vector<ros::Subscriber> pose_subs, collaboration_params_subs;

	std::vector<geometry_msgs::PoseStamped> poses;
	std::vector<int> c;
	std::vector<hybrid_controller::Params> params;
	arma::vec X;

	std::vector<std::vector<int>> clusters;

	PPC prescribed_performance_controller;
	int robot_id;

	std::vector<bool> state_was_read;

public:
	ControllerNode(ros::NodeHandle nh, ros::NodeHandle priv_nh, PPC ppc, int n_robots, int robot_id, std::vector<int> robots_in_cluster, arma::vec u_max): prescribed_performance_controller(ppc), robot_id(robot_id){
		state_was_read = std::vector<bool>(n_robots, false);
		X = arma::vec(3*n_robots);

		control_input_pub = nh.advertise<geometry_msgs::Twist>("/cmdvel", 100);
		critical_event_pub = nh.advertise<hybrid_controller::CriticalEvent>("/critical_event"+std::to_string(robot_id), 100);
		collaboration_request_pub = nh.advertise<hybrid_controller::Params>("/collaboration_params", 100);

		poses = std::vector<geometry_msgs::PoseStamped>(n_robots);

		for(int i=0; i<n_robots; i++){
			pose_subs.push_back(
				nh.subscribe<geometry_msgs::PoseStamped>(
					"/pose_robot" + std::to_string(i),
					100,
					boost::bind(&ControllerNode::poseCallback, this, _1, i)));
		}
		for(int i=0; i<robots_in_cluster.size(); i++){
			collaboration_params_subs.push_back(
				nh.subscribe<hybrid_controller::Params>(
					"/collaboration_params"+std::to_string(robots_in_cluster[i]), 
					100, 
					boost::bind(&ControllerNode::externalCollaborationRequestCallback, this, _1, robots_in_cluster[i])));
		}
	}

	void externalCollaborationRequestCallback(const hybrid_controller::Params::ConstPtr& msg, int i){
		prescribed_performance_controller.externalTaskChangeCallback(
			i, msg->c,
			msg->t_star, msg->r, msg->rho_max,
			msg->gamma_0, msg->gamma_inf, msg->l);
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

		geometry_msgs::Twist u_msg;
		double c = cos(X(robot_id*3+2));
		double s = sin(X(robot_id*3+2));
		u_msg.linear.x = (u(0)*c + u(1)*s)*1000.0;
		u_msg.linear.y = (-u(0)*s + u(1)*c)*1000.0;
		control_input_pub.publish(u_msg);
	}

	void setCriticalEventCallback(void (*callback)(CriticalEventParam)){
		prescribed_performance_controller.criticalEventCallback = callback;
	}

	void setCollaborationRequestCallback(void (*callback)(CollaborationRequestParam)){
		prescribed_performance_controller.collaborationRequest = callback;
	}

	void publishCriticalEvent(CriticalEventParam critical_event_param){
		hybrid_controller::CriticalEvent ce_msg;
		ce_msg.stamp = ros::Time::now();
		ce_msg.rho_max = critical_event_param.rho_max;
		ce_msg.r = critical_event_param.r;
		ce_msg.gamma_0 = critical_event_param.gamma_0;
		ce_msg.gamma_inf = critical_event_param.gamma_inf;
		ce_msg.l = critical_event_param.l;
		ce_msg.t_star = critical_event_param.t_star;
		critical_event_pub.publish(ce_msg);
	}

	void publishCollaborationRequest(CollaborationRequestParam collaboration_request_param){
		hybrid_controller::Params msg;
		msg.stamp = ros::Time::now();
		msg.rho_max = collaboration_request_param.rho_max;
		msg.r = collaboration_request_param.r;
		msg.gamma_0 = collaboration_request_param.gamma_0;
		msg.gamma_inf = collaboration_request_param.gamma_inf;
		msg.l = collaboration_request_param.l;
		msg.t_star = collaboration_request_param.t_star;
		collaboration_request_pub.publish(msg);
	}
};

class CriticalEvent{
public:
	static ControllerNode* controller_node;
	static void criticalEventCallback(CriticalEventParam critical_event_param){
		controller_node->publishCriticalEvent(critical_event_param);
	}
	static void collaborationRequestCallback(CollaborationRequestParam collaboration_request_param){
		controller_node->publishCollaborationRequest(collaboration_request_param);
	}
};
ControllerNode* CriticalEvent::controller_node;

void readParameters(ros::NodeHandle nh, ros::NodeHandle priv_nh, int& n_robots, int& robot_id, int& K, int& freq,
		std::vector<std::string>& formula, std::vector<std::string>& formula_type,
		std::vector<std::vector<std::string>>& dformula,
		std::vector<int>& cluster, std::vector<int>& robots_in_cluster,
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

	for(int i=0; i<cluster.size(); i++){
		if(cluster[robot_id] == cluster[i]){
			robots_in_cluster.push_back(i);
		}
	}
}

int main(int argc, char* argv[]){
	ros::init(argc, argv, "hybrid_controller_node");
	ros::NodeHandle nh;
	ros::NodeHandle priv_nh("~");

	int n_robots, robot_id, K, freq;
	std::vector<std::string> formula, formula_type;
	std::vector<std::vector<std::string>> dformula;
	std::vector<int> cluster, robots_in_cluster;
	std::vector<double> a, b, rho_opt;
	arma::vec u_max;

	readParameters(nh, priv_nh, n_robots, robot_id, K, freq, 
		formula, formula_type, dformula, cluster, robots_in_cluster, a, b, rho_opt, u_max);

	PPC ppc(robot_id, a, b, 
			formula_type, formula[robot_id],
			dformula[robot_id], rho_opt[robot_id], K, u_max,
			robots_in_cluster);

	ControllerNode controller_node(nh, priv_nh, ppc, n_robots, robot_id, robots_in_cluster, u_max);

	CriticalEvent::controller_node = &controller_node;
	controller_node.setCriticalEventCallback(CriticalEvent::criticalEventCallback);
	controller_node.setCollaborationRequestCallback(CriticalEvent::collaborationRequestCallback);

	ros::Rate rate(freq);
	while(ros::ok()){
		ros::spinOnce();
		controller_node.update();
		rate.sleep();
	}
}
