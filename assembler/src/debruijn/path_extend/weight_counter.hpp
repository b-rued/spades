//***************************************************************************
//* Copyright (c) 2011-2013 Saint-Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//****************************************************************************

/*
 * weight_counter.hpp
 *
 *  Created on: Feb 19, 2012
 *      Author: andrey
 */

#ifndef WEIGHT_COUNTER_HPP_
#define WEIGHT_COUNTER_HPP_

#include "bidirectional_path.hpp"
#include "paired_library.hpp"
#include <algorithm>
#include <boost/math/special_functions/fpclassify.hpp>

namespace path_extend {

struct EdgeWithPairedInfo {
	size_t e_;
	double pi_;

	EdgeWithPairedInfo(size_t e_, double pi) :
			e_(e_), pi_(pi) {

	}
};

struct EdgeWithDistance {
	EdgeId e_;
	int d_;

	EdgeWithDistance(EdgeId e, size_t d) :
			e_(e), d_((int) d) {
	}
};

class ExtentionAnalyzer {

protected:
	const Graph& g_;
	PairedInfoLibrary& lib_;

public:
	ExtentionAnalyzer(const Graph& g, PairedInfoLibrary& l) :
			g_(g), lib_(l) {
	}

	PairedInfoLibrary& getLib() {
		return lib_;
	}

	void FindCoveredEdges(const BidirectionalPath& path, EdgeId candidate,
			std::vector<EdgeWithPairedInfo>& edges) {
		edges.clear();

		for (int i = (int) path.Size() - 1; i >= 0; --i) {
			double w = lib_.IdealPairedInfo(path[i], candidate,
					(int) path.LengthAt(i));
			if (math::gr(w, 0.)) {
			}
				edges.push_back(EdgeWithPairedInfo(i, w));
		}
	}

	void FindForwardEdges(const BidirectionalPath& /*path*/, EdgeId candidate,
			std::vector<EdgeWithDistance>& edges) {
		edges.clear();
		edges.push_back(EdgeWithDistance(candidate, 0));

		size_t i = 0;
		while (i < edges.size()) {
			size_t currentDistance = edges[i].d_ + g_.length(edges[i].e_);
			auto nextEdges = g_.OutgoingEdges(g_.EdgeEnd(edges[i].e_));

			if (edges[i].d_ < (int) lib_.insert_size_) {
				for (auto edge = nextEdges.begin(); edge != nextEdges.end();
						++edge) {
					edges.push_back(EdgeWithDistance(*edge, currentDistance));
				}
			}
			++i;
		}
	}
};

class WeightCounter {

protected:
	const Graph& g_;
	PairedInfoLibraries& libs_;
	std::vector<ExtentionAnalyzer *> analyzers_;
	double avrageLibWeight_;

	double threshold_;
	bool normalizeWeight_;

	std::map<size_t, double> excluded_edges_;

public:

	WeightCounter(const Graph& g, PairedInfoLibraries& libs, double threshold = 0.0) :
			g_(g), libs_(libs), threshold_(threshold), normalizeWeight_(true), excluded_edges_() {
		avrageLibWeight_ = 0.0;
		analyzers_.reserve(libs_.size());
		for (auto iter = libs_.begin(); iter != libs_.end(); ++iter) {
			analyzers_.push_back(new ExtentionAnalyzer(g_, **iter));
			avrageLibWeight_ += (*iter)->coverage_coeff_;
		}
		avrageLibWeight_ /= (double) max(libs_.size(), (size_t) 1);
	}

	virtual ~WeightCounter() {
		for (auto iter = analyzers_.begin(); iter != analyzers_.end(); ++iter) {
			delete *iter;
		}
		analyzers_.clear();
	}

	virtual bool PairInfoExist(EdgeId first, EdgeId second, int distance) = 0;

	virtual double CountWeight(BidirectionalPath& path, EdgeId e,
			int gapLength = 0) = 0;

	virtual void GetDistances(EdgeId e1, EdgeId e2, std::vector<int>& dist,
			std::vector<double>& w) = 0;

	virtual double CountIdealInfo(EdgeId e1, EdgeId e2, size_t dist) = 0;

	virtual double CountIdealInfo(const BidirectionalPath& p, EdgeId e,
			size_t gap) = 0;

	virtual bool IsExtensionPossible(BidirectionalPath& path, EdgeId e) {
		return IsExtensionPossible(CountWeight(path, e)) ? true : false;
	}

	virtual bool IsExtensionPossible(double weight) const {
		return math::ge(weight, threshold_) ? true : false;
	}

	std::map<size_t, double>& GetExcludedEdges() {
		return excluded_edges_;
	}

	double getThreshold() const {
		return threshold_;
	}

	bool isNormalizeWeight() const {
		return normalizeWeight_;
	}

	void setNormalizeWeight(bool normalizeWeight) {
		this->normalizeWeight_ = normalizeWeight;
	}

	void setThreshold(double threshold) {
		this->threshold_ = threshold;
	}

	PairedInfoLibraries& getLibs() {
		return libs_;
	}

};

class ReadCountWeightCounter: public WeightCounter {

protected:

	double CountSingleLib(int libIndex, BidirectionalPath& path, EdgeId e,
			int additionalGapLength = 0.0) {

		double weight = 0.0;

		std::vector<EdgeWithPairedInfo> coveredEdges;
		analyzers_[libIndex]->FindCoveredEdges(path, e, coveredEdges);

		for (auto iter = coveredEdges.begin(); iter != coveredEdges.end();
				++iter) {
			if (excluded_edges_.find((int) iter->e_) != excluded_edges_.end()) {
				DEBUG("excluded " << iter->e_)
				continue;
			}
			double w = libs_[libIndex]->CountPairedInfo(path[iter->e_], e,
					(int) path.LengthAt(iter->e_) + additionalGapLength);

			if (normalizeWeight_) {
				w /= iter->pi_;
			}
			weight += w;

		}

		return weight;
	}

public:

	ReadCountWeightCounter(Graph& g_, PairedInfoLibraries& libs_,
			double threshold_ = 0.0) :
			WeightCounter(g_, libs_, threshold_) {
	}

	virtual void GetDistances(EdgeId e1, EdgeId e2, std::vector<int>& dist,
			std::vector<double>& w) {
		for (size_t i = 0; i < libs_.size(); ++i) {
			libs_[i]->CountDistances(e1, e2, dist, w);
		}
	}

	virtual double CountIdealInfo(EdgeId e1, EdgeId e2, size_t dist) {
		double res = 0.0;
		for (size_t i = 0; i < libs_.size(); ++i) {
			res += libs_[i]->IdealPairedInfo(e1, e2, (int) dist);
		}
		return res;
	}

	virtual double CountIdealInfo(const BidirectionalPath& p, EdgeId e,
			size_t gap) {
		double w = 0.0;
		for (int i = (int) p.Size() - 1; i >= 0; --i) {
			w += CountIdealInfo(p[i], e, gap + p.LengthAt(i));
		}
		return w;
	}

	virtual double CountWeight(BidirectionalPath& path, EdgeId e,
			int gapLength = 0) {
		double weight = 0.0;
		std::vector<EdgeWithDistance> edges;

		for (size_t i = 0; i < libs_.size(); ++i) {
			weight += CountSingleLib((int) i, path, e, gapLength);
		}

		return weight;
	}

	virtual bool PairInfoExist(EdgeId first, EdgeId second, int distance) {
		for (size_t libIndex = 0; libIndex < libs_.size(); ++libIndex) {
			double w = libs_[libIndex]->CountPairedInfo(first, second,
					distance);
			double w_ideal = libs_[libIndex]->IdealPairedInfo(first, second,
					distance);
			if (w_ideal == 0) {
				continue;
			}
			if (normalizeWeight_) {
				w /= w_ideal;
			}
			if (w > 0) {
				return true;
			}
		}
		return false;
	}

};

class PathCoverWeightCounter: public WeightCounter {

protected:

	double singleThreshold;

	double CountSingleLib(int libIndex, BidirectionalPath& path, EdgeId e,
			int additionalGapLength = 0.0) {
		PairedInfoLibrary& pairedInfoLibrary = *libs_[libIndex];
		double weight = 0.0;
		double idealWeight = 0.0;

		std::vector<EdgeWithPairedInfo> coveredEdges;
		analyzers_[libIndex]->FindCoveredEdges(path, e, coveredEdges);
		for (auto iter = coveredEdges.begin(); iter != coveredEdges.end();
				++iter) {
			double ideal_weight = iter->pi_;
			if (excluded_edges_.find(iter->e_) != excluded_edges_.end()) {
				if (math::eq(excluded_edges_[iter->e_], 0.0)) {
					continue;
				} else {
					ideal_weight = excluded_edges_[iter->e_];
				}
			}
			double threshold =
					pairedInfoLibrary.single_threshold_ >= 0.0 ?
							pairedInfoLibrary.single_threshold_ :
							singleThreshold;
			double singleWeight = libs_[libIndex]->CountPairedInfo(
					path[iter->e_], e,
					(int) path.LengthAt(iter->e_) + additionalGapLength);

			if (normalizeWeight_) {
				singleWeight /= ideal_weight;
			}
			if (math::ge(singleWeight, threshold)) {
				weight += ideal_weight;
			}
			idealWeight += ideal_weight;
		}

		return math::gr(idealWeight, 0.0) ? weight / idealWeight : 0.0;
	}

public:

	PathCoverWeightCounter(const Graph& g_, PairedInfoLibraries& libs_,
			double threshold_ = 0.0, double singleThreshold_ = 0.0) :
			WeightCounter(g_, libs_, threshold_), singleThreshold(
					singleThreshold_) {

	}

	virtual void GetDistances(EdgeId e1, EdgeId e2, std::vector<int>& dist,
			std::vector<double>& w) {
		for (size_t i = 0; i < libs_.size(); ++i) {
			libs_[i]->CountDistances(e1, e2, dist, w);
		}
	}

	virtual double CountIdealInfo(EdgeId e1, EdgeId e2, size_t dist) {
		double res = 0.0;
		for (size_t i = 0; i < libs_.size(); ++i) {
			res += libs_[i]->IdealPairedInfo(e1, e2, (int) dist);
		}
		return res;
	}

	virtual double CountIdealInfo(const BidirectionalPath& p, EdgeId e,
			size_t gap) {
		double w = 0.0;
		for (int i = (int) p.Size() - 1; i >= 0; --i) {
			w += g_.length(p[i]) ?
					CountIdealInfo(p[i], e, gap + p.LengthAt(i)) > 0 : 0;
		}
		return w;
	}

	virtual double CountWeight(BidirectionalPath& path, EdgeId e,
			int gapLength = 0) {
		double weight = 0.0;
		for (size_t i = 0; i < libs_.size(); ++i) {
			weight += CountSingleLib((int) i, path, e, gapLength);
		}

		return weight / (double) max(libs_.size(), (size_t) 1);
	}

	virtual bool PairInfoExist(EdgeId first, EdgeId second, int distance) {
		for (size_t libIndex = 0; libIndex < libs_.size(); ++libIndex) {
			double w = libs_[libIndex]->CountPairedInfo(first, second,
					distance);
			double w_ideal = libs_[libIndex]->IdealPairedInfo(first, second,
					distance);
			if (w_ideal == 0.0) {
				continue;
			}
			if (normalizeWeight_) {
				w /= w_ideal;
			}
			double threshold =
					libs_[libIndex]->single_threshold_ >= 0.0 ?
							libs_[libIndex]->single_threshold_ :
							singleThreshold;
			DEBUG("pair info exsist " <<g_.int_id(first) << "  " << g_.int_id(second) << " " << w << " " << distance << " " << threshold << " " << w_ideal);
			if (w > threshold) {
				return true;
			}
		}
		return false;
	}

};

}

#endif /* WEIGHT_COUNTER_HPP_ */
