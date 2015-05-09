/*

Copyright (c) 2015, Project OSRM contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "extraction_containers.hpp"
#include "extraction_way.hpp"

#include "../data_structures/coordinate_calculation.hpp"
#include "../data_structures/node_id.hpp"
#include "../data_structures/range_table.hpp"

#include "../util/osrm_exception.hpp"
#include "../util/simple_logger.hpp"
#include "../util/timing_util.hpp"

#include <boost/assert.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <stxxl/sort>

#include <chrono>
#include <limits>

ExtractionContainers::ExtractionContainers()
{
    // Check if stxxl can be instantiated
    stxxl::vector<unsigned> dummy_vector;
    name_list.push_back("");
}

ExtractionContainers::~ExtractionContainers()
{
    // FIXME isn't this done implicitly of the stxxl::vectors go out of scope?
    used_node_id_list.clear();
    all_nodes_list.clear();
    all_edges_list.clear();
    name_list.clear();
    restrictions_list.clear();
    way_start_end_id_list.clear();
}

/**
 * Processes the collected data and serializes it.
 * At this point nodes are still referenced by their OSM id.
 *
 * - map start-end nodes of ways to ways used int restrictions to compute compressed
 *   trippe representation
 * - filter nodes list to nodes that are referenced by ways
 * - merge edges with nodes to include location of start/end points and serialize
 *
 */
void ExtractionContainers::PrepareData(const std::string &output_file_name,
                                       const std::string &restrictions_file_name,
                                       const std::string &name_file_name)
{
    try
    {

        PrepareRestrictions();
        WriteRestrictions(restrictions_file_name);

        std::ofstream file_out_stream;
        file_out_stream.open(output_file_name.c_str(), std::ios::binary);
        file_out_stream.write((char *)&fingerprint, sizeof(FingerPrint));

        PrepareNodes();
        WriteNodes(file_out_stream);
        PrepareEdges();
        WriteEdges(file_out_stream);

        file_out_stream.close();

        WriteNames(name_file_name);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Caught Execption:" << e.what() << std::endl;
    }
}

void ExtractionContainers::WriteNames(const std::string& names_file_name) const
{
    std::cout << "[extractor] writing street name index ... " << std::flush;
    TIMER_START(write_name_index);
    boost::filesystem::ofstream name_file_stream(names_file_name, std::ios::binary);

    unsigned total_length = 0;
    std::vector<unsigned> name_lengths;
    for (const std::string &temp_string : name_list)
    {
        const unsigned string_length =
            std::min(static_cast<unsigned>(temp_string.length()), 255u);
        name_lengths.push_back(string_length);
        total_length += string_length;
    }

    // builds and writes the index
    RangeTable<> name_index_range(name_lengths);
    name_file_stream << name_index_range;

    name_file_stream.write((char *)&total_length, sizeof(unsigned));
    // write all chars consecutively
    for (const std::string &temp_string : name_list)
    {
        const unsigned string_length =
            std::min(static_cast<unsigned>(temp_string.length()), 255u);
        name_file_stream.write(temp_string.c_str(), string_length);
    }

    name_file_stream.close();
    TIMER_STOP(write_name_index);
    std::cout << "ok, after " << TIMER_SEC(write_name_index) << "s" << std::endl;
}

void ExtractionContainers::PrepareNodes()
{
    std::cout << "[extractor] Sorting used nodes        ... " << std::flush;
    TIMER_START(sorting_used_nodes);
    stxxl::sort(used_node_id_list.begin(), used_node_id_list.end(), Cmp(), stxxl_memory);
    TIMER_STOP(sorting_used_nodes);
    std::cout << "ok, after " << TIMER_SEC(sorting_used_nodes) << "s" << std::endl;

    std::cout << "[extractor] Erasing duplicate nodes   ... " << std::flush;
    TIMER_START(erasing_dups);
    auto new_end = std::unique(used_node_id_list.begin(), used_node_id_list.end());
    used_node_id_list.resize(new_end - used_node_id_list.begin());
    TIMER_STOP(erasing_dups);
    std::cout << "ok, after " << TIMER_SEC(erasing_dups) << "s" << std::endl;

    std::cout << "[extractor] Sorting all nodes         ... " << std::flush;
    TIMER_START(sorting_nodes);
    stxxl::sort(all_nodes_list.begin(), all_nodes_list.end(), ExternalMemoryNodeSTXXLCompare(),
                stxxl_memory);
    TIMER_STOP(sorting_nodes);
    std::cout << "ok, after " << TIMER_SEC(sorting_nodes) << "s" << std::endl;
}

void ExtractionContainers::PrepareEdges()
{
    // Sort edges by start.
    std::cout << "[extractor] Sorting edges by start    ... " << std::flush;
    TIMER_START(sort_edges_by_start);
    stxxl::sort(all_edges_list.begin(), all_edges_list.end(), CmpEdgeByStartID(), stxxl_memory);
    TIMER_STOP(sort_edges_by_start);
    std::cout << "ok, after " << TIMER_SEC(sort_edges_by_start) << "s" << std::endl;

    std::cout << "[extractor] Setting start coords      ... " << std::flush;
    TIMER_START(set_start_coords);
    // Traverse list of edges and nodes in parallel and set start coord
    auto node_iterator = all_nodes_list.begin();
    auto edge_iterator = all_edges_list.begin();
    while (edge_iterator != all_edges_list.end() && node_iterator != all_nodes_list.end())
    {
        if (edge_iterator->result.source < node_iterator->node_id)
        {
            ++edge_iterator;
            continue;
        }
        if (edge_iterator->result.source > node_iterator->node_id)
        {
            node_iterator++;
            continue;
        }

        BOOST_ASSERT(edge_iterator->result.source == node_iterator->node_id);
        edge_iterator->source_coordinate.lat = node_iterator->lat;
        edge_iterator->source_coordinate.lon = node_iterator->lon;
        ++edge_iterator;
    }
    TIMER_STOP(set_start_coords);
    std::cout << "ok, after " << TIMER_SEC(set_start_coords) << "s" << std::endl;

    // Sort Edges by target
    std::cout << "[extractor] Sorting edges by target   ... " << std::flush;
    TIMER_START(sort_edges_by_target);
    stxxl::sort(all_edges_list.begin(), all_edges_list.end(), CmpEdgeByTargetID(),
                stxxl_memory);
    TIMER_STOP(sort_edges_by_target);
    std::cout << "ok, after " << TIMER_SEC(sort_edges_by_target) << "s" << std::endl;

    // Compute edge weights
    std::cout << "[extractor] Computing edge weights    ... " << std::flush;
    TIMER_START(compute_weights);
    node_iterator = all_nodes_list.begin();
    edge_iterator = all_edges_list.begin();
    while (edge_iterator != all_edges_list.end() && node_iterator != all_nodes_list.end())
    {
        if (edge_iterator->result.target < node_iterator->node_id)
        {
            // FIXME we are skipping edges here: That means the data is broken!
            ++edge_iterator;
            continue;
        }
        if (edge_iterator->result.target > node_iterator->node_id)
        {
            ++node_iterator;
            continue;
        }
        BOOST_ASSERT(edge_iterator->result.target == node_iterator->node_id);
        if (edge_iterator->source_coordinate.lat != std::numeric_limits<int>::min() &&
            edge_iterator->source_coordinate.lon != std::numeric_limits<int>::min())
        {
            BOOST_ASSERT(edge_iterator->weight_data.speed != -1);

            const double distance = coordinate_calculation::euclidean_distance(
                edge_iterator->source_coordinate.lat, edge_iterator->source_coordinate.lon,
                node_iterator->lat, node_iterator->lon);

            const double weight = [distance](const InternalExtractorEdge::WeightData& data) {
                switch (data.type)
                {
                    case InternalExtractorEdge::WeightType::EDGE_DURATION:
                    case InternalExtractorEdge::WeightType::WAY_DURATION:
                        return data.duration * 10.;
                        break;
                    case InternalExtractorEdge::WeightType::SPEED:
                        return (distance * 10.) / (data.speed / 3.6);
                        break;
                    default:
                        osrm::exception("invalid weight type");
                }
                return -1.0;
            }(edge_iterator->weight_data);
            // if duration was set for the way the speed value is duratoin / number_of_nodes ~ edge duration
            int integer_weight = std::max(1, (int)std::floor(weight + .5));
            edge_iterator->result.weight = integer_weight;
        }
        ++edge_iterator;
    }
    TIMER_STOP(compute_weights);
    std::cout << "ok, after " << TIMER_SEC(compute_weights) << "s" << std::endl;
}

void ExtractionContainers::WriteEdges(std::ofstream& file_out_stream) const
{
    std::cout << "[extractor] Writing used egdes       ... " << std::flush;
    TIMER_START(write_edges);
    // Traverse list of edges and nodes in parallel and set target coord
    unsigned number_of_used_edges = 0;

    auto start_position = file_out_stream.tellp();
    file_out_stream.write((char *)&number_of_used_edges, sizeof(unsigned));

    for (const auto& edge : all_edges_list)
    {
        if (edge.result.weight > 0)
        {
            file_out_stream.write((char*) &edge.result, sizeof(NodeBasedEdge));
            number_of_used_edges++;
        }
    }
    TIMER_STOP(write_edges);
    std::cout << "ok, after " << TIMER_SEC(write_edges) << "s" << std::endl;

    std::cout << "[extractor] setting number of edges   ... " << std::flush;
    file_out_stream.seekp(start_position);
    file_out_stream.write((char *)&number_of_used_edges, sizeof(unsigned));
    std::cout << "ok" << std::endl;

    SimpleLogger().Write() << "Processed " << number_of_used_edges << " nodes";
}

void ExtractionContainers::WriteNodes(std::ofstream& file_out_stream) const
{
    unsigned number_of_used_nodes = 0;
    // write dummy value, will be overwritten later
    file_out_stream.write((char *)&number_of_used_nodes, sizeof(unsigned));
    std::cout << "[extractor] Confirming/Writing used nodes     ... " << std::flush;
    TIMER_START(write_nodes);
    // identify all used nodes by a merging step of two sorted lists
    auto node_iterator = all_nodes_list.begin();
    auto node_id_iterator = used_node_id_list.begin();
    while (node_id_iterator != used_node_id_list.end() && node_iterator != all_nodes_list.end())
    {
        if (*node_id_iterator < node_iterator->node_id)
        {
            ++node_id_iterator;
            continue;
        }
        if (*node_id_iterator > node_iterator->node_id)
        {
            ++node_iterator;
            continue;
        }
        BOOST_ASSERT(*node_id_iterator == node_iterator->node_id);

        file_out_stream.write((char *)&(*node_iterator), sizeof(ExternalMemoryNode));

        ++number_of_used_nodes;
        ++node_id_iterator;
        ++node_iterator;
    }
    TIMER_STOP(write_nodes);
    std::cout << "ok, after " << TIMER_SEC(write_nodes) << "s" << std::endl;

    std::cout << "[extractor] setting number of nodes   ... " << std::flush;
    std::ios::pos_type previous_file_position = file_out_stream.tellp();
    file_out_stream.seekp(std::ios::beg + sizeof(FingerPrint));
    file_out_stream.write((char *)&number_of_used_nodes, sizeof(unsigned));
    file_out_stream.seekp(previous_file_position);
    std::cout << "ok" << std::endl;

    SimpleLogger().Write() << "Processed " << number_of_used_nodes << " nodes";
}

void ExtractionContainers::WriteRestrictions(const std::string& path) const
{
    // serialize restrictions
    std::ofstream restrictions_out_stream;
    unsigned written_restriction_count = 0;
    restrictions_out_stream.open(path.c_str(), std::ios::binary);
    restrictions_out_stream.write((char *)&fingerprint, sizeof(FingerPrint));
    const auto count_position = restrictions_out_stream.tellp();
    restrictions_out_stream.write((char *)&written_restriction_count, sizeof(unsigned));

    for (const auto &restriction_container : restrictions_list)
    {
        if (SPECIAL_NODEID != restriction_container.restriction.from.node &&
            SPECIAL_NODEID != restriction_container.restriction.to.node)
        {
            restrictions_out_stream.write((char *)&(restriction_container.restriction),
                                          sizeof(TurnRestriction));
            ++written_restriction_count;
        }
    }
    restrictions_out_stream.seekp(count_position);
    restrictions_out_stream.write((char *)&written_restriction_count, sizeof(unsigned));
    restrictions_out_stream.close();
    SimpleLogger().Write() << "usable restrictions: " << written_restriction_count;
}

void ExtractionContainers::PrepareRestrictions()
{
    std::cout << "[extractor] Sorting used ways         ... " << std::flush;
    TIMER_START(sort_ways);
    stxxl::sort(way_start_end_id_list.begin(), way_start_end_id_list.end(),
                FirstAndLastSegmentOfWayStxxlCompare(), stxxl_memory);
    TIMER_STOP(sort_ways);
    std::cout << "ok, after " << TIMER_SEC(sort_ways) << "s" << std::endl;

    std::cout << "[extractor] Sorting " << restrictions_list.size()
              << " restrictions. by from... " << std::flush;
    TIMER_START(sort_restrictions);
    stxxl::sort(restrictions_list.begin(), restrictions_list.end(),
                CmpRestrictionContainerByFrom(), stxxl_memory);
    TIMER_STOP(sort_restrictions);
    std::cout << "ok, after " << TIMER_SEC(sort_restrictions) << "s" << std::endl;

    std::cout << "[extractor] Fixing restriction starts ... " << std::flush;
    TIMER_START(fix_restriction_starts);
    auto restrictions_iterator = restrictions_list.begin();
    auto way_start_and_end_iterator = way_start_end_id_list.cbegin();

    while (way_start_and_end_iterator != way_start_end_id_list.cend() &&
           restrictions_iterator != restrictions_list.end())
    {
        if (way_start_and_end_iterator->way_id < restrictions_iterator->restriction.from.way)
        {
            ++way_start_and_end_iterator;
            continue;
        }

        if (way_start_and_end_iterator->way_id > restrictions_iterator->restriction.from.way)
        {
            ++restrictions_iterator;
            continue;
        }

        BOOST_ASSERT(way_start_and_end_iterator->way_id ==
                     restrictions_iterator->restriction.from.way);
        const NodeID via_node_id = restrictions_iterator->restriction.via.node;

        if (way_start_and_end_iterator->first_segment_source_id == via_node_id)
        {
            restrictions_iterator->restriction.from.node =
                way_start_and_end_iterator->first_segment_target_id;
        }
        else if (way_start_and_end_iterator->last_segment_target_id == via_node_id)
        {
            restrictions_iterator->restriction.from.node =
                way_start_and_end_iterator->last_segment_source_id;
        }
        ++restrictions_iterator;
    }

    TIMER_STOP(fix_restriction_starts);
    std::cout << "ok, after " << TIMER_SEC(fix_restriction_starts) << "s" << std::endl;

    std::cout << "[extractor] Sorting restrictions. by to  ... " << std::flush;
    TIMER_START(sort_restrictions_to);
    stxxl::sort(restrictions_list.begin(), restrictions_list.end(),
                CmpRestrictionContainerByTo(), stxxl_memory);
    TIMER_STOP(sort_restrictions_to);
    std::cout << "ok, after " << TIMER_SEC(sort_restrictions_to) << "s" << std::endl;

    std::cout << "[extractor] Fixing restriction ends   ... " << std::flush;
    TIMER_START(fix_restriction_ends);
    restrictions_iterator = restrictions_list.begin();
    way_start_and_end_iterator = way_start_end_id_list.cbegin();
    while (way_start_and_end_iterator != way_start_end_id_list.cend() &&
           restrictions_iterator != restrictions_list.end())
    {
        if (way_start_and_end_iterator->way_id < restrictions_iterator->restriction.to.way)
        {
            ++way_start_and_end_iterator;
            continue;
        }
        if (way_start_and_end_iterator->way_id > restrictions_iterator->restriction.to.way)
        {
            ++restrictions_iterator;
            continue;
        }
        BOOST_ASSERT(way_start_and_end_iterator->way_id ==
                     restrictions_iterator->restriction.to.way);
        const NodeID via_node_id = restrictions_iterator->restriction.via.node;

        if (way_start_and_end_iterator->first_segment_source_id == via_node_id)
        {
            restrictions_iterator->restriction.to.node =
                way_start_and_end_iterator->first_segment_target_id;
        }
        else if (way_start_and_end_iterator->last_segment_target_id == via_node_id)
        {
            restrictions_iterator->restriction.to.node =
                way_start_and_end_iterator->last_segment_source_id;
        }
        ++restrictions_iterator;
    }
    TIMER_STOP(fix_restriction_ends);
    std::cout << "ok, after " << TIMER_SEC(fix_restriction_ends) << "s" << std::endl;
}
