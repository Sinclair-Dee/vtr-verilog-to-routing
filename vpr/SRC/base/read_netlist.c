/**
 * Author: Jason Luu
 * Date: May 2009
 * 
 * Read a circuit netlist in XML format and populate the netlist data structures for VPR
 */

#include <cstdio>
#include <cstring>
using namespace std;

#include "pugixml.hpp"
#include "pugixml_loc.hpp"
#include "pugixml_util.hpp"

#include "vtr_assert.h"
#include "vtr_util.h"
#include "vtr_log.h"

#include "vpr_types.h"
#include "vpr_error.h"
#include "vpr_utils.h"

#include "hash.h"
#include "globals.h"
#include "atom_netlist.h"
#include "read_xml_util.h"
#include "read_netlist.h"
#include "pb_type_graph.h"
#include "token.h"
#include "netlist.h"

static const char* netlist_file_name = NULL;

static void processPorts(pugi::xml_node Parent, t_pb* pb, t_pb_route *pb_route,		
        const pugiutil::loc_data& loc_data);

static void processPb(pugi::xml_node Parent, t_block *cb, const int index,
		t_pb* pb, t_pb_route *pb_route, int *num_primitives,
        const pugiutil::loc_data& loc_data);

static void processComplexBlock(pugi::xml_node Parent, t_block *cb,
		const int index, int *num_primitives,
        const pugiutil::loc_data& loc_data);

static struct s_net *alloc_and_init_netlist_from_hash(const int ncount,
		struct s_hash **nhash);

static int add_net_to_hash(struct s_hash **nhash, const char *net_name,
		int *ncount);

static void load_external_nets_and_cb(const int L_num_blocks,
		const struct s_block block_list[],
		int *ext_ncount,
		struct s_net **ext_nets, const std::vector<std::string>& circuit_clocks);

static void load_interal_to_block_net_nums(const t_type_ptr type, t_pb_route *pb_route);

static void load_atom_index_for_pb_pin(t_pb_route *pb_route, int ipin);

static void mark_constant_generators(const int L_num_blocks,
		const struct s_block block_list[]);

static void mark_constant_generators_rec(const t_pb *pb, const t_pb_route *pb_route);

static t_pb_route *alloc_pb_route(t_pb_graph_node *pb_graph_node);

/**
 * Initializes the block_list with info from a netlist 
 * net_file - Name of the netlist file to read
 * num_blocks - number of CLBs in netlist 
 * block_list - array of blocks in netlist [0..num_blocks - 1]
 * num_nets - number of nets in netlist
 * net_list - nets in netlist [0..num_nets - 1]
 */
void read_netlist(const char *net_file, const t_arch* /*arch*/,
		int *L_num_blocks, struct s_block *block_list[],
		int *L_num_nets, struct s_net *net_list[]) {
	clock_t begin = clock();
	int i;
	int bcount = 0;
	struct s_block *blist;
	int ext_ncount;
	struct s_net *ext_nlist;
    std::vector<std::string> circuit_inputs, circuit_outputs, circuit_clocks;

	int num_primitives = 0;

	/* Parse the file */
	vtr::printf_info("Begin loading packed FPGA netlist file.\n");

    pugi::xml_document doc;
    pugiutil::loc_data loc_data;
    try {
        loc_data = pugiutil::load_xml(doc, net_file);
    } catch(pugiutil::XmlError& e) {
        vpr_throw(VPR_ERROR_NET_F, net_file, 0,
                  "Failed to load netlist file '%s' (%s).\n", net_file, e.what());
    }


    try {
        /* Save netlist file's name in file-scoped variable */
        netlist_file_name = net_file;

        /* Root node should be block */
        auto top = doc.child("block");
        if(!top) {
            vpr_throw(VPR_ERROR_NET_F, net_file, loc_data.line(top),
                      "Root element must be 'block'.\n");
        }

        /* Check top-level netlist attributes */
        auto top_name = top.attribute("name");
        if(!top_name) {
            vpr_throw(VPR_ERROR_NET_F, net_file, loc_data.line(top),
                      "Root element must have a 'name' attribute.\n");
        }

        vtr::printf_info("Netlist generated from file '%s'.\n", top_name.value());

        //Verify top level attributes
        auto top_instance = pugiutil::get_attribute(top, "instance", loc_data);

        if(strcmp(top_instance.value(), "FPGA_packed_netlist[0]") != 0) {
            vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(top),
                    "Expected top instance to be \"FPGA_packed_netlist[0]\", found \"%s\".",
                    top_instance.value());
        }

        //Collect top level I/Os
        auto top_inputs = pugiutil::get_single_child(top, "inputs", loc_data);
        circuit_inputs = vtr::split(top_inputs.text().get());

        auto top_outputs = pugiutil::get_single_child(top, "outputs", loc_data);
        circuit_outputs = vtr::split(top_outputs.text().get());

        auto top_clocks = pugiutil::get_single_child(top, "clocks", loc_data);
        circuit_clocks = vtr::split(top_clocks.text().get());

        /* Parse all CLB blocks and all nets*/

        //Reset atom/pb mapping (it is reloaded from the packed netlist file)
        for(auto blk_id : g_atom_nl.blocks()) {
            g_atom_map.set_atom_pb(blk_id, NULL);
        }

        //Count the number of blocks for allocation
        bcount = pugiutil::count_children(top, "block", loc_data, pugiutil::ReqOpt::OPTIONAL);
        if(bcount == 0) {
            vtr::printf_warning(__FILE__, __LINE__, "Packed netlist contains no clustered blocks\n");
        }

        blist = (struct s_block *) vtr::calloc(bcount, sizeof(t_block));

        /* Process netlist */

        i = 0;
        for(auto curr_block = top.child("block"); curr_block; curr_block = curr_block.next_sibling("block")) {
            processComplexBlock(curr_block, blist, i, &num_primitives, loc_data);
            i++;
        }
        VTR_ASSERT(i == bcount);
        VTR_ASSERT(num_primitives >= 0);
        VTR_ASSERT(static_cast<size_t>(num_primitives) == g_atom_nl.blocks().size());

        /* Error check */
        for(auto blk_id : g_atom_nl.blocks()) {
            if (g_atom_map.atom_pb(blk_id) == NULL) {
                vpr_throw(VPR_ERROR_NET_F, __FILE__, __LINE__,
                        ".blif file and .net file do not match, .net file missing atom %s.\n",
                        g_atom_nl.block_name(blk_id).c_str());
            }
        }
        /* TODO: Add additional check to make sure net connections match */

        mark_constant_generators(bcount, blist);
        load_external_nets_and_cb(bcount, blist, &ext_ncount, &ext_nlist, circuit_clocks);
    } catch(pugiutil::XmlError& e) {
        vpr_throw(VPR_ERROR_NET_F, e.filename_c_str(), e.line(),
                  "Error loading post-pack netlist (%s)", e.what());
    }

	/* TODO: create this function later
	 check_top_IO_matches_IO_blocks(circuit_inputs, circuit_outputs, circuit_clocks, blist, bcount);
	 */

	/* load mapping between external nets and all nets */
    for(auto net_id : g_atom_nl.nets()) {
        g_atom_map.set_atom_clb_net(net_id, OPEN);
	}

	for (i = 0; i < ext_ncount; i++) {
        AtomNetId net_id = g_atom_nl.find_net(ext_nlist[i].name);
        VTR_ASSERT(net_id);
        g_atom_map.set_atom_clb_net(net_id, i);
	}

	/* Return blocks and nets */
	*L_num_blocks = bcount;
	*block_list = blist;
	*L_num_nets = ext_ncount;
	*net_list = ext_nlist;

	//Added August 2013, Daniel Chen for loading post-pack netlist into new data structures
	load_global_net_from_array(ext_nlist, ext_ncount, &g_clbs_nlist);
	//echo_global_nlist_net(&g_clbs_nlist, ext_nlist);

	clock_t end = clock();

	vtr::printf_info("Finished loading packed FPGA netlist file (took %g seconds).\n", (float)(end - begin) / CLOCKS_PER_SEC);
}

/**
 * XML parser to populate CLB info and to update nets with the nets of this CLB 
 * Parent - XML tag for this CLB
 * clb - Array of CLBs in the netlist
 * index - index of the CLB to allocate and load information into
 * loc_data - xml location info for error reporting
 */
static void processComplexBlock(pugi::xml_node clb_block, t_block *cb,
		const int index, int *num_primitives,
        const pugiutil::loc_data& loc_data) {

	bool found;
	int num_tokens = 0;
	t_token *tokens;
	int i;
	const t_pb_type * pb_type = NULL;

	/* parse cb attributes */
	cb[index].pb = (t_pb*) vtr::calloc(1, sizeof(t_pb));

    auto block_name = pugiutil::get_attribute(clb_block, "name", loc_data);
	cb[index].name = vtr::strdup(block_name.value());
	cb[index].pb->name = vtr::strdup(block_name.value());


    auto block_inst = pugiutil::get_attribute(clb_block, "instance", loc_data);
	tokens = GetTokensFromString(block_inst.value(), &num_tokens);
	if (num_tokens != 4 || tokens[0].type != TOKEN_STRING
			|| tokens[1].type != TOKEN_OPEN_SQUARE_BRACKET
			|| tokens[2].type != TOKEN_INT
			|| tokens[3].type != TOKEN_CLOSE_SQUARE_BRACKET) {

		vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(clb_block),
				"Unknown syntax for instance %s in %s. Expected pb_type[instance_number].\n",
				block_inst.value(), clb_block.name());
	}
	VTR_ASSERT(vtr::atoi(tokens[2].data) == index);

	found = false;
	for (i = 0; i < num_types; i++) {
		if (strcmp(type_descriptors[i].name, tokens[0].data) == 0) {
			cb[index].type = &type_descriptors[i];
			pb_type = cb[index].type->pb_type;
			found = true;
			break;
		}
	}
	if (!found) {
		vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(clb_block),
				"Unknown cb type %s for cb %s #%d.\n", block_inst.value(), cb[index].name,
				index);
	}

	/* Parse all pbs and CB internal nets*/
    g_atom_map.set_atom_pb(AtomBlockId::INVALID(), cb[index].pb);

	cb[index].pb->pb_graph_node = cb[index].type->pb_graph_head;
	cb[index].pb_route = alloc_pb_route(cb[index].pb->pb_graph_node);
	
    auto clb_mode = pugiutil::get_attribute(clb_block, "mode", loc_data);

	found = false;
	for (i = 0; i < pb_type->num_modes; i++) {
		if (strcmp(clb_mode.value(), pb_type->modes[i].name) == 0) {
			cb[index].pb->mode = i;
			found = true;
		}
	}
	if (!found) {
		vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(clb_block),
				"Unknown mode %s for cb %s #%d.\n", clb_mode.value(), cb[index].name,
				index);
	}

	processPb(clb_block, cb, index, cb[index].pb, cb[index].pb_route, num_primitives, loc_data);

	cb[index].nets = (int *) vtr::malloc(cb[index].type->num_pins * sizeof(int));
	for (i = 0; i < cb[index].type->num_pins; i++) {
		cb[index].nets[i] = OPEN;
	}
	load_interal_to_block_net_nums(cb[index].type, cb[index].pb_route);
	freeTokens(tokens, num_tokens);
}

/**
 * XML parser to populate pb info and to update internal nets of the parent CLB
 * Parent - XML tag for this pb_type
 * pb - physical block to use
 * loc_data - xml location info for error reporting
 */
static void processPb(pugi::xml_node Parent, t_block *cb, const int index,
	t_pb* pb, t_pb_route *pb_route, int *num_primitives,
    const pugiutil::loc_data& loc_data) {
	int i, j, pb_index;
	bool found;
	const t_pb_type *pb_type;
	t_token *tokens;
	int num_tokens;

    auto inputs = pugiutil::get_single_child(Parent, "inputs", loc_data);
	processPorts(inputs, pb, pb_route, loc_data);

    auto outputs = pugiutil::get_single_child(Parent, "outputs", loc_data);
	processPorts(outputs, pb, pb_route, loc_data);

    auto clocks = pugiutil::get_single_child(Parent, "clocks", loc_data);
	processPorts(clocks, pb, pb_route, loc_data);

	pb_type = pb->pb_graph_node->pb_type;
	if (pb_type->num_modes == 0) {
        AtomBlockId blk_id = g_atom_nl.find_block(pb->name);
		if (!blk_id) {
			vpr_throw(VPR_ERROR_NET_F, __FILE__, __LINE__,
					".net file and .blif file do not match, encountered unknown primitive %s in .net file.\n",
					pb->name);
		}

        //Update atom netlist mapping
        VTR_ASSERT(blk_id);
        g_atom_map.set_atom_pb(blk_id, pb);
        g_atom_map.set_atom_clb(blk_id, index);

		(*num_primitives)++;
	} else {
		/* process children of child if exists */

		pb->child_pbs = (t_pb **) vtr::calloc(pb_type->modes[pb->mode].num_pb_type_children, sizeof(t_pb*));
		for (i = 0; i < pb_type->modes[pb->mode].num_pb_type_children; i++) {
			pb->child_pbs[i] = (t_pb *) vtr::calloc(pb_type->modes[pb->mode].pb_type_children[i].num_pb, sizeof(t_pb));
		}

		/* Populate info for each physical block  */
        for(auto child = Parent.child("block"); child; child = child.next_sibling("block")) {
            VTR_ASSERT(strcmp(child.name(), "block") == 0);

            auto instance_type = pugiutil::get_attribute(child, "instance", loc_data);
            tokens = GetTokensFromString(instance_type.value(), &num_tokens);
            if (num_tokens != 4 || tokens[0].type != TOKEN_STRING
                    || tokens[1].type != TOKEN_OPEN_SQUARE_BRACKET
                    || tokens[2].type != TOKEN_INT
                    || tokens[3].type != TOKEN_CLOSE_SQUARE_BRACKET) {
                vpr_throw(VPR_ERROR_NET_F, loc_data.filename_c_str(), loc_data.line(child),
                        "Unknown syntax for instance %s in %s. Expected pb_type[instance_number].\n",
                        instance_type.value(), child.name());
            }

            found = false;
            pb_index = OPEN;
            for (i = 0; i < pb_type->modes[pb->mode].num_pb_type_children; i++) {
                if (strcmp( pb_type->modes[pb->mode].pb_type_children[i].name, tokens[0].data) == 0) {
                    if (vtr::atoi(tokens[2].data) >= pb_type->modes[pb->mode].pb_type_children[i].num_pb) {
                        vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(child),
                                "Instance number exceeds # of pb available for instance %s in %s.\n",
                                instance_type.value(), child.name());
                    }
                    pb_index = vtr::atoi(tokens[2].data);
                    if (pb->child_pbs[i][pb_index].pb_graph_node != NULL) {
                        vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(child),
                                "node is used by two different blocks %s and %s.\n",
                                instance_type.value(),
                                pb->child_pbs[i][pb_index].name);
                    }
                    pb->child_pbs[i][pb_index].pb_graph_node = &pb->pb_graph_node->child_pb_graph_nodes[pb->mode][i][pb_index];
                    found = true;
                    break;
                }
            }
            if (!found) {
                vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(child),
                        "Unknown pb type %s.\n", instance_type.value());
            }

            auto name = pugiutil::get_attribute(child, "name", loc_data);
            if (0 != strcmp(name.value(), "open")) {
                pb->child_pbs[i][pb_index].name = vtr::strdup(name.value());

                /* Parse all pbs and CB internal nets*/
                g_atom_map.set_atom_pb(AtomBlockId::INVALID(), &pb->child_pbs[i][pb_index]);

                auto mode = child.attribute("mode");
                pb->child_pbs[i][pb_index].mode = 0;
                found = false;
                for (j = 0; j < pb->child_pbs[i][pb_index].pb_graph_node->pb_type->num_modes; j++) {
                    if (strcmp(mode.value(), pb->child_pbs[i][pb_index].pb_graph_node->pb_type->modes[j].name) == 0) {
                        pb->child_pbs[i][pb_index].mode = j;
                        found = true;
                    }
                }
                if (!found && pb->child_pbs[i][pb_index].pb_graph_node->pb_type->num_modes != 0) {
                    vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(child),
                            "Unknown mode %s for cb %s #%d.\n", mode.value(),
                            pb->child_pbs[i][pb_index].name, pb_index);
                }
                pb->child_pbs[i][pb_index].parent_pb = pb;

                processPb(child, cb, index, &pb->child_pbs[i][pb_index], pb_route, num_primitives,	loc_data);
            } else {
                /* physical block has no used primitives but it may have used routing */
                pb->child_pbs[i][pb_index].name = NULL;
                g_atom_map.set_atom_pb(AtomBlockId::INVALID(), &pb->child_pbs[i][pb_index]);

                auto lookahead1 = pugiutil::get_first_child(child, "outputs", loc_data, pugiutil::OPTIONAL);
                if (lookahead1) {
                    pugiutil::get_first_child(lookahead1, "port", loc_data); //Check that port child tag exists
                    auto mode = pugiutil::get_attribute(child, "mode", loc_data);

                    pb->child_pbs[i][pb_index].mode = 0;
                    found = false;
                    for (j = 0; j < pb->child_pbs[i][pb_index].pb_graph_node->pb_type->num_modes; j++) {
                        if (strcmp(mode.value(), pb->child_pbs[i][pb_index].pb_graph_node->pb_type->modes[j].name) == 0) {
                            pb->child_pbs[i][pb_index].mode = j;
                            found = true;
                        }
                    }
                    if (!found && pb->child_pbs[i][pb_index].pb_graph_node->pb_type->num_modes != 0) {
                        vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(child),
                                "Unknown mode %s for cb %s #%d.\n", mode.value(),
                                pb->child_pbs[i][pb_index].name, pb_index);
                    }
                    pb->child_pbs[i][pb_index].parent_pb = pb;
                    processPb(child, cb, index, &pb->child_pbs[i][pb_index], pb_route, num_primitives, loc_data);
                }
            }
            freeTokens(tokens, num_tokens);
		}
	}
}

/**
 * Allocates memory for nets and loads the name of the net so that it can be identified and loaded with
 * more complete information later
 * ncount - number of nets in the hashtable of nets
 * nhash - hashtable of nets
 * returns array of nets stored in hashtable
 */
static struct s_net *alloc_and_init_netlist_from_hash(const int ncount,
		struct s_hash **nhash) {
	struct s_net *nlist;
	struct s_hash_iterator hash_iter;
	struct s_hash *curr_net;
	int i;

	nlist = (struct s_net *) vtr::calloc(ncount, sizeof(struct s_net));

	hash_iter = start_hash_table_iterator();
	curr_net = get_next_hash(nhash, &hash_iter);
	while (curr_net != NULL) {
		VTR_ASSERT(nlist[curr_net->index].name == NULL);
		nlist[curr_net->index].name = vtr::strdup(curr_net->name);
		nlist[curr_net->index].num_sinks = curr_net->count - 1;

		nlist[curr_net->index].node_block = (int *) vtr::malloc(
				curr_net->count * sizeof(int));
		nlist[curr_net->index].node_block_pin = (int *) vtr::malloc(
				curr_net->count * sizeof(int));
		nlist[curr_net->index].is_routed = false;
		nlist[curr_net->index].is_fixed = false;
		nlist[curr_net->index].is_global = false;
		for (i = 0; i < curr_net->count; i++) {
			nlist[curr_net->index].node_block[i] = OPEN;
			nlist[curr_net->index].node_block_pin[i] = OPEN;
		}
		curr_net = get_next_hash(nhash, &hash_iter);
	}
	return nlist;
}

/**
 * Adds net to hashtable of nets.  If the net is "open", then this is a keyword so do not add it.  
 * If the net already exists, increase the count on that net 
 */
static int add_net_to_hash(struct s_hash **nhash, const char *net_name,
		int *ncount) {
	struct s_hash *hash_value;

	if (strcmp(net_name, "open") == 0) {
		return OPEN;
	}

	hash_value = insert_in_hash_table(nhash, net_name, *ncount);
	if (hash_value->count == 1) {
		VTR_ASSERT(*ncount == hash_value->index);
		(*ncount)++;
	}
	return hash_value->index;
}

static void processPorts(pugi::xml_node Parent, t_pb* pb, t_pb_route *pb_route,
        const pugiutil::loc_data& loc_data) {

	int i, j, in_port, out_port, clock_port, num_tokens;
    std::vector<std::string> pins;
	int rr_node_index;
	t_pb_graph_pin *** pin_node;
	int *num_ptrs, num_sets;
	bool found;

    for(auto Cur = pugiutil::get_first_child(Parent, "port", loc_data, pugiutil::OPTIONAL); Cur; Cur = Cur.next_sibling("port")) {

        auto port_name = pugiutil::get_attribute(Cur, "name", loc_data);

        //Determine the port index on the pb
        //
        //Traverse all the ports on the pb until we find the matching port name,
        //at that point in_port/clock_port/output_port will be the index associated
        //with that port
        in_port = out_port = clock_port = 0;
        found = false;
        for (i = 0; i < pb->pb_graph_node->pb_type->num_ports; i++) {
            if (0 == strcmp(pb->pb_graph_node->pb_type->ports[i].name, port_name.value())) {
                found = true;
                break;
            }
            if (pb->pb_graph_node->pb_type->ports[i].is_clock
                    && pb->pb_graph_node->pb_type->ports[i].type == IN_PORT) {
                clock_port++;
            } else if (!pb->pb_graph_node->pb_type->ports[i].is_clock
                    && pb->pb_graph_node->pb_type->ports[i].type == IN_PORT) {
                in_port++;
            } else {
                VTR_ASSERT( pb->pb_graph_node->pb_type->ports[i].type == OUT_PORT);
                out_port++;
            }
        }
        if (!found) {
            vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(Cur),
                    "Unknown port %s for pb %s[%d].\n", port_name.value(),
                    pb->pb_graph_node->pb_type->name,
                    pb->pb_graph_node->placement_index);
        }

        //Extract all the pins for this port
        pins = vtr::split(Cur.text().get());
        num_tokens = pins.size();

        //Check that the number of pins from the netlist file matches the pb port's number of pins
        if (0 == strcmp(Parent.name(), "inputs")) {
            if (num_tokens != pb->pb_graph_node->num_input_pins[in_port]) {
                vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(Cur),
                        "Incorrect # pins %d found (expected %d) for input port %s for pb %s[%d].\n",
                        num_tokens, pb->pb_graph_node->num_input_pins[in_port], port_name.value(), pb->pb_graph_node->pb_type->name,
                        pb->pb_graph_node->placement_index);
            }
        } else if (0 == strcmp(Parent.name(), "outputs")) {
            if (num_tokens != pb->pb_graph_node->num_output_pins[out_port]) {
                vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(Cur),
                        "Incorrect # pins %d (expected %d) found for output port %s for pb %s[%d].\n",
                        num_tokens, pb->pb_graph_node->num_output_pins[out_port], port_name.value(), pb->pb_graph_node->pb_type->name,
                        pb->pb_graph_node->placement_index);
            }
        } else {
            VTR_ASSERT(0 == strcmp(Parent.name(), "clocks"));
            if (num_tokens != pb->pb_graph_node->num_clock_pins[clock_port]) {
                vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(Cur),
                        "Incorrect # pins %d found for clock port %s for pb %s[%d].\n",
                        num_tokens, pb->pb_graph_node->num_clock_pins[clock_port], port_name.value(), pb->pb_graph_node->pb_type->name,
                        pb->pb_graph_node->placement_index);
            }
        }

        //Process the input and clock ports
        if (0 == strcmp(Parent.name(), "inputs") || 0 == strcmp(Parent.name(), "clocks")) {
            if (pb->parent_pb == NULL) {
                //We are processing a top-level pb, so these pins connect to inter-block nets
                for (i = 0; i < num_tokens; i++) {
                    //Set rr_node_index to the pb_route for the appropriate port
                    if (0 == strcmp(Parent.name(), "inputs"))
                        rr_node_index = pb->pb_graph_node->input_pins[in_port][i].pin_count_in_cluster;
                    else
                        rr_node_index = pb->pb_graph_node->clock_pins[clock_port][i].pin_count_in_cluster;


                    if (strcmp(pins[i].c_str(), "open") != 0) {
                        //For connected pins look-up the inter-block net index associated with it
                        AtomNetId net_id = g_atom_nl.find_net(pins[i].c_str());
                        if (!net_id) {
                            vpr_throw(VPR_ERROR_NET_F, __FILE__, __LINE__,
                                    ".blif and .net do not match, unknown net %s found in .net file.\n.",
                                    pins[i].c_str());
                        }
                        //Mark the associated inter-block net
                        pb_route[rr_node_index].atom_net_id = net_id;
                    }						
                }
            } else {
                //We are processing an internal pb
                for (i = 0; i < num_tokens; i++) {
                    if (0 == strcmp(pins[i].c_str(), "open")) {
                        continue;
                    }

                    //Extract the portion of the pin name after '->'
                    //
                    //e.g. 'memory.addr1[0]->address1' becomes 'address1'
                    size_t loc = pins[i].find("->");
                    VTR_ASSERT(loc != std::string::npos);

                    std::string pin_name = pins[i].substr(0, loc);

                    loc += 2; //Skip over the '->'
                    std::string interconnect_name = pins[i].substr(loc, std::string::npos);

                    pin_node = alloc_and_load_port_pin_ptrs_from_string(
                                    pb->pb_graph_node->pb_type->parent_mode->interconnect[0].line_num,
                                    pb->pb_graph_node->parent_pb_graph_node,
                                    pb->pb_graph_node->parent_pb_graph_node->child_pb_graph_nodes[pb->parent_pb->mode],
                                    pin_name.c_str(), &num_ptrs, &num_sets, true,
                                    true);
                    VTR_ASSERT(num_sets == 1 && num_ptrs[0] == 1);
                    if (0 == strcmp(Parent.name(), "inputs"))
                        rr_node_index = pb->pb_graph_node->input_pins[in_port][i].pin_count_in_cluster;
                    else
                        rr_node_index = pb->pb_graph_node->clock_pins[clock_port][i].pin_count_in_cluster;
                    pb_route[rr_node_index].prev_pb_pin_id = pin_node[0][0]->pin_count_in_cluster;
                    found = false;
                    for (j = 0; j < pin_node[0][0]->num_output_edges; j++) {
                        if (0 == strcmp(interconnect_name.c_str(), pin_node[0][0]->output_edges[j]->interconnect->name)) {
                            found = true;
                            break;
                        }
                    }
                    for (j = 0; j < num_sets; j++) {
                        free(pin_node[j]);
                    }
                    free(pin_node);
                    free(num_ptrs);
                    if (!found) {
                        vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(Cur),
                                "Unknown interconnect %s connecting to pin %s.\n",
                                interconnect_name.c_str(), pin_name.c_str());
                    }
                }
            }
        }

        if (0 == strcmp(Parent.name(), "outputs")) {
            if (pb->pb_graph_node->pb_type->num_modes == 0) {
                /* primitives are drivers of nets */
                for (i = 0; i < num_tokens; i++) {
                    rr_node_index = pb->pb_graph_node->output_pins[out_port][i].pin_count_in_cluster;
                    if (strcmp(pins[i].c_str(), "open") != 0) {
                        AtomNetId net_id = g_atom_nl.find_net(pins[i].c_str());
                        if (!net_id) {
                            vpr_throw(VPR_ERROR_NET_F, __FILE__, __LINE__,
                                    ".blif and .net do not match, unknown net %s found in .net file.\n",
                                    pins[i].c_str());
                        }
                        pb_route[rr_node_index].atom_net_id = net_id;
                    }
                }
            } else {
                for (i = 0; i < num_tokens; i++) {
                    if (0 == strcmp(pins[i].c_str(), "open")) {
                        continue;
                    }
                    //Extract the portion of the pin name after '->'
                    //
                    //e.g. 'memory.addr1[0]->address1' becomes 'address1'
                    size_t loc = pins[i].find("->");
                    VTR_ASSERT(loc != std::string::npos);

                    std::string pin_name = pins[i].substr(0, loc);

                    loc += 2; //Skip over the '->'
                    std::string interconnect_name = pins[i].substr(loc, std::string::npos);

                    pin_node = alloc_and_load_port_pin_ptrs_from_string(
                                    pb->pb_graph_node->pb_type->modes[pb->mode].interconnect->line_num,
                                    pb->pb_graph_node,
                                    pb->pb_graph_node->child_pb_graph_nodes[pb->mode],
                                    pin_name.c_str(), &num_ptrs, &num_sets, true,
                                    true);
                    VTR_ASSERT(num_sets == 1 && num_ptrs[0] == 1);
                    rr_node_index = pb->pb_graph_node->output_pins[out_port][i].pin_count_in_cluster;
                    pb_route[rr_node_index].prev_pb_pin_id = pin_node[0][0]->pin_count_in_cluster;
                    found = false;
                    for (j = 0; j < pin_node[0][0]->num_output_edges; j++) {
                        if (0 == strcmp(interconnect_name.c_str(), pin_node[0][0]->output_edges[j]->interconnect->name)) {
                            found = true;
                            break;
                        }
                    }
                    for (j = 0; j < num_sets; j++) {
                        free(pin_node[j]);
                    }
                    free(pin_node);
                    free(num_ptrs);
                    if (!found) {
                        vpr_throw(VPR_ERROR_NET_F, netlist_file_name, loc_data.line(Cur),
                                "Unknown interconnect %s connecting to pin %s.\n",
                                interconnect_name.c_str(), pin_name.c_str());
                    }
                }
            }
        }
	}
}

/**  
 * This function updates the nets list and the connections between that list and the complex block
 */
static void load_external_nets_and_cb(const int L_num_blocks,
		const struct s_block block_list[],
		int *ext_ncount,
		struct s_net **ext_nets, const std::vector<std::string>& circuit_clocks) {
	int i, j, k, ipin;
	struct s_hash **ext_nhash;
	t_pb_graph_pin *pb_graph_pin;
	int *count;
	int netnum, num_tokens;

	*ext_ncount = 0;
	ext_nhash = alloc_hash_table();

	/* Assumes that complex block pins are ordered inputs, outputs, globals */

	/* Determine the external nets of complex block */
	for (i = 0; i < L_num_blocks; i++) {
		ipin = 0;
		if (block_list[i].type->pb_type->num_input_pins
				+ block_list[i].type->pb_type->num_output_pins
				+ block_list[i].type->pb_type->num_clock_pins
				!= block_list[i].type->num_pins / block_list[i].type->capacity) {

			VTR_ASSERT(0);
		}

		VTR_ASSERT( block_list[i].type->pb_type->num_input_pins
						+ block_list[i].type->pb_type->num_output_pins
						+ block_list[i].type->pb_type->num_clock_pins
						== block_list[i].type->num_pins / block_list[i].type->capacity);

        //Load the external nets connected to input ports
		for (j = 0; j < block_list[i].pb->pb_graph_node->num_input_ports; j++) {
			for (k = 0; k < block_list[i].pb->pb_graph_node->num_input_pins[j]; k++) {
				pb_graph_pin = &block_list[i].pb->pb_graph_node->input_pins[j][k];
				VTR_ASSERT(pb_graph_pin->pin_count_in_cluster == ipin);

				if (block_list[i].pb_route[pb_graph_pin->pin_count_in_cluster].atom_net_id) {
                    AtomNetId net_id = block_list[i].pb_route[pb_graph_pin->pin_count_in_cluster].atom_net_id;
					block_list[i].nets[ipin] = add_net_to_hash(ext_nhash,
                                                g_atom_nl.net_name(net_id).c_str(),
                                                ext_ncount);
				} else {
					block_list[i].nets[ipin] = OPEN;
				}
				ipin++;
			}
		}

        //Load the external nets connected to output ports
		for (j = 0; j < block_list[i].pb->pb_graph_node->num_output_ports; j++) {
			for (k = 0; k < block_list[i].pb->pb_graph_node->num_output_pins[j]; k++) {
				pb_graph_pin = &block_list[i].pb->pb_graph_node->output_pins[j][k];
				VTR_ASSERT(pb_graph_pin->pin_count_in_cluster == ipin);
				if (block_list[i].pb_route[pb_graph_pin->pin_count_in_cluster].atom_net_id) {
                    AtomNetId net_id = block_list[i].pb_route[pb_graph_pin->pin_count_in_cluster].atom_net_id;
					block_list[i].nets[ipin] = add_net_to_hash(ext_nhash,
                                                g_atom_nl.net_name(net_id).c_str(),
                                                ext_ncount);
				} else {
					block_list[i].nets[ipin] = OPEN;
				}
				ipin++;
			}
		}

        //Load the external nets connected to clock ports
		for (j = 0; j < block_list[i].pb->pb_graph_node->num_clock_ports; j++) {
			for (k = 0; k < block_list[i].pb->pb_graph_node->num_clock_pins[j]; k++) {
				pb_graph_pin = &block_list[i].pb->pb_graph_node->clock_pins[j][k];
				VTR_ASSERT(pb_graph_pin->pin_count_in_cluster == ipin);
				if (block_list[i].pb_route[pb_graph_pin->pin_count_in_cluster].atom_net_id) {
                    AtomNetId net_id = block_list[i].pb_route[pb_graph_pin->pin_count_in_cluster].atom_net_id;
					block_list[i].nets[ipin] = add_net_to_hash(ext_nhash,
                                                g_atom_nl.net_name(net_id).c_str(),
                                                ext_ncount);
				} else {
					block_list[i].nets[ipin] = OPEN;
				}
				ipin++;
			}
		}
		for (j = ipin; j < block_list[i].type->num_pins; j++) {
			block_list[i].nets[ipin] = OPEN;
		}
	}

	/* alloc and partially load the list of external nets */
	(*ext_nets) = alloc_and_init_netlist_from_hash(*ext_ncount, ext_nhash);

	/* Load global nets */
	num_tokens = circuit_clocks.size();

	count = (int *) vtr::calloc(*ext_ncount, sizeof(int));

	/* complete load of external nets so that each net points back to the blocks */
	for (i = 0; i < L_num_blocks; i++) {
		ipin = 0;
		for (j = 0; j < block_list[i].type->num_pins; j++) {
			netnum = block_list[i].nets[j];
			if (netnum != OPEN) {
				if (RECEIVER == block_list[i].type->class_inf[block_list[i].type->pin_class[j]].type) {
					count[netnum]++;
					if (count[netnum] > (*ext_nets)[netnum].num_sinks) {
						vpr_throw(VPR_ERROR_NET_F, __FILE__, __LINE__,
								"net %s #%d inconsistency, expected %d terminals but encountered %d terminals, it is likely net terminal is disconnected in netlist file.\n",
								(*ext_nets)[netnum].name, netnum, count[netnum],
								(*ext_nets)[netnum].num_sinks);
					}

					(*ext_nets)[netnum].node_block[count[netnum]] = i;
					(*ext_nets)[netnum].node_block_pin[count[netnum]] = j;

					(*ext_nets)[netnum].is_global =
							block_list[i].type->is_global_pin[j]; /* Error check performed later to ensure no mixing of global and non-global signals */
				} else {
					VTR_ASSERT(DRIVER == block_list[i].type->class_inf[block_list[i].type->pin_class[j]].type);
					VTR_ASSERT((*ext_nets)[netnum].node_block[0] == OPEN);
					(*ext_nets)[netnum].node_block[0] = i;
					(*ext_nets)[netnum].node_block_pin[0] = j;
				}
			}
		}
	}
	/* Error check global and non global signals */
	for (i = 0; i < *ext_ncount; i++) {
		for (j = 1; j <= (*ext_nets)[i].num_sinks; j++) {
			bool is_global_net =
					static_cast<bool>((*ext_nets)[i].is_global);
			if (block_list[(*ext_nets)[i].node_block[j]].type->is_global_pin[(*ext_nets)[i].node_block_pin[j]]
					!= is_global_net) {
				vpr_throw(VPR_ERROR_NET_F, __FILE__, __LINE__,
						"Netlist attempts to connect net %s to both global and non-global pins.\n",
						(*ext_nets)[i].name);
			}
		}
		for (j = 0; j < num_tokens; j++) {
			if (strcmp(circuit_clocks[j].c_str(), (*ext_nets)[i].name) == 0) {
				VTR_ASSERT((*ext_nets)[i].is_global == true); /* above code should have caught this case, if not, then bug in code */
			}
		}
	}
	free(count);
	free_hash_table(ext_nhash);
}


static void mark_constant_generators(const int L_num_blocks,
		const struct s_block block_list[]) {
	int i;
	for (i = 0; i < L_num_blocks; i++) {
		mark_constant_generators_rec(block_list[i].pb,
				block_list[i].pb_route);
	}
}

static void mark_constant_generators_rec(const t_pb *pb, const t_pb_route *pb_route) {
	int i, j;
	t_pb_type *pb_type;
	bool const_gen;
	if (pb->pb_graph_node->pb_type->blif_model == NULL) {
		for (i = 0; i < pb->pb_graph_node->pb_type->modes[pb->mode].num_pb_type_children; i++) {
			pb_type = &(pb->pb_graph_node->pb_type->modes[pb->mode].pb_type_children[i]);
			for (j = 0; j < pb_type->num_pb; j++) {
				if (pb->child_pbs[i][j].name != NULL) {
					mark_constant_generators_rec(&(pb->child_pbs[i][j]), pb_route);
				}
			}
		}
	} else if (strcmp(pb->pb_graph_node->pb_type->name, "inpad") != 0) {
		const_gen = true;
		for (i = 0; i < pb->pb_graph_node->num_input_ports && const_gen == true; i++) {
			for (j = 0; j < pb->pb_graph_node->num_input_pins[i] && const_gen == true; j++) {
                int cluster_pin_idx = pb->pb_graph_node->input_pins[i][j].pin_count_in_cluster;
				if (pb_route[cluster_pin_idx].atom_net_id) {
					const_gen = false;
				}
			}
		}
		for (i = 0; i < pb->pb_graph_node->num_clock_ports && const_gen == true; i++) {
			for (j = 0; j < pb->pb_graph_node->num_clock_pins[i] && const_gen == true; j++) {
                int cluster_pin_idx = pb->pb_graph_node->clock_pins[i][j].pin_count_in_cluster;
				if (pb_route[cluster_pin_idx].atom_net_id) {
					const_gen = false;
				}
			}
		}
		if (const_gen == true) {
			vtr::printf_info("%s is a constant generator.\n", pb->name);
			for (i = 0; i < pb->pb_graph_node->num_output_ports; i++) {
				for (j = 0; j < pb->pb_graph_node->num_output_pins[i]; j++) {
                    int cluster_pin_idx = pb->pb_graph_node->output_pins[i][j].pin_count_in_cluster;
					if (pb_route[cluster_pin_idx].atom_net_id) {
                        AtomNetId net_id = pb_route[pb->pb_graph_node->output_pins[i][j].pin_count_in_cluster].atom_net_id;
                        AtomPinId driver_pin_id = g_atom_nl.net_driver(net_id);
                        VTR_ASSERT(g_atom_nl.pin_is_constant(driver_pin_id));
					}
				}
			}
		}
	}
}

static t_pb_route *alloc_pb_route(t_pb_graph_node *pb_graph_node) {
	t_pb_route *pb_route;
	int num_pins = pb_graph_node->total_pb_pins;

	VTR_ASSERT(pb_graph_node->parent_pb_graph_node == NULL); /* This function only operates on top-level pb_graph_node */

	pb_route = new t_pb_route[num_pins];

	return pb_route;
}

static void load_interal_to_block_net_nums(const t_type_ptr type, t_pb_route *pb_route) {
	int num_pins = type->pb_graph_head->total_pb_pins;

	for (int i = 0; i < num_pins; i++) {
		if (pb_route[i].prev_pb_pin_id != OPEN && !pb_route[i].atom_net_id) {
			load_atom_index_for_pb_pin(pb_route, i);
		}
	}
}

static void load_atom_index_for_pb_pin(t_pb_route *pb_route, int ipin) {
	int driver = pb_route[ipin].prev_pb_pin_id;
	
	VTR_ASSERT(driver != OPEN);
	VTR_ASSERT(!pb_route[ipin].atom_net_id);

	if (!pb_route[driver].atom_net_id) {
		load_atom_index_for_pb_pin(pb_route, driver);
	}	
		
	pb_route[ipin].atom_net_id = pb_route[driver].atom_net_id;
}
