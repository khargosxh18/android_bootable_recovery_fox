/*
	Copyright (C) 2024-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _FOX_FIFO_HPP
#define _FOX_FIFO_HPP

#include <string>
#include <vector>

#include <json/json.h>

// Fox_Fifo is the command engine behind Fox RPC. It dispatches a parsed,
// typed request (an op plus a JSON args object) straight to a handler — there
// is no argv round-trip. Request formatting is the client's job (foxcli); the
// engine only consumes the structured request.
//
// A few high-frequency remote-control verbs (screencap, input, screenstream)
// are intercepted in Fox_Command_Dispatcher before the foxcmd action path, so
// screen/input traffic stays off singleaction_page and the RPC channel remains
// free while a continuous screen stream uses FOX_SCREEN_STREAM_FILE.
class Fox_Fifo
{
public:
	// Stash the parsed request (op + typed args) before the foxcmd action runs.
	static void Set_Request(const std::string& op, const Json::Value& args);

	// Execute the stashed command and return the result code.
	static int Run_Command();

private:
	static std::string op;
	static Json::Value cmd_args;

	// Each handler reads its parameters directly from the typed args object
	// (see arg_* helpers in fox_fifo.cpp). The JSON field names are the wire
	// contract shared with foxcli.
	static int Cmd_Mount(const Json::Value& args, bool unmount);
	static int Cmd_Flash(const Json::Value& args);
	static int Cmd_Backup(const Json::Value& args);
	static int Cmd_Restore(const Json::Value& args);
	static int Cmd_Decrypt(const Json::Value& args);
	static int Cmd_Sideload(const Json::Value& args);
	static int Cmd_Status(const Json::Value& args);
	static int Cmd_Reboot(const Json::Value& args);
	static int Cmd_Internal(const Json::Value& args);
	static int Cmd_Ors(const Json::Value& args);
	static int Cmd_OrsCmd(const Json::Value& args);
	static int Cmd_Wipe(const Json::Value& args);
	static int Cmd_Format_Data(const Json::Value& args);
	static int Cmd_Reflash(const Json::Value& args);
	static int Cmd_Mtp(const Json::Value& args);
	static int Cmd_Log(const Json::Value& args);
	static int Cmd_Partition(const Json::Value& args);
	static int Cmd_Addons(const Json::Value& args);
	static int Cmd_Storages(const Json::Value& args);
	// input {tap|swipe|key|down|move|up}: inject touch/keys via uinput.
	static int Cmd_Input(const Json::Value& args);
	// screencap [path]: write the framebuffer to a PNG for ADB sync/pull.
	static int Cmd_Screencap(const Json::Value& args);

	// Build a ;-delimited backup/restore list from explicit partition specs
	// (mount points like "/system" or partition display names). Validates
	// each spec against PartitionManager and prints an error for unknowns.
	// Returns true on success; sets list_out.
	static bool Resolve_Partition_List(const std::vector<std::string>& specs,
	                                   std::string& list_out);
	static bool Resolve_Storage(const std::string& spec, std::string& path_out);

	// Emit a FOX_LIST_BEGIN/END block describing the partitions in the given
	// Get_Partition_List ListType ("mount", "backup", "wipe", "part_option").
	// Each record is "mount_point\tdisplay_name\tmounted(0|1)\textra". When
	// 'enrich' is true, 'extra' carries the file system and repair/resize/wipe
	// capability flags (used for partition management listings).
	static void Emit_Partition_List(const std::string& list_type, bool enrich);
};

#endif // _FOX_FIFO_HPP
