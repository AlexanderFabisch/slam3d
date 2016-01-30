#ifndef SLAM3D_BOOSTMAPPER_HPP
#define SLAM3D_BOOSTMAPPER_HPP

#include "GraphMapper.hpp"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <flann/flann.hpp>

namespace slam3d
{
	// Definitions of boost-graph related types
	typedef boost::listS VRep;
	typedef boost::listS ERep;
	typedef boost::bidirectionalS GType;
	typedef boost::adjacency_list<VRep, ERep, GType, VertexObject, EdgeObject> AdjacencyGraph;
	
	typedef boost::graph_traits<AdjacencyGraph>::vertex_descriptor Vertex;
	typedef boost::graph_traits<AdjacencyGraph>::vertex_iterator VertexIterator;
	typedef std::pair<VertexIterator, VertexIterator> VertexRange;
	
	typedef boost::graph_traits<AdjacencyGraph>::edge_descriptor Edge;
	typedef boost::graph_traits<AdjacencyGraph>::edge_iterator EdgeIterator;
	typedef boost::graph_traits<AdjacencyGraph>::out_edge_iterator OutEdgeIterator;
	typedef boost::graph_traits<AdjacencyGraph>::in_edge_iterator InEdgeIterator;
	typedef std::pair<EdgeIterator, EdgeIterator> EdgeRange;
	
	typedef boost::graph_traits<AdjacencyGraph>::adjacency_iterator AdjacencyIterator;
	typedef std::pair<AdjacencyIterator, AdjacencyIterator> AdjacencyRange;
	
	// List types
	typedef std::vector<Vertex> VertexList;
	typedef std::vector<Edge> EdgeList;

	// Index types
	typedef flann::Index< flann::L2<float> > NeighborIndex;
	typedef std::map<IdType, Vertex> IndexMap;
	typedef std::map<boost::uuids::uuid, Vertex> UuidMap;
	
	class BoostMapper : public GraphMapper
	{
	public:
		BoostMapper(Logger* log);
		~BoostMapper();
		
		bool addReading(Measurement* m);
		void addExternalReading(Measurement* m, const Transform& t);
		
		VertexObjectList getVertexObjectsFromSensor(const std::string& sensor);
		EdgeObjectList getEdgeObjectsFromSensor(const std::string& sensor);
		
		const VertexObject& getLastVertex() { return mPoseGraph[mLastVertex]; }
		const VertexObject& getVertex(IdType id);
		
	private:
		Vertex addVertex(Measurement* m,
		                 const Transform &corrected);

		Edge addEdge(Vertex source,
		             Vertex target,
		             const Transform &t,
		             const Covariance &c,
		             const std::string &sensor,
		             const std::string &label);

		void linkToNeighbors(Vertex vertex, Sensor* sensor, int max_links);
		
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
		std::vector<Vertex> getNearbyVertices(const Transform &tf, float radius);

		/**
		 * @brief Start the backend optimization process.
		 * @details Requires that a Solver has been set with setSolver.
		 * @return true if optimization was successful
		 */
		bool optimize();
		
	private:
		// The boost graph object
		AdjacencyGraph mPoseGraph;
		Indexer mIndexer;
		
		// Index to map a vertex' id to its descriptor
		IndexMap mIndexMap;
		
		// Index to use nearest neighbor search
		// Whenever this index is created, we have to enumerate all vertices from 0 to n-1.
		// This mapping is kept in a separate map to later apply the result to the graph.
		flann::SearchParams mSearchParams;
		NeighborIndex mNeighborIndex;
		IndexMap mNeighborMap;

		// Index to find Vertices by their unique id
		UuidMap mVertexIndex;
		
		// Some special vertices
		Vertex mLastVertex;
		Vertex mFirstVertex;
	};
}

#endif
