#ifndef SLAM_GRAPHMAPPER_HPP
#define SLAM_GRAPHMAPPER_HPP

/**
 * @mainpage A generic frontend for 3D Simultaneous Localization and Mapping
 * 
 * @section sec_motiv Motivation
 * 
 * This library provides a frontend for a graph-based SLAM in three dimensional space.
 * It does not provide a graph-optimization-backend itself (often referred to as SLAM).
 * Instead different backends can be used by implementing the Solver-Interface.
 * 
 * @section sec_start Getting started
 * 
 * The central component of this library is the GraphMapper class.
 * The documentation is best read by starting from there.
 * This class is extended by registering Sensor modules, an Odometry and a Solver.
 * 
 * @section sec_example Programming example
 * 
 * Start by creating the mapper itself and registering the required modules.
 @code
#include <slam3d/GraphMapper.hpp> 
#include <slam3d/G2oSolver.hpp>

using namespace slam3d;
Clock* clock = new Clock();
Logger* logger = new Logger(*c);
GraphMapper* mapper = new GraphMapper(logger);

Sensor* laser = new PointCloudSensor("laser", logger, Transform::Identity());
mapper->registerSensor(laser);

G2oSolver* g2o = new G2oSolver(logger);
mapper->setSolver(g2o);
 @endcode
 * Within the callback of your sensor data, add the new measurements to the mapper.
 @code
Measurement m* = new PointCloudMeasurement(cloud, "my_robot", laser->getName(), laser->getSensorPose());
if(!mapper->addReading(m))
{
  delete m;
}
 @endcode
 */

#include "Odometry.hpp"
#include "Sensor.hpp"

#include <graph_analysis/BaseGraph.hpp>
#include <flann/flann.hpp>
#include <map>

namespace slam3d
{
	/**
	 * @class VertexObject
	 * @brief Object attached to a vertex in the pose graph.
	 * @details It contains a pointer to an abstract measurement, which could
	 * be anything, e.g. a range scan, point cloud or image.
	 */
	class VertexObject : public graph_analysis::Vertex
	{
	public:
		typedef boost::shared_ptr<VertexObject> Ptr;
		typedef boost::shared_ptr<const VertexObject> ConstPtr;

		VertexObject(const std::string& label = ""):graph_analysis::Vertex(label){}

		Transform corrected_pose;
		Measurement* measurement;

		std::string getClassName() const { return "slam3d::VertexObject"; }
	};

	/**
	 * @class EdgeObject
	 * @brief Object attached to an edge in the pose graph.
	 * @details It contains the relative transform from source to target,
	 * the associated covariance matrix and the name of the sensor that
	 * created this spatial relationship.
	 */
	class EdgeObject : public graph_analysis::Edge
	{
	public:
		typedef boost::shared_ptr<EdgeObject> Ptr;
		typedef boost::shared_ptr<const EdgeObject> ConstPtr;

		EdgeObject(const std::string& s, const std::string& l = ""):graph_analysis::Edge(l),sensor(s){}

		Transform transform;
		Covariance covariance;
		std::string sensor;

		std::string getClassName() const { return "slam3d::EdgeObject"; }
	};

	typedef std::vector<VertexObject::Ptr> VertexList;
	typedef std::vector<EdgeObject::Ptr> EdgeList;
	typedef std::map<std::string, Sensor*> SensorList;
	typedef std::map<boost::uuids::uuid, VertexObject::Ptr> VertexIndex;
	
	typedef flann::Index< flann::L2<float> > NeighborIndex;
	
	/**
	 * @class BadElementType
	 * @brief Exception thrown when element types do not match.
	 * @details This happens when an element returned from 
	 * graph_analysis::BaseGraph is not of the expected slam type,
	 * e.g. VertexObject or EdgeObject.
	 */
	class BadElementType: public std::exception
	{
	public:
		BadElementType(){}
		virtual const char* what() const throw()
		{
			return "Could not convert from base-type to slam-type!";
		}
	};

	class Solver;

	/**
	 * @class GraphMapper
	 * @brief Holds measurements from different sensors in a graph.
	 * @details The GraphMapper is the central structure that provides the
	 * frontend for a graph-based SLAM approach. A registered Sensor
	 * will provide a specific Measurement type to the internal graph.
	 * For each added measurement a new vertex is created in the graph
	 * that holds a pointer to the measurement together with the measurement's
	 * pose in the map coordinate frame. This data is stored in a VertexObject.
	 * 
	 * Spatial relations between measurements are represented as edges in the
	 * graph. A registered Odometry will provide spatial constraints between any
	 * kind of consecutive measurements. Each sensor can create constraints
	 * between its own measurements by applying some kind of matching algoithm.
	 * This kind of 6 DoF spatial relation is stored as transform and covariance
	 * within an EdgeObject.
	 * 
	 * The global optimization is provided by a Solver that takes the internal
	 * nodes and edges (without the measurements) and solves the SLAM problem by
	 * applying a graph optimization algorithm. This will usually change the
	 * poses of all nodes in the map coordinate frame.
	 */
	class GraphMapper
	{
	public:
		GraphMapper(Logger* log);
		~GraphMapper();

		/**
		 * @brief Sets a specific Solver to be used as SLAM backend.
		 * @details The mapper can be used without a backend,
		 * but mapping results might be inconsistant.
		 * @param solver backend to be used for optimization
		 */
		void setSolver(Solver* solver);

		/**
		 * @brief Sets an odometry module to provide relative poses 
		 * @details Depending on the matching abilities of the
		 * used sensors (e.g. a 360° laser-scanner), the mapping might work
		 * correctly without an odometry module.
		 * @param odom odometry module
		 * @param add_edges whether to create odometry edges in the graph
		 */
		void setOdometry(Odometry* odom, bool add_edges = false);

		/**
		 * @brief Register a sensor, so its data can be added to the graph.
		 * @details Multiple sensors can be used, but in this case an odometry module
		 * is required for the mapping to work correctly. Matching is currently
		 * done only between measurements of the same sensor.
		 * @param s sensor to be registered for mapping
		 */
		void registerSensor(Sensor* s);

		/**
		 * @brief Add a new measurement to the graph.
		 * @details The sensor specified in the measurement has to be registered
		 * with the mapper before. If the change in robot pose since the last
		 * added scan is smaller then min-translation or min-rotation, the
		 * measurement will not be added. Use GraphMapper::setMinPoseDistance to
		 * adjust this distance.
		 * @param m pointer to a new measurement
		 * @return true if the measurement was added
		 */
		bool addReading(Measurement* m);
		
		/**
		 * @brief Add a new measurement from another robot.
		 * @param m pointer to a new measurement
		 * @param t pose in map coordinates
		 */
		void addExternalReading(Measurement* m, const Transform& t);

		/**
		 * @brief Get the current pose of the robot within the generated map.
		 * @details The pose is updated at least whenever a new node is added.
		 * Depending on the available information, it might be updated
		 * more often. (e.g. when odometry is available)
		 * @return current robot pose in map coordinates
		 */
		Transform getCurrentPose();
		
		/**
		 * @brief Start the backend optimization process.
		 * @details Requires that a Solver has been set with setSolver.
		 * @return true if optimization was successful
		 */
		bool optimize();
		
		/**
		 * @brief Gets a list with all vertices from a given sensor.
		 * @param sensor name of the sensor which vertices are requested
		 * @return list of all vertices from given sensor
		 */
		VertexList getVerticesFromSensor(const std::string& sensor);

		/**
		 * @brief Get a list with all edges from a given sensor.
		 * @param sensor name of the sensor which edges are requested
		 * @return list of all edges from given sensor
		 */
		EdgeList getEdgesFromSensor(const std::string& sensor);

		/**
		 * @brief Create the index for nearest neighbor search of nodes.
		 * @param sensor index nodes of this sensor
		 */
		void buildNeighborIndex(const std::string& sensor);

		/**
		 * @brief Search for nodes in the graph near the given pose.
		 * @details This does not refer to a NN-Search in the graph, but to search for
		 * spatially near poses according to their current corrected pose.
		 * If new nodes have been added, the index has to be created with
		 * a call to buildNeighborIndex.
		 * @param tf The pose where to search for nodes
		 * @param radius The radius within nodes should be returned
		 * @return list of spatially near vertices
		 */
		VertexList getNearbyVertices(const Transform &tf, float radius);

		/**
		 * @brief Get the last vertex, that was locally added to the graph.
		 * @details This will not return external vertices from other robots.
		 * @return last added vertex
		 */
		VertexObject::ConstPtr getLastVertex() { return mLastVertex; }

		/**
		 * @brief Write the current graph to a file (currently dot).
		 * @details For larger graphs, this can take a very long time.
		 * @param name filename without type ending
		 */
		void writeGraphToFile(const std::string &name);

		/**
		 * @brief Sets neighbor radius for matching
		 * @details New nodes are matched against all nodes of the same sensor
		 * within the given radius.
		 * @param r radius within additional edges are created
		 */
		void setNeighborRadius(float r){ mNeighborRadius = r; }

		/**
		 * @brief Set minimal change in pose between adjacent nodes.
		 * @param t Minimum translation between nodes (in meter).
		 * @param r Minimum rotation between nodes (in rad).
		 */
		void setMinPoseDistance(float t, float r){ mMinTranslation = t; mMinRotation = r; }

	private:
		VertexObject::Ptr addVertex(Measurement* m,
		                            const Transform &corrected);

		EdgeObject::Ptr addEdge(VertexObject::Ptr source,
		                        VertexObject::Ptr target,
		                        const Transform &t,
		                        const Covariance &c,
		                        const std::string &sensor,
		                        const std::string &label);

		bool checkMinDistance(const Transform &t);

		void linkToNeighbors(VertexObject::Ptr vertex, Sensor* sensor, int max_links);

		static VertexObject::Ptr fromBaseGraph(graph_analysis::Vertex::Ptr base);
		static EdgeObject::Ptr fromBaseGraph(graph_analysis::Edge::Ptr base);

	private:
		graph_analysis::BaseGraph::Ptr mPoseGraph;
		VertexObject::Ptr mLastVertex;
		VertexObject::Ptr mFirstVertex;

		Solver* mSolver;
		Logger* mLogger;
		Odometry* mOdometry;
		SensorList mSensors;

		Transform mCurrentPose;
		Transform mLastOdometricPose;

		// Index to use nearest neighbor search
		flann::SearchParams mSearchParams;
		NeighborIndex mIndex;
		std::map<int, VertexObject::Ptr> mIndexMap;

		// Index to find Vertices by their unique id
		VertexIndex mVertexIndex;

		// Parameters
		float mNeighborRadius;
		float mMinTranslation;
		float mMinRotation;
		bool mAddOdometryEdges;
	};
}

#endif
