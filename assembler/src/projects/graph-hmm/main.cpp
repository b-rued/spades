//***************************************************************************
//* Copyright (c) 2018 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "assembly_graph/core/graph.hpp"
#include "assembly_graph/dijkstra/dijkstra_helper.hpp"
#include "assembly_graph/components/graph_component.hpp"

#include "visualization/visualization.hpp"
#include "pipeline/graphio.hpp"
#include "utils/logger/log_writers.hpp"
#include "utils/segfault_handler.hpp"
#include "io/graph/gfa_reader.hpp"
#include "io/reads/io_helper.hpp"
#include "io/reads/osequencestream.hpp"

#include "version.hpp"

#include "hmmfile.hpp"
#include "hmmmatcher.hpp"
#include "fees.hpp"
#include "omnigraph_wrapper.hpp"

#include <clipp/clipp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string>

#include "aa.hpp"

void create_console_logger() {
    using namespace logging;

    logger *lg = create_logger("");
    lg->add_writer(std::make_shared<console_writer>());
    attach_logger(lg);
}

struct cfg {
    std::string load_from;
    std::string hmmfile;
    size_t k;
    size_t top;
    uint64_t int_id;
    unsigned min_size;
    unsigned max_size;
    bool debug;
    bool draw;
    bool save;
    bool rescore;

    hmmer::hmmer_cfg hcfg;
    cfg()
            : load_from(""), hmmfile(""), k(0), top(10),
              int_id(0), min_size(2), max_size(1000),
              debug(false), draw(false),
              save(true), rescore(true)
    {}
};

extern "C" {
#include "p7_config.h"

#include <stdlib.h>
#include <string.h>

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_sq.h"
#include "esl_stopwatch.h"

#include "hmmer.h"
}

void process_cmdline(int argc, char **argv, cfg &cfg) {
  using namespace clipp;

  auto cli = (
      cfg.hmmfile    << value("hmm file"),
      cfg.load_from  << value("load from"),
      cfg.k          << integer("k-mer size"),
      (option("--top") & integer("x", cfg.top)) % "extract top x paths",
      (option("--edge_id") & integer("value", cfg.int_id)) % "match around edge",
      (option("--min_size") & integer("value", cfg.min_size)) % "minimal component size to consider (default: 2)",
      (option("--max_size") & integer("value", cfg.max_size)) % "maximal component size to consider (default: 1000)",
      // Control of output
      cfg.hcfg.acc     << option("--acc")          % "prefer accessions over names in output",
      cfg.hcfg.noali   << option("--noali")        % "don't output alignments, so output is smaller",
      // Control of reporting thresholds
      (option("-E") & number("value", cfg.hcfg.E))        % "report sequences <= this E-value threshold in output",
      (option("-T") & number("value", cfg.hcfg.T))        % "report sequences >= this score threshold in output",
      (option("--domE") & number("value", cfg.hcfg.domE)) % "report domains <= this E-value threshold in output",
      (option("--domT") & number("value", cfg.hcfg.domT)) % "report domains >= this score cutoff in output",
      // Control of inclusion (significance) thresholds
      (option("-incE") & number("value", cfg.hcfg.incE))       % "consider sequences <= this E-value threshold as significant",
      (option("-incT") & number("value", cfg.hcfg.incT))       % "consider sequences >= this score threshold as significant",
      (option("-incdomE") & number("value", cfg.hcfg.incdomE)) % "consider domains <= this E-value threshold as significant",
      (option("-incdomT") & number("value", cfg.hcfg.incdomT)) % "consider domains >= this score threshold as significant",
      // Model-specific thresholding for both reporting and inclusion
      cfg.hcfg.cut_ga  << option("--cut_ga")       % "use profile's GA gathering cutoffs to set all thresholding",
      cfg.hcfg.cut_nc  << option("--cut_nc")       % "use profile's NC noise cutoffs to set all thresholding",
      cfg.hcfg.cut_tc  << option("--cut_tc")       % "use profile's TC trusted cutoffs to set all thresholding",
      // Control of acceleration pipeline
      cfg.hcfg.max     << option("--max")             % "Turn all heuristic filters off (less speed, more power)",
      (option("--F1") & number("value", cfg.hcfg.F1)) % "Stage 1 (MSV) threshold: promote hits w/ P <= F1",
      (option("--F2") & number("value", cfg.hcfg.F2)) % "Stage 2 (Vit) threshold: promote hits w/ P <= F2",
      (option("--F3") & number("value", cfg.hcfg.F3)) % "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",
      cfg.debug << option("--debug") % "enable extensive debug output",
      cfg.draw  << option("--draw")  % "draw pictures around the interesting edges",
      cfg.save << option("--save") % "save found sequences",
      cfg.rescore  << option("--rescore")  % "rescore paths via HMMer"

  );

  if (!parse(argc, argv, cli)) {
    std::cout << make_man_page(cli, argv[0]);
    exit(1);
  }
}

void DrawComponent(const omnigraph::GraphComponent<debruijn_graph::ConjugateDeBruijnGraph> &component,
                   const debruijn_graph::ConjugateDeBruijnGraph &graph,
                   const std::string &prefix,
                   const std::vector<debruijn_graph::EdgeId> &match_edges) {
    using namespace visualization;
    using namespace visualization::visualization_utils;
    using namespace debruijn_graph;

    // FIXME: This madness needs to be refactored
    graph_labeler::StrGraphLabeler<ConjugateDeBruijnGraph> tmp_labeler1(graph);
    graph_labeler::CoverageGraphLabeler<ConjugateDeBruijnGraph> tmp_labeler2(graph);
    graph_labeler::CompositeLabeler<ConjugateDeBruijnGraph> labeler{tmp_labeler1, tmp_labeler2};

    auto colorer = graph_colorer::DefaultColorer(graph);
    auto edge_colorer = std::make_shared<graph_colorer::CompositeEdgeColorer<ConjugateDeBruijnGraph>>("black");
    edge_colorer->AddColorer(colorer);
    edge_colorer->AddColorer(std::make_shared<graph_colorer::SetColorer<ConjugateDeBruijnGraph>>(graph, match_edges, "green"));
    std::shared_ptr<graph_colorer::GraphColorer<ConjugateDeBruijnGraph>>
            resulting_colorer = std::make_shared<graph_colorer::CompositeGraphColorer<Graph>>(colorer, edge_colorer);

    WriteComponent(component,
                   prefix + ".dot",
                   resulting_colorer,
                   labeler);
}

template<class GraphCursor>
std::vector<typename GraphCursor::EdgeId> to_path(const std::vector<GraphCursor> &cpath) {
    std::vector<typename GraphCursor::EdgeId> path;

    auto it = cpath.begin();
    for (; it != cpath.end() && it->is_empty(); ++it) {}

    for (; it != cpath.end(); ++it) {
        if (it->is_empty())
            continue;

        if (path.size() == 0 || it->edge() != path.back()) {
            path.push_back(it->edge());
        } else {
            for (auto e : it->edges())
                if (e != path.back())
                    path.push_back(e);
        }

    }

    return path;
}

using debruijn_graph::EdgeId;
using debruijn_graph::VertexId;
using debruijn_graph::ConjugateDeBruijnGraph;
using EdgeAlnInfo = std::unordered_map<EdgeId, std::pair<int, int>>;
EdgeAlnInfo MatchedEdges(const std::vector<EdgeId> &edges,
                         const ConjugateDeBruijnGraph &graph,
                         const hmmer::HMM &hmm, const cfg &cfg,
                         ESL_STOPWATCH *w) {
    bool hmm_in_aas = hmm.abc()->K == 20;
    hmmer::HMMMatcher matcher(hmm, cfg.hcfg);

    if (!hmm_in_aas) {
        INFO("HMM in nucleotides");
        for (size_t i = 0; i < edges.size(); ++i) {
            // FIXME: this conversion is pointless
            std::string ref = std::to_string(i);
            std::string seq = graph.EdgeNucls(edges[i]).str();
            matcher.match(ref.c_str(), seq.c_str());
        }
    } else {
        INFO("HMM in amino acids");
        for (size_t i = 0; i < edges.size(); ++i) {
            // FIXME: this conversion is pointless
            std::string ref = std::to_string(i);
            std::string seq = graph.EdgeNucls(edges[i]).str();
            for (size_t shift = 0; shift < 3; ++shift) {
                std::string ref_shift = ref + "_" + std::to_string(shift);
                std::string seq_aas = translate(seq.c_str() + shift);
                matcher.match(ref_shift.c_str(), seq_aas.c_str());
            }
        }
    }

    matcher.summarize();
    esl_stopwatch_Stop(w);

    EdgeAlnInfo match_edges;
    for (const auto &hit : matcher.hits()) {
        if (!hit.reported() || !hit.included())
            continue;

        EdgeId e = edges[std::stoull(hit.name())];
        if (cfg.debug)
            INFO("HMMER seq id:" <<  hit.name() << ", edge id:" << e);

        for (const auto &domain : hit.domains()) {
            // Calculate HMM overhang
            std::pair<int, int> seqpos = domain.seqpos();
            std::pair<int, int> hmmpos = domain.hmmpos();

            int roverhang = static_cast<int>(domain.M() - hmmpos.second) - static_cast<int>(domain.L() - seqpos.second);
            int loverhang = static_cast<int>(hmmpos.first) - static_cast<int>(seqpos.first);

            if (!match_edges.count(e)) {
                match_edges[e] = {loverhang, roverhang};
            } else {
                auto &entry = match_edges[e];
                if (entry.first < loverhang) {
                    entry.first = loverhang;
                }
                if (entry.second < roverhang) {
                    entry.second = roverhang;
                }
            }

            INFO("" << e << ":" << match_edges[e]);
        }
    }
    INFO("Total matched edges: " << match_edges.size());

    int textw = 120;
    if (match_edges.size() && cfg.debug) {
        p7_tophits_Targets(stderr, matcher.top_hits(), matcher.pipeline(), textw); if (fprintf(stderr, "\n\n") < 0) FATAL_ERROR("write failed");
        p7_tophits_Domains(stderr, matcher.top_hits(), matcher.pipeline(), textw); if (fprintf(stderr, "\n\n") < 0) FATAL_ERROR("write failed");
        p7_pli_Statistics(stderr, matcher.pipeline(), w); if (fprintf(stderr, "//\n") < 0) FATAL_ERROR("write failed");
    }

    return match_edges;
}

std::string PathToString(const std::vector<EdgeId>& path,
                         const ConjugateDeBruijnGraph &graph) {
    std::string res = "";
    for (auto e : path)
        res = res + graph.EdgeNucls(e).First(graph.length(e)).str();
    return res;
}

template<class Graph>
Sequence MergeSequences(const Graph &g,
                        const std::vector<typename Graph::EdgeId> &continuous_path) {
    std::vector<Sequence> path_sequences;
    path_sequences.push_back(g.EdgeNucls(continuous_path[0]));
    for (size_t i = 1; i < continuous_path.size(); ++i) {
        VERIFY(g.EdgeEnd(continuous_path[i - 1]) == g.EdgeStart(continuous_path[i]));
        path_sequences.push_back(g.EdgeNucls(continuous_path[i]));
    }
    return MergeOverlappingSequences(path_sequences, g.k());
}

static bool ends_with(const std::string &s, const std::string &p) {
    if (s.size() < p.size())
        return false;

    return (s.compare(s.size() - p.size(), p.size(), p) == 0);
}

std::unordered_map<EdgeId, std::unordered_set<VertexId>> ExtractNeighbourhoods(const EdgeAlnInfo &matched_edges,
                                                                               const ConjugateDeBruijnGraph &graph,
                                                                               int mult) {
    using namespace debruijn_graph;

    std::unordered_map<EdgeId, std::unordered_set<VertexId>> neighbourhoods;
    for (const auto &entry : matched_edges) {
        EdgeId e = entry.first;
        INFO("Extracting neighbourhood of edge " << e);

        std::pair<int, int> overhangs = entry.second;
        overhangs.first *= mult; overhangs.second *= mult;
        INFO("Dijkstra bounds set to " << overhangs);

        std::vector<VertexId> fvertices, bvertices;
        // If hmm overhangs from the edge, then run edge-bounded dijkstra to
        // extract the graph neighbourhood.
        if (overhangs.second > 0) {
            auto fdijkstra = omnigraph::CreateEdgeBoundedDijkstra(graph, overhangs.second);
            fdijkstra.Run(graph.EdgeEnd(e));
            fvertices = fdijkstra.ReachedVertices();
        }
        if (overhangs.first > 0) {
            auto bdijkstra = omnigraph::CreateBackwardEdgeBoundedDijkstra(graph, overhangs.first);
            bdijkstra.Run(graph.EdgeStart(e));
            bvertices = bdijkstra.ReachedVertices();
        }

        INFO("Total " << std::make_pair(bvertices.size(), fvertices.size()) << " extracted");

        neighbourhoods[e].insert(fvertices.begin(), fvertices.end());
        neighbourhoods[e].insert(bvertices.begin(), bvertices.end());
        neighbourhoods[e].insert(graph.EdgeEnd(e));
        neighbourhoods[e].insert(graph.EdgeStart(e));
    }

    return neighbourhoods;
}

int main(int argc, char* argv[]) {
    utils::segfault_handler sh;
    utils::perf_counter pc;

    srand(42);
    srandom(42);

    cfg cfg;
    process_cmdline(argc, argv, cfg);

    create_console_logger();
    INFO("Starting Graph HMM aligning engine, built from " SPADES_GIT_REFSPEC ", git revision " SPADES_GIT_SHA1);

    /* Open the query profile HMM file */
    hmmer::HMMFile hmmfile(cfg.hmmfile);
    if (!hmmfile.valid())
        FATAL_ERROR("Error opening HMM file "<< cfg.hmmfile);

    using namespace debruijn_graph;
    ConjugateDeBruijnGraph graph(cfg.k);
    if (ends_with(cfg.load_from, ".gfa")) {
        gfa::GFAReader gfa(cfg.load_from);
        INFO("GFA segments: " << gfa.num_edges() << ", links: " << gfa.num_links());
        gfa.to_graph(graph);
    } else {
        graphio::ScanBasicGraph(cfg.load_from, graph);
    }
    INFO("Graph loaded. Total vertices: " << graph.size());

    // Collect all the edges
    std::vector<EdgeId> edges;
    for (auto it = graph.ConstEdgeBegin(); !it.IsEnd(); ++it) {
        EdgeId edge = *it;
        if (cfg.int_id == 0 ||
            edge.int_id() == cfg.int_id)
        edges.push_back(edge);
    }

    auto hmmw = hmmfile.read();
    if (!hmmw) {
        FATAL_ERROR("Error reading HMM file "<< cfg.hmmfile);
    }

    ESL_STOPWATCH *w = esl_stopwatch_Create();

    // Outer loop: over each query HMM in <hmmfile>.
    std::unordered_set<std::vector<EdgeId>> to_rescore;
    while (hmmw) {
        const hmmer::HMM &hmm = hmmw.get();
        P7_HMM *p7hmm = hmmw->get();

        if (fprintf(stderr, "Query:       %s  [M=%d]\n", p7hmm->name, p7hmm->M) < 0) FATAL_ERROR("write failed");
        if (p7hmm->acc)  { if (fprintf(stderr, "Accession:   %s\n", p7hmm->acc)  < 0) FATAL_ERROR("write failed"); }
        if (p7hmm->desc) { if (fprintf(stderr, "Description: %s\n", p7hmm->desc) < 0) FATAL_ERROR("write failed"); }

        esl_stopwatch_Start(w);

        // Collect the neighbourhood of the matched edges
        auto matched_edges = MatchedEdges(edges, graph, hmm, cfg, w);
        bool hmm_in_aas = hmm.abc()->K == 20;
        std::unordered_map<EdgeId, std::unordered_set<VertexId>>
                neighbourhoods = ExtractNeighbourhoods(matched_edges,
                                                       graph,
                                                       (hmm_in_aas ? 6 : 2));

        // See, whether we could join some components
        INFO("Joining components")
        for (auto it = neighbourhoods.begin(); it != neighbourhoods.end(); ++it) {
            for (auto to_check = std::next(it); to_check != neighbourhoods.end(); ) {
                VertexId vstart = graph.EdgeStart(to_check->first), vend = graph.EdgeEnd(to_check->first);
                if (it->second.count(vstart) || it->second.count(vend)) {
                    it->second.insert(to_check->second.begin(), to_check->second.end());
                    to_check = neighbourhoods.erase(to_check);
                } else
                    ++to_check;
            }
        }
        INFO("Total unique neighbourhoods extracted " << neighbourhoods.size());

        struct PathInfo {
            EdgeId leader;
            unsigned priority;
            std::string seq;
            std::vector<EdgeId> path;

            PathInfo(EdgeId e, unsigned prio, std::string s, std::vector<EdgeId> p)
                    : leader(e), priority(prio), seq(std::move(s)), path(std::move(p)) {}
        };

        std::vector<PathInfo> results;
        std::vector<std::pair<double, std::string>> resultant_paths;
        auto fees = hmm::fees_from_hmm(p7hmm, hmmw->abc());

        auto run_search = [&](const auto &initial, EdgeId e, size_t top,
                              std::vector<PathInfo> &local_results) {
            auto result = find_best_path(fees, initial);

            INFO("Best score: " << result.best_score());
            INFO("Best of the best");
            INFO(result.best_path_string());
            INFO("Extracting top paths");
            auto top_paths = result.top_k(top);
            size_t idx = 0;
            for (const auto& kv : top_paths) {
                auto seq = top_paths.str(kv.first);
                local_results.emplace_back(e, idx++, seq, to_path(kv.first));
                resultant_paths.push_back({kv.second, seq});
            }
        };

        std::vector<EdgeId> match_edges;
        for (const auto &entry : matched_edges)
            match_edges.push_back(entry.first);

        for (const auto &kv : neighbourhoods) {
            EdgeId e = kv.first;

            INFO("Looking HMM path around " <<e);
            auto component = omnigraph::GraphComponent<ConjugateDeBruijnGraph>::FromVertices(graph,
                                                                                             kv.second.begin(), kv.second.end(),
                                                                                             true);
            INFO("Neighbourhood vertices: " << component.v_size() << ", edges: " << component.e_size());

            if (component.e_size()/2 < cfg.min_size) {
                INFO("Component is too small (" << component.e_size() / 2 << " vs " << cfg.min_size << "), skipping");
                // Special case: if the component has only a single edge, add it to results
                results.emplace_back(e, 0, std::string(), std::vector<EdgeId>(1, e));
                continue;
            }

            if (component.e_size()/2 > cfg.max_size) {
                WARN("Component is too large (" << component.e_size() / 2 << " vs " << cfg.max_size << "), skipping");
                continue;
            }

            if (cfg.draw) {
                INFO("Writing component around edge " << e);
                DrawComponent(component, graph, std::to_string(graph.int_id(e)), match_edges);
            }

            auto initial = all(component);

            INFO("Running path search");
            std::vector<PathInfo> local_results;
            if (hmm_in_aas) {
                run_search(make_aa_cursors(initial), e, cfg.top, local_results);
            } else {
                run_search(initial, e, cfg.top, local_results);
            }

            std::unordered_set<std::vector<EdgeId>> paths;
            for (const auto& entry : local_results)
                paths.insert(entry.path);
            results.insert(results.end(), local_results.begin(), local_results.end());

            INFO("Total " << paths.size() << " unique edge paths extracted");
            size_t idx = 0;
            for (const auto &path : paths) {
                INFO("Path length : " << path.size() << " edges");
                for (EdgeId e : path)
                    INFO("" << e.int_id());
                if (cfg.draw) {
                    INFO("Writing component around path");
                    DrawComponent(component, graph, std::to_string(graph.int_id(e)) + "_" + std::to_string(idx), path);
                }
                idx += 1;
            }
        }
        INFO("Total " << results.size() << " results extracted");
        INFO("Total " << resultant_paths.size() << " resultant paths extracted");
        std::sort(resultant_paths.begin(), resultant_paths.end());

        std::unordered_set<std::vector<EdgeId>> to_rescore_local;
        if (cfg.save) {
            if (resultant_paths.size()) {
                std::ofstream o(std::string("graph-hmm-") + p7hmm->name + ".paths.fa", std::ios::out);
                for (const auto &result : resultant_paths) {
                    o << ">Score_" << result.first << '\n';
                    io::WriteWrapped(result.second, o);
                }
            }

            if (results.size()) {
                std::ofstream o(std::string("graph-hmm-") + p7hmm->name + ".fa", std::ios::out);
                for (const auto &result : results) {
                    o << ">" << result.leader << "_" << result.priority;
                    if (result.seq.size() == 0)
                        o << " (whole edge)";
                    o << '\n';
                    if (result.seq.size() == 0) {
                        io::WriteWrapped(graph.EdgeNucls(result.leader).str(), o);
                    } else {
                        io::WriteWrapped(result.seq, o);
                    }
                    if (cfg.rescore && result.path.size() > 0) {
                        to_rescore_local.insert(result.path);
                        to_rescore.insert(result.path);
                    }
                }
            }
        }

        INFO("Total " << to_rescore_local.size() << " local paths to rescore");
        if (cfg.rescore && to_rescore_local.size()) {
            std::ofstream o(std::string("graph-hmm-") + p7hmm->name + ".edges.fa", std::ios::out);

            for (const auto &entry : to_rescore_local) {
                o << ">";
                for (size_t i = 0; i < entry.size(); ++i) {
                    o << entry[i];
                    if (i != entry.size() - 1)
                        o << "_";
                }
                o << '\n';
                io::WriteWrapped(MergeSequences(graph, entry).str(), o);
            }
        }

        hmmw = hmmfile.read();
    } // end outer loop over query HMMs

    INFO("Total " << to_rescore.size() << " paths to rescore");
    if (cfg.rescore && to_rescore.size()) {
        std::ofstream o(std::string("graph-hmm") + ".all.edges.fa", std::ios::out);

        for (const auto &entry : to_rescore) {
            o << ">";
            for (size_t i = 0; i < entry.size(); ++i) {
                o << entry[i];
                if (i != entry.size() - 1)
                    o << "_";
            }
            o << '\n';
            io::WriteWrapped(MergeSequences(graph, entry).str(), o);
        }
    }

    esl_stopwatch_Destroy(w);

    return 0;
}
