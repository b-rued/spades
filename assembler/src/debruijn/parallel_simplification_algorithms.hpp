#pragma once

#include "standard_base.hpp"
#include "omni/graph_processing_algorithm.hpp"
#include "omni/basic_edge_conditions.hpp"
#include "omni/bulge_remover.hpp"
#include "omni/abstract_conjugate_graph.hpp"
#include "simplification_settings.hpp"
#include <boost/range/adaptor/reversed.hpp>

namespace debruijn {

namespace simplification {

template<class Graph>
class ParallelTipClippingFunctor {
    typedef typename Graph::EdgeId EdgeId;
    typedef typename Graph::VertexId VertexId;
    typedef boost::function<void(EdgeId)> HandlerF;
    typedef omnigraph::PairedVertexLock<VertexId> VertexLockT;

    Graph& g_;
    size_t length_bound_;
    double coverage_bound_;
    HandlerF handler_f_;

    size_t LockingIncomingCount(VertexId v) const {
        VertexLockT lock(v);
        return g_.IncomingEdgeCount(v);
    }

    size_t LockingOutgoingCount(VertexId v) const {
        VertexLockT lock(v);
        return g_.OutgoingEdgeCount(v);
    }

    bool IsIncomingTip(EdgeId e) const {
        return g_.length(e) <= length_bound_
                && math::le(g_.coverage(e), coverage_bound_)
                && LockingIncomingCount(g_.EdgeStart(e)) + LockingOutgoingCount(g_.EdgeStart(e)) == 1;
    }

    void RemoveEdge(EdgeId e) {
        VertexLockT lock1(g_.EdgeStart(e));
        VertexLockT lock2(g_.EdgeEnd(e));
        g_.DeleteEdge(e);
    }

public:

    ParallelTipClippingFunctor(Graph& g, size_t length_bound, 
                                double coverage_bound, HandlerF handler_f = 0)
            : g_(g),
              length_bound_(length_bound),
              coverage_bound_(coverage_bound),
              handler_f_(handler_f) {

    }

    bool operator()(VertexId v) {
        if (LockingOutgoingCount(v) == 0)
            return false;

        vector<EdgeId> tips;
        //don't need lock here after the previous check
        for (EdgeId e : g_.IncomingEdges(v)) {
            if (IsIncomingTip(e)) {
                tips.push_back(e);
            }
        }
        
        //if all of edges are tips, leave the longest one
        if (!tips.empty() && tips.size() == g_.IncomingEdgeCount(v)) {
            sort(tips.begin(), tips.end(), omnigraph::LengthComparator<Graph>(g_));
            tips.pop_back();
        }

        for (EdgeId e : tips) {
            if (handler_f_) {
                handler_f_(e);
            }
            RemoveEdge(e);
        }
        return false;
    }
};

template<class Graph>
class ParallelSimpleBRFunctor {
    typedef typename Graph::EdgeId EdgeId;
    typedef typename Graph::VertexId VertexId;
    typedef omnigraph::PairedVertexLock<VertexId> VertexLockT;

    Graph& g_;
    size_t max_length_;
    double max_coverage_;
    double max_relative_coverage_;
    size_t max_delta_;
    double max_relative_delta_;
    boost::function<void(EdgeId)> handler_f_;

    bool LengthDiffCheck(size_t l1, size_t l2, size_t delta) const {
        return l1 <= l2 + delta  && l2 <= l1 + delta;
    }

    EdgeId Alternative(EdgeId e, const vector<EdgeId>& edges) const {
        size_t delta = omnigraph::CountMaxDifference(max_delta_, g_.length(e), max_relative_delta_);
        for (EdgeId candidate : boost::adaptors::reverse(edges)) {
            if (g_.EdgeEnd(candidate) == g_.EdgeEnd(e)
                    && candidate != e
                    && candidate != g_.conjugate(e)
                    && LengthDiffCheck(g_.length(candidate), g_.length(e), delta)) {
                return candidate;
            }
        }
        return EdgeId(0);
    }

    bool ProcessEdges(const vector<EdgeId>& edges) {
        for (EdgeId e : edges) {
            if (g_.length(e) <= max_length_ && math::le(g_.coverage(e), max_coverage_)) {
                EdgeId alt = Alternative(e, edges);
                if (alt != EdgeId(0)
                        && math::ge(g_.coverage(alt) * max_relative_coverage_, g_.coverage(e))) {
                    //todo is not work in multiple threads for now :)
                    //Reasons: id distribution, kmer-mapping
                    handler_f_(e);
                    g_.GlueEdges(e, alt);
                    return true;
                }
            }
        }
        return false;
    }

    vector<VertexId> MultiEdgeDestinations(VertexId v) const {
        vector<VertexId> answer;
        set<VertexId> destinations;
        for (EdgeId e : g_.OutgoingEdges(v)) {
            VertexId end = g_.EdgeEnd(e);
            if (destinations.count(end) > 0) {
                answer.push_back(end);
            }
            destinations.insert(end);
        }
        return answer;
    }

    VertexId SingleMultiEdgeDestination(VertexId v) const {
        vector<VertexId> dests = MultiEdgeDestinations(v);
        if (dests.size() == 1) {
            return dests.front();
        } else {
            return VertexId(0);
        }
    }

    void RemoveBulges(VertexId v) {
        bool flag = true;
        while (flag) {
            vector<EdgeId> edges(g_.out_begin(v), g_.out_end(v));
            if (edges.size() == 1)
                return;
            sort(edges.begin(), edges.end(), omnigraph::CoverageComparator<Graph>(g_));
            flag = ProcessEdges(edges);
        }
    }

    bool CheckVertex(VertexId v) const {
        VertexLockT lock(v);
        return MultiEdgeDestinations(v).size() == 1
                && MultiEdgeDestinations(g_.conjugate(v)).size() == 0;
    }

    size_t MinId(VertexId v) const {
        return std::min(v.int_id(), g_.conjugate(v).int_id());
    }

    bool IsMinimal(VertexId v1, VertexId v2) const {
        return MinId(v1) < MinId(v2);
    }

public:

    ParallelSimpleBRFunctor(Graph& g,
                            size_t max_length,
                            double max_coverage,
                            double max_relative_coverage,
                            size_t max_delta,
                            double max_relative_delta,
                            boost::function<void(EdgeId)> handler_f = 0)
            : g_(g),
              max_length_(max_length),
              max_coverage_(max_coverage),
              max_relative_coverage_(max_relative_coverage),
              max_delta_(max_delta),
              max_relative_delta_(max_relative_delta),
              handler_f_(handler_f) {

    }

    bool operator()(VertexId v/*, need number of vertex for stable id distribution*/) {
        vector<VertexId> multi_dest;

        {
            VertexLockT lock(v);
            multi_dest = MultiEdgeDestinations(v);
        }

        if (multi_dest.size() == 1 && IsMinimal(v, multi_dest.front())) {
            VertexId dest = multi_dest.front();
            if (CheckVertex(v) && CheckVertex(g_.conjugate(dest))) {
                VertexLockT lock1(v);
                VertexLockT lock2(dest);
                RemoveBulges(v);
            }
        }
        return false;
    }

};

//currently just a stab and not parallel at all,
//no way to write it parallel before deletion of edge requires two locks
template<class Graph>
class ParallelLowCoverageFunctor {
    typedef typename Graph::EdgeId EdgeId;
    typedef typename Graph::VertexId VertexId;
    typedef boost::function<void(EdgeId)> HandlerF;
    typedef omnigraph::PairedVertexLock<VertexId> VertexLockT;

    Graph& g_;
    shared_ptr<func::Predicate<EdgeId>> ec_condition_;
    HandlerF handler_f_;

    vector<EdgeId> edges_to_remove_;

public:

    ParallelLowCoverageFunctor(Graph& g,
                               size_t max_length,
                               double max_coverage,
                               HandlerF handler_f = 0)
            : g_(g),
              ec_condition_(
                      func::And<EdgeId>(
                      func::And<EdgeId>(make_shared<omnigraph::LengthUpperBound<Graph>>(g, max_length),
                                        make_shared<omnigraph::CoverageUpperBound<Graph>>(g, max_coverage)),
                      make_shared<omnigraph::AlternativesPresenceCondition<Graph>>(g))),
              handler_f_(handler_f)
    {

    }

    bool operator()(EdgeId e) {
        if (ec_condition_->Check(e)) {
            edges_to_remove_.push_back(e);
        }
        return false;
    }

    void RemoveCollectedEdges() {
        omnigraph::SmartSetIterator<Graph, EdgeId> to_delete(g_, edges_to_remove_.begin(), edges_to_remove_.end());
        while (!to_delete.IsEnd()) {
            EdgeId e = *to_delete;
            handler_f_(e);
            g_.DeleteEdge(e);
            ++to_delete;
        }
    }

};

template<class Graph>
class ParallelCompressor {
    typedef typename Graph::EdgeId EdgeId;
    typedef typename Graph::EdgeData EdgeData;
    typedef typename Graph::VertexId VertexId;
    typedef omnigraph::PairedVertexLock<VertexId> VertexLockT;

    Graph& g_;
    typename Graph::HelperT helper_;
    restricted::IdSegmentStorage segment_storage_;

    bool IsBranching(VertexId v) const {
//        VertexLockT lock(v);
        return !g_.CheckUniqueOutgoingEdge(v) || !g_.CheckUniqueIncomingEdge(v);
    }

    size_t LockingIncomingCount(VertexId v) const {
        VertexLockT lock(v);
        return g_.IncomingEdgeCount(v);
    }

    size_t LockingOutgoingCount(VertexId v) const {
        VertexLockT lock(v);
        return g_.OutgoingEdgeCount(v);
    }

    vector<VertexId> LockingNextVertices(VertexId v) const {
        VertexLockT lock(v);
        vector<VertexId> answer;
        for (EdgeId e : g_.OutgoingEdges(v)) {
            answer.push_back(g_.EdgeEnd(e));
        }
        return answer;
    }

    vector<VertexId> FilterBranchingVertices(const vector<VertexId>& vertices) const {
        vector<VertexId> answer;
        for (VertexId v : vertices) {
            VertexLockT lock(v);
            if (!IsBranching(v)) {
                answer.push_back(v);
            }
        }
        return answer;
    }

    //correctly handles self-conjugate case
    bool IsMinimal(VertexId v1, VertexId v2) const {
        return !(g_.conjugate(v2) < v1);
    }

    //true if need to go further, false if stop on any reason!
    //to_compress is not empty only if compression needs to be done
    //don't need additional checks for v == init | conjugate(init), because init is branching!
    //fixme what about plasmids?! =)
    bool ProcessNextAndGo(VertexId& v, VertexId init, vector<VertexId>& to_compress) {
        VertexLockT lock(v);
        if (!CheckConsistent(v)) {
            to_compress.clear();
            return false;
        }
        if (IsBranching(v) ) {
            if (!IsMinimal(init, v)) {
                to_compress.clear();
            }
            return false;
        } else {
            to_compress.push_back(v);
            v = g_.EdgeEnd(g_.GetUniqueOutgoingEdge(v));
            return true;
        }
    }

    void UnlinkEdge(VertexId v, EdgeId e) {
        VertexLockT lock(v);
        helper_.DeleteLink(v, e);
    }

    void UnlinkEdges(VertexId v) {
        VertexLockT lock(v);
        helper_.DeleteLink(v, g_.GetUniqueOutgoingEdge(v));
        helper_.DeleteLink(g_.conjugate(v), g_.GetUniqueOutgoingEdge(g_.conjugate(v)));
    }

    //todo duplication with abstract conj graph
    vector<EdgeId> EdgesToDelete(const vector<EdgeId> &path) const {
        set<EdgeId> edgesToDelete;
        edgesToDelete.insert(path[0]);
        for (size_t i = 0; i + 1 < path.size(); i++) {
            EdgeId e = path[i + 1];
            if (edgesToDelete.find(g_.conjugate(e)) == edgesToDelete.end())
                edgesToDelete.insert(e);
        }
        return vector<EdgeId>(edgesToDelete.begin(), edgesToDelete.end());
    }

    vector<VertexId> VerticesToDelete(
            const vector<EdgeId> &path) const {
        set<VertexId> verticesToDelete;
        for (size_t i = 0; i + 1 < path.size(); i++) {
            EdgeId e = path[i + 1];
            VertexId v = g_.EdgeStart(e);
            if (verticesToDelete.find(g_.conjugate(v)) == verticesToDelete.end())
                verticesToDelete.insert(v);
        }
        return vector<VertexId>(verticesToDelete.begin(),
                                verticesToDelete.end());
    }
    //todo end duplication with abstract conj graph

    //not locking!
    vector<EdgeId> CollectEdges(const vector<VertexId>& to_compress) const {
        vector<EdgeId> answer;
        answer.push_back(g_.GetUniqueIncomingEdge(to_compress.front()));
        for (VertexId v : to_compress) {
            answer.push_back(g_.GetUniqueOutgoingEdge(v));
        }
        return answer;
    }

    void CallHandlers(const vector<EdgeId>& edges, EdgeId new_edge) const {
        g_.FireMerge(edges, new_edge);
        g_.FireDeletePath(EdgesToDelete(edges), VerticesToDelete(edges));
        g_.FireAddEdge(new_edge);
    }

    EdgeData MergedData(const vector<EdgeId>& edges) const {
        vector<const EdgeData*> to_merge;
        for (EdgeId e : edges) {
            to_merge.push_back(&(g_.data(e)));
        }
        return g_.master().MergeData(to_merge);
    }

    EdgeId AddEdge(VertexId v1, VertexId v2, const EdgeData& data, IdDistributor& id_distributor) {
        EdgeId new_edge = helper_.AddEdge(data, id_distributor);
        {
            VertexLockT lock(v1);
            helper_.LinkOutgoingEdge(v1, new_edge);
        }
        {
            VertexLockT lock(v2);
            helper_.LinkIncomingEdge(v2, new_edge);
        }
        return new_edge;
    }

    void ProcessBranching(VertexId next, VertexId init, size_t idx) {
        vector<VertexId> to_compress;
        while (ProcessNextAndGo(next, init, to_compress)) {
        }

        if (!to_compress.empty()) {
            //here we are sure that we are the ones to process the path
            //so we can collect edges without any troubles (and actually without locks todo check!)
            vector<EdgeId> edges = CollectEdges(to_compress);

            restricted::ListIdDistributor<restricted::SegmentIterator> id_distributor =
                    segment_storage_.GetSegmentIdDistributor(2 * idx, 2 * idx + 1);

            EdgeId new_edge = AddEdge(g_.EdgeStart(edges.front()), g_.EdgeEnd(edges.back()),
                                      MergeSequences(g_, edges), id_distributor);

            CallHandlers(edges, new_edge);

            VertexId final = g_.EdgeEnd(edges.back());
            UnlinkEdge(init, edges.front());
            for (VertexId v: to_compress) {
                UnlinkEdges(v);
            }
            UnlinkEdge(g_.conjugate(final), g_.conjugate(edges.back()));

            for (EdgeId e : EdgesToDelete(edges)) {
                helper_.DeleteUnlinkedEdge(e);
            }
        }
    }

    //not needed here, but could check if vertex is fully isolated
    bool CheckConsistent(VertexId v) const {
        //todo change to incoming edge count
        return g_.OutgoingEdgeCount(g_.conjugate(v)) > 0;
    }

    //long, but safe way to get left neighbour
    //heavily relies on the current graph structure!
    VertexId LockingGetInit(VertexId v) {
        VertexLockT lock(v);
        if (!CheckConsistent(v))
            return VertexId(0);

        //works even if this edge is already unlinked from the vertex =)
        VERIFY(g_.CheckUniqueIncomingEdge(v));
        return g_.EdgeStart(g_.GetUniqueIncomingEdge(v));
    }

public:

    ParallelCompressor(Graph& g)
            : g_(g), helper_(g_.GetConstructionHelper()) {

    }

    //returns true iff v is the "leftmost" vertex to compress in the chain
    bool IsOfInterest(VertexId v) const {
        return !IsBranching(v) && IsBranching(g_.EdgeStart(g_.GetUniqueIncomingEdge(v)));
    }

    void PrepareForProcessing(size_t interesting_cnt) {
        segment_storage_ =
                g_.GetGraphIdDistributor().Reserve(interesting_cnt * 2);
    }

    bool Process(VertexId v, size_t idx) {
        if (IsBranching(v)) {
            VertexId init = LockingGetInit(v);
            if (init != VertexId(0))
                ProcessBranching(v, init, idx);
        }
        return false;
    }

};

//todo generalize to edges
template<class Graph, class ElementType>
class TwoStepAlgorithmRunner {
    typedef typename Graph::VertexId VertexId;
    typedef typename Graph::EdgeId EdgeId;

    const Graph& g_;
    vector<ElementType> elements_of_interest_;
protected:

    const Graph& g() const {
        return g_;
    }

    TwoStepAlgorithmRunner(Graph& g) : g_(g) {

    }

    template <class Algo, class It>
    void RunFromIterator(Algo& algo, It begin, It end) {
        //todo make parallel
        for (auto it = begin; it != end; ++it) {
            if (algo.IsOfInterest(*it)) {
                elements_of_interest_.push_back(*it);
            }
        }
        algo.PrepareForProcessing(elements_of_interest_.size());
        for (size_t i = 0; i < elements_of_interest_.size(); ++i) {
            algo.Process(elements_of_interest_[i], i);
        }
    }
};

template<class Graph>
class TwoStepVertexAlgorithmRunner : public TwoStepAlgorithmRunner<Graph, typename Graph::VertexId> {
    typedef typename Graph::VertexId VertexId;
    typedef typename Graph::EdgeId EdgeId;
    typedef TwoStepAlgorithmRunner<Graph, VertexId> base;

public:

    TwoStepVertexAlgorithmRunner(Graph& g) : base(g) {

    }

    template <class Algo>
    void Run(Algo& algo) {
        this->RunFromIterator(algo, this->g().begin(), this->g().end());
    }
};

//template<class Graph>
//class TwoStepEdgeAlgorithmRunner : public TwoStepAlgorithmRunner<Graph, typename Graph::VertexId> {
//    typedef typename Graph::VertexId VertexId;
//    typedef typename Graph::EdgeId EdgeId;
//    typedef TwoStepAlgorithmRunner<Graph, VertexId> base;
//
//public:
//
//    TwoStepEdgeAlgorithmRunner(Graph& g) : base(g) {
//
//    }
//
//    template <class Algo>
//    void Run(Algo& algo) const {
//        this->RunFromIterator(algo, this->g().ConstEdgeBegin());
//    }
//};

}

}
