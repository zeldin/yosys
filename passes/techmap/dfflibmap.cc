/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/log.h"
#include "libparse.h"
#include <string.h>
#include <errno.h>

using namespace PASS_DFFLIBMAP;

struct cell_mapping {
	std::string cell_name;
	std::map<std::string, char> ports;
};
static std::map<RTLIL::IdString, cell_mapping> cell_mappings;

static void logmap(std::string dff)
{
	if (cell_mappings.count(dff) == 0) {
		log("    unmapped dff cell: %s\n", dff.c_str());
	} else {
		log("    %s %s (", cell_mappings[dff].cell_name.c_str(), dff.substr(1).c_str());
		bool first = true;
		for (auto &port : cell_mappings[dff].ports) {
			char arg[3] = { port.second, 0, 0 };
			if ('a' <= arg[0] && arg[0] <= 'z')
				arg[1] = arg[0] - ('a' - 'A'), arg[0] = '~';
			else
				arg[1] = arg[0], arg[0] = ' ';
			log("%s.%s(%s)", first ? "" : ", ", port.first.c_str(), arg);
			first = false;
		}
		log(");\n");
	}
}

static void logmap_all()
{
	logmap("$_DFF_N_");
	logmap("$_DFF_P_");

	logmap("$_DFF_NN0_");
	logmap("$_DFF_NN1_");
	logmap("$_DFF_NP0_");
	logmap("$_DFF_NP1_");
	logmap("$_DFF_PN0_");
	logmap("$_DFF_PN1_");
	logmap("$_DFF_PP0_");
	logmap("$_DFF_PP1_");

	logmap("$_DFFSR_NNN_");
	logmap("$_DFFSR_NNP_");
	logmap("$_DFFSR_NPN_");
	logmap("$_DFFSR_NPP_");
	logmap("$_DFFSR_PNN_");
	logmap("$_DFFSR_PNP_");
	logmap("$_DFFSR_PPN_");
	logmap("$_DFFSR_PPP_");
}

static bool parse_pin(LibertyAst *cell, LibertyAst *attr, std::string &pin_name, bool &pin_pol)
{
	if (cell == NULL || attr == NULL || attr->value.empty())
		return false;
	
	std::string value = attr->value;

	for (size_t pos = value.find_first_of("\" \t()"); pos != std::string::npos; pos = value.find_first_of("\" \t()"))
		value.erase(pos, 1);

	if (value[value.size()-1] == '\'') {
		pin_name = value.substr(0, value.size()-1);
		pin_pol = false;
	} else if (value[0] == '!') {
		pin_name = value.substr(1, value.size()-1);
		pin_pol = false;
	} else {
		pin_name = value;
		pin_pol = true;
	}

	for (auto child : cell->children)
		if (child->id == "pin" && child->args.size() == 1 && child->args[0] == pin_name)
			return true;
	return false;
}

static void find_cell(LibertyAst *ast, std::string cell_type, bool clkpol, bool has_reset, bool rstpol, bool rstval)
{
	LibertyAst *best_cell = NULL;
	std::map<std::string, char> best_cell_ports;
	int best_cell_pins = 0;
	float best_cell_area = 0;

	if (ast->id != "library")
		log_error("Format error in liberty file.\n");

	for (auto cell : ast->children)
	{
		if (cell->id != "cell" || cell->args.size() != 1)
			continue;

		LibertyAst *ff = cell->find("ff");
		if (ff == NULL)
			continue;

		std::string cell_clk_pin, cell_rst_pin, cell_next_pin;
		bool cell_clk_pol, cell_rst_pol, cell_next_pol;

		if (!parse_pin(cell, ff->find("clocked_on"), cell_clk_pin, cell_clk_pol) || cell_clk_pol != clkpol)
			continue;
		if (!parse_pin(cell, ff->find("next_state"), cell_next_pin, cell_next_pol))
			continue;
		if (has_reset && rstval == false) {
			if (!parse_pin(cell, ff->find("clear"), cell_rst_pin, cell_rst_pol) || cell_rst_pol != rstpol)
				continue;
		}
		if (has_reset && rstval == true) {
			if (!parse_pin(cell, ff->find("preset"), cell_rst_pin, cell_rst_pol) || cell_rst_pol != rstpol)
				continue;
		}

		std::map<std::string, char> this_cell_ports;
		this_cell_ports[cell_clk_pin] = 'C';
		if (has_reset)
			this_cell_ports[cell_rst_pin] = 'R';
		this_cell_ports[cell_next_pin] = 'D';

		float area = 0;
		LibertyAst *ar = cell->find("area");
		if (ar != NULL && !ar->value.empty())
			area = atof(ar->value.c_str());

		int num_pins = 0;
		bool found_output = false;
		for (auto pin : cell->children)
		{
			if (pin->id != "pin" || pin->args.size() != 1)
				continue;

			LibertyAst *dir = pin->find("direction");
			if (dir == NULL || dir->value == "internal")
				continue;
			num_pins++;

			if (dir->value == "input" && this_cell_ports.count(pin->args[0]) == 0)
				goto continue_cell_loop;

			LibertyAst *func = pin->find("function");
			if (dir->value == "output" && func != NULL) {
				std::string value = func->value;
				for (size_t pos = value.find_first_of("\" \t"); pos != std::string::npos; pos = value.find_first_of("\" \t"))
					value.erase(pos, 1);
				if ((cell_next_pol == true && value == ff->args[0]) || (cell_next_pol == false && value == ff->args[1])) {
					this_cell_ports[pin->args[0]] = 'Q';
					found_output = true;
				}
			}

			if (this_cell_ports.count(pin->args[0]) == 0)
				this_cell_ports[pin->args[0]] = 0;
		}

		if (!found_output || (best_cell != NULL && num_pins > best_cell_pins))
			continue;

		if (best_cell != NULL && num_pins == best_cell_pins && area > best_cell_area)
			continue;

		best_cell = cell;
		best_cell_pins = num_pins;
		best_cell_area = area;
		best_cell_ports.swap(this_cell_ports);
	continue_cell_loop:;
	}

	if (best_cell != NULL) {
		log("  cell %s (pins=%d, area=%.2f) is a direct match for cell type %s.\n", best_cell->args[0].c_str(), best_cell_pins, best_cell_area, cell_type.substr(1).c_str());
		cell_mappings[cell_type].cell_name = best_cell->args[0];
		cell_mappings[cell_type].ports = best_cell_ports;
	}
}

static void find_cell_sr(LibertyAst *ast, std::string cell_type, bool clkpol, bool setpol, bool clrpol)
{
	LibertyAst *best_cell = NULL;
	std::map<std::string, char> best_cell_ports;
	int best_cell_pins = 0;
	float best_cell_area = 0;

	if (ast->id != "library")
		log_error("Format error in liberty file.\n");

	for (auto cell : ast->children)
	{
		if (cell->id != "cell" || cell->args.size() != 1)
			continue;

		LibertyAst *ff = cell->find("ff");
		if (ff == NULL)
			continue;

		std::string cell_clk_pin, cell_set_pin, cell_clr_pin, cell_next_pin;
		bool cell_clk_pol, cell_set_pol, cell_clr_pol, cell_next_pol;

		if (!parse_pin(cell, ff->find("clocked_on"), cell_clk_pin, cell_clk_pol) || cell_clk_pol != clkpol)
			continue;
		if (!parse_pin(cell, ff->find("next_state"), cell_next_pin, cell_next_pol))
			continue;
		if (!parse_pin(cell, ff->find("preset"), cell_set_pin, cell_set_pol) || cell_set_pol != setpol)
			continue;
		if (!parse_pin(cell, ff->find("clear"), cell_clr_pin, cell_clr_pol) || cell_clr_pol != clrpol)
			continue;

		std::map<std::string, char> this_cell_ports;
		this_cell_ports[cell_clk_pin] = 'C';
		this_cell_ports[cell_set_pin] = 'S';
		this_cell_ports[cell_clr_pin] = 'R';
		this_cell_ports[cell_next_pin] = 'D';

		float area = 0;
		LibertyAst *ar = cell->find("area");
		if (ar != NULL && !ar->value.empty())
			area = atof(ar->value.c_str());

		int num_pins = 0;
		bool found_output = false;
		for (auto pin : cell->children)
		{
			if (pin->id != "pin" || pin->args.size() != 1)
				continue;

			LibertyAst *dir = pin->find("direction");
			if (dir == NULL || dir->value == "internal")
				continue;
			num_pins++;

			if (dir->value == "input" && this_cell_ports.count(pin->args[0]) == 0)
				goto continue_cell_loop;

			LibertyAst *func = pin->find("function");
			if (dir->value == "output" && func != NULL) {
				std::string value = func->value;
				for (size_t pos = value.find_first_of("\" \t"); pos != std::string::npos; pos = value.find_first_of("\" \t"))
					value.erase(pos, 1);
				if ((cell_next_pol == true && value == ff->args[0]) || (cell_next_pol == false && value == ff->args[1])) {
					this_cell_ports[pin->args[0]] = 'Q';
					found_output = true;
				}
			}

			if (this_cell_ports.count(pin->args[0]) == 0)
				this_cell_ports[pin->args[0]] = 0;
		}

		if (!found_output || (best_cell != NULL && num_pins > best_cell_pins))
			continue;

		if (best_cell != NULL && num_pins == best_cell_pins && area > best_cell_area)
			continue;

		best_cell = cell;
		best_cell_pins = num_pins;
		best_cell_area = area;
		best_cell_ports.swap(this_cell_ports);
	continue_cell_loop:;
	}

	if (best_cell != NULL) {
		log("  cell %s (pins=%d, area=%.2f) is a direct match for cell type %s.\n", best_cell->args[0].c_str(), best_cell_pins, best_cell_area, cell_type.substr(1).c_str());
		cell_mappings[cell_type].cell_name = best_cell->args[0];
		cell_mappings[cell_type].ports = best_cell_ports;
	}
}

static bool expand_cellmap_worker(std::string from, std::string to, std::string inv)
{
	if (cell_mappings.count(to) > 0)
		return false;

	log("  create mapping for %s from mapping for %s.\n", to.c_str(), from.c_str());
	cell_mappings[to].cell_name = cell_mappings[from].cell_name;
	cell_mappings[to].ports = cell_mappings[from].ports;

	for (auto &it : cell_mappings[to].ports) {
		char cmp_ch = it.second;
		if ('a' <= cmp_ch && cmp_ch <= 'z')
			cmp_ch -= 'a' - 'A';
		if (inv.find(cmp_ch) == std::string::npos)
			continue;
		if ('a' <= it.second && it.second <= 'z')
			it.second -= 'a' - 'A';
		else if ('A' <= it.second && it.second <= 'Z')
			it.second += 'a' - 'A';
	}
	return true;
}

static bool expand_cellmap(std::string pattern, std::string inv)
{
	std::vector<std::pair<std::string, std::string>> from_to_list;
	bool return_status = false;

	for (auto &it : cell_mappings) {
		std::string from = it.first.str(), to = it.first.str();
		if (from.size() != pattern.size())
			continue;
		for (size_t i = 0; i < from.size(); i++) {
			if (pattern[i] == '*') {
				to[i] = from[i] == 'P' ? 'N' :
					from[i] == 'N' ? 'P' :
					from[i] == '1' ? '0' :
					from[i] == '0' ? '1' : '*';
			} else
			if (pattern[i] != '?' && pattern[i] != from[i])
				goto pattern_failed;
		}
		from_to_list.push_back(std::pair<std::string, std::string>(from, to));
	pattern_failed:;
	}

	for (auto &it : from_to_list)
		return_status = return_status || expand_cellmap_worker(it.first, it.second, inv);
	return return_status;
}

static void map_sr_to_arst(const char *from, const char *to)
{
	if (!cell_mappings.count(from) || cell_mappings.count(to) > 0)
		return;

	char from_clk_pol = from[8], from_set_pol = from[9], from_clr_pol = from[10];
	char to_clk_pol = to[6], to_rst_pol = to[7], to_rst_val = to[8];

	log_assert(from_clk_pol == to_clk_pol);
	log_assert(to_rst_pol == from_set_pol && to_rst_pol == from_clr_pol);

	log("  create mapping for %s from mapping for %s.\n", to, from);
	cell_mappings[to].cell_name = cell_mappings[from].cell_name;
	cell_mappings[to].ports = cell_mappings[from].ports;

	for (auto &it : cell_mappings[to].ports)
	{
		bool is_set_pin = it.second == 'S' || it.second == 's';
		bool is_clr_pin = it.second == 'R' || it.second == 'r';

		if (!is_set_pin && !is_clr_pin)
			continue;

		if ((to_rst_val == '0' && is_set_pin) || (to_rst_val == '1' && is_clr_pin))
		{
			// this is the unused set/clr pin -- deactivate it
			if (is_set_pin)
				it.second = (from_set_pol == 'P') == (it.second == 'S') ? '0' : '1';
			else
				it.second = (from_clr_pol == 'P') == (it.second == 'R') ? '0' : '1';
		}
		else
		{
			// this is the used set/clr pin -- rename it to 'reset'
			if (it.second == 'S')
				it.second = 'R';
			if (it.second == 's')
				it.second = 'r';
		}
	}
}

static void dfflibmap(RTLIL::Design *design, RTLIL::Module *module)
{
	log("Mapping DFF cells in module `%s':\n", module->name.c_str());

	std::vector<RTLIL::Cell*> cell_list;
	for (auto &it : module->cells_) {
		if (design->selected(module, it.second) && cell_mappings.count(it.second->type) > 0)
			cell_list.push_back(it.second);
	}

	std::map<std::string, int> stats;
	for (auto cell : cell_list)
	{
		auto cell_type = cell->type;
		auto cell_name = cell->name;
		auto cell_connections = cell->connections();
		module->remove(cell);

		cell_mapping &cm = cell_mappings[cell_type];
		RTLIL::Cell *new_cell = module->addCell(cell_name, "\\" + cm.cell_name);

		for (auto &port : cm.ports) {
			RTLIL::SigSpec sig;
			if ('A' <= port.second && port.second <= 'Z') {
				sig = cell_connections[std::string("\\") + port.second];
			} else
			if (port.second == 'q') {
				RTLIL::SigSpec old_sig = cell_connections[std::string("\\") + char(port.second - ('a' - 'A'))];
				sig = module->addWire(NEW_ID, SIZE(old_sig));
				module->addNotGate(NEW_ID, sig, old_sig);
			} else
			if ('a' <= port.second && port.second <= 'z') {
				sig = cell_connections[std::string("\\") + char(port.second - ('a' - 'A'))];
				sig = module->NotGate(NEW_ID, sig);
			} else
			if (port.second == '0' || port.second == '1') {
				sig = RTLIL::SigSpec(port.second == '0' ? 0 : 1, 1);
			} else
			if (port.second != 0)
				log_abort();
			new_cell->setPort("\\" + port.first, sig);
		}

		stats[stringf("  mapped %%d %s cells to %s cells.\n", cell_type.c_str(), new_cell->type.c_str())]++;
	}

	for (auto &stat: stats)
		log(stat.first.c_str(), stat.second);
}

struct DfflibmapPass : public Pass {
	DfflibmapPass() : Pass("dfflibmap", "technology mapping of flip-flops") { }
	virtual void help()
	{
		log("\n");
		log("    dfflibmap -liberty <file> [selection]\n");
		log("\n");
		log("Map internal flip-flop cells to the flip-flop cells in the technology\n");
		log("library specified in the given liberty file.\n");
		log("\n");
		log("This pass may add inverters as needed. Therefore it is recommended to\n");
		log("first run this pass and then map the logic paths to the target technology.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing DFFLIBMAP pass (mapping DFF cells to sequential cells from liberty file).\n");

		std::string liberty_file;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			std::string arg = args[argidx];
			if (arg == "-liberty" && argidx+1 < args.size()) {
				liberty_file = args[++argidx];
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (liberty_file.empty())
			log_cmd_error("Missing `-liberty liberty_file' option!\n");

		std::ifstream f;
		f.open(liberty_file.c_str());
		if (f.fail())
			log_cmd_error("Can't open liberty file `%s': %s\n", liberty_file.c_str(), strerror(errno));
		LibertyParser libparser(f);
		f.close();

		find_cell(libparser.ast, "$_DFF_N_", false, false, false, false);
		find_cell(libparser.ast, "$_DFF_P_", true, false, false, false);

		find_cell(libparser.ast, "$_DFF_NN0_", false, true, false, false);
		find_cell(libparser.ast, "$_DFF_NN1_", false, true, false, true);
		find_cell(libparser.ast, "$_DFF_NP0_", false, true, true, false);
		find_cell(libparser.ast, "$_DFF_NP1_", false, true, true, true);
		find_cell(libparser.ast, "$_DFF_PN0_", true, true, false, false);
		find_cell(libparser.ast, "$_DFF_PN1_", true, true, false, true);
		find_cell(libparser.ast, "$_DFF_PP0_", true, true, true, false);
		find_cell(libparser.ast, "$_DFF_PP1_", true, true, true, true);

		find_cell_sr(libparser.ast, "$_DFFSR_NNN_", false, false, false);
		find_cell_sr(libparser.ast, "$_DFFSR_NNP_", false, false, true);
		find_cell_sr(libparser.ast, "$_DFFSR_NPN_", false, true, false);
		find_cell_sr(libparser.ast, "$_DFFSR_NPP_", false, true, true);
		find_cell_sr(libparser.ast, "$_DFFSR_PNN_", true, false, false);
		find_cell_sr(libparser.ast, "$_DFFSR_PNP_", true, false, true);
		find_cell_sr(libparser.ast, "$_DFFSR_PPN_", true, true, false);
		find_cell_sr(libparser.ast, "$_DFFSR_PPP_", true, true, true);

		// try to implement as many cells as possible just by inverting
		// the SET and RESET pins. If necessary, implement cell types
		// by inverting both D and Q. Only invert clock pins if there
		// is no other way of implementing the cell.
		while (1)
		{
			if (expand_cellmap("$_DFF_?*?_", "R") ||
					expand_cellmap("$_DFFSR_?*?_", "S") ||
					expand_cellmap("$_DFFSR_??*_", "R"))
				continue;

			if (expand_cellmap("$_DFF_??*_", "DQ"))
				continue;

			if (expand_cellmap("$_DFF_*_", "C") ||
					expand_cellmap("$_DFF_*??_", "C") ||
					expand_cellmap("$_DFFSR_*??_", "C"))
				continue;

			break;
		}

		map_sr_to_arst("$_DFFSR_NNN_", "$_DFF_NN0_");
		map_sr_to_arst("$_DFFSR_NNN_", "$_DFF_NN1_");
		map_sr_to_arst("$_DFFSR_NPP_", "$_DFF_NP0_");
		map_sr_to_arst("$_DFFSR_NPP_", "$_DFF_NP1_");
		map_sr_to_arst("$_DFFSR_PNN_", "$_DFF_PN0_");
		map_sr_to_arst("$_DFFSR_PNN_", "$_DFF_PN1_");
		map_sr_to_arst("$_DFFSR_PPP_", "$_DFF_PP0_");
		map_sr_to_arst("$_DFFSR_PPP_", "$_DFF_PP1_");
 
 		log("  final dff cell mappings:\n");
 		logmap_all();

		for (auto &it : design->modules_)
			if (design->selected(it.second) && !it.second->get_bool_attribute("\\blackbox"))
				dfflibmap(design, it.second);

		cell_mappings.clear();
	}
} DfflibmapPass;
 
