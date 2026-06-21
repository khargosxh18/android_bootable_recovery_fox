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

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fox_fifo.hpp"
#include "twrp-functions.hpp"
#include "partitions.hpp"
#include "twcommon.h"
#include "openrecoveryscript.hpp"
#include "variables.h"
#include "data.hpp"
#include "twrpRepacker.hpp"
#include "gui/gui.hpp"
#include "gui/gui.h"
#include "gui/pages.hpp"
#include "twinstall.h"

#include "fox_remote_input.hpp"
#include "fox_command_dispatcher.hpp"
#include "minuitwrp/minui.h"

std::string Fox_Fifo::op;
Json::Value Fox_Fifo::cmd_args;

void Fox_Fifo::Set_Request(const std::string& request_op, const Json::Value& args) {
	op = request_op;
	cmd_args = args;
}

// ---- Typed argument accessors --------------------------------------------
// Handlers read their parameters straight off the request's JSON args object.
// These helpers coerce loosely (a number or bool requested as a string is
// stringified, etc.) so a slightly off-typed client request still works.

// String value for 'key' ("" if absent). Numbers/bools are stringified.
static std::string arg_str(const Json::Value& a, const char* key) {
	if (!a.isObject() || !a.isMember(key))
		return "";
	const Json::Value& v = a[key];
	if (v.isString())
		return v.asString();
	if (v.isIntegral())
		return std::to_string(v.asInt64());
	if (v.isBool())
		return v.asBool() ? "1" : "0";
	return "";
}

// Convenience for the conventional "action" field (the subcommand verb).
static std::string arg_action(const Json::Value& a) {
	return arg_str(a, "action");
}

static bool arg_has(const Json::Value& a, const char* key) {
	return a.isObject() && a.isMember(key);
}

// Bool value for 'key', defaulting to 'dflt' when absent or not coercible.
static bool arg_bool(const Json::Value& a, const char* key, bool dflt = false) {
	if (!arg_has(a, key))
		return dflt;
	const Json::Value& v = a[key];
	if (v.isBool())
		return v.asBool();
	if (v.isIntegral())
		return v.asInt64() != 0;
	if (v.isString()) {
		std::string s = v.asString();
		return s == "1" || s == "true" || s == "yes" || s == "on";
	}
	return dflt;
}

// Integer value for 'key', defaulting to 'dflt' when absent.
static long arg_int(const Json::Value& a, const char* key, long dflt) {
	if (!arg_has(a, key))
		return dflt;
	const Json::Value& v = a[key];
	if (v.isIntegral())
		return (long)v.asInt64();
	if (v.isString())
		return strtol(v.asString().c_str(), nullptr, 10);
	return dflt;
}

// String array for 'key'. A lone string is accepted as a one-element list;
// numeric elements are stringified. Empty when absent.
static std::vector<std::string> arg_array(const Json::Value& a, const char* key) {
	std::vector<std::string> out;
	if (!arg_has(a, key))
		return out;
	const Json::Value& v = a[key];
	if (v.isArray()) {
		for (const Json::Value& e : v) {
			if (e.isString())
				out.push_back(e.asString());
			else if (e.isIntegral())
				out.push_back(std::to_string(e.asInt64()));
		}
	} else if (v.isString()) {
		out.push_back(v.asString());
	}
	return out;
}

// Generate a strong random alphanumeric password. Used by `web password
// --generate`. Sourced from /dev/urandom; the slight modulo bias across a
// 62-char alphabet is irrelevant for a 20-char secret.
static std::string generate_password(size_t len = 20) {
	static const char* cs =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	const size_t n = 62;
	std::string out;
	out.reserve(len);
	FILE* f = fopen("/dev/urandom", "rb");
	for (size_t i = 0; i < len; i++) {
		unsigned char b = 0;
		if (!f || fread(&b, 1, 1, f) != 1)
			b = static_cast<unsigned char>(time(nullptr) + i * 61);
		out.push_back(cs[b % n]);
	}
	if (f)
		fclose(f);
	return out;
}

static std::string normalize_partition_spec(const std::string& raw) {
	if (raw.empty() || raw[0] == '/')
		return raw;
	return "/" + raw;
}

static std::string display_name_without_size(const std::string& display_name) {
	size_t pos = display_name.find(" (");
	if (pos == std::string::npos)
		return display_name;
	return display_name.substr(0, pos);
}

static std::string lower_copy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return value;
}

static std::string read_text_file(const std::string& path) {
	std::ifstream input(path.c_str());
	if (!input.is_open())
		return "";
	std::ostringstream out;
	out << input.rdbuf();
	return out.str();
}

bool Fox_Fifo::Resolve_Partition_List(const std::vector<std::string>& specs,
                                     std::string& list_out) {
	list_out.clear();
	std::vector<PartitionList> backup_partitions;
	PartitionManager.Get_Partition_List("backup", &backup_partitions);

	for (const auto& raw : specs) {
		std::string path = normalize_partition_spec(raw);
		bool found = false;
		for (const auto& part : backup_partitions) {
			if (part.Mount_Point == path || part.Mount_Point == raw ||
			    display_name_without_size(part.Display_Name) == raw) {
				list_out += part.Mount_Point + ";";
				found = true;
				break;
			}
		}
		if (!found) {
			gui_print("fox: unknown partition '%s'\n", raw.c_str());
			return false;
		}
	}
	return true;
}

bool Fox_Fifo::Resolve_Storage(const std::string& spec, std::string& path_out) {
	path_out.clear();
	if (spec.empty())
		return false;
	if (spec[0] == '/') {
		path_out = spec;
		return true;
	}

	std::vector<PartitionList> storage_list;
	PartitionManager.Get_Partition_List("storage", &storage_list);

	std::string wanted = lower_copy(spec);
	for (const auto& storage : storage_list) {
		std::string path = storage.Mount_Point;
		std::string display = lower_copy(display_name_without_size(storage.Display_Name));
		std::string mount = lower_copy(path);

		bool matched = path == spec || mount == wanted || display == wanted;
		if (!matched && (wanted == "internal" || wanted == "emulated"))
			matched = mount == "/sdcard" || mount == "/data/media" || mount == "/data/media/0";
		if (!matched && (wanted == "microsd" || wanted == "sdcard" || wanted == "sd"))
			matched = mount == "/external_sd" || mount == "/external_sdcard" ||
			          mount == "/sdcard1";
		if (!matched && (wanted == "otg" || wanted == "usb" || wanted == "usb-otg"))
			matched = mount == "/usb_otg" || mount == "/usb-otg";

		if (matched) {
			path_out = path;
			return true;
		}
	}
	return false;
}

void Fox_Fifo::Emit_Partition_List(const std::string& list_type, bool enrich) {
	std::vector<PartitionList> parts;
	PartitionManager.Get_Partition_List(list_type, &parts);

	Json::Value items(Json::arrayValue);
	for (const auto& part : parts) {
		bool mounted = PartitionManager.Is_Mounted_By_Path(part.Mount_Point);
		std::string extra;
		if (enrich) {
			TWPartition* p = PartitionManager.Find_Partition_By_Path(part.Mount_Point);
			if (p) {
				extra = "fs=" + p->Current_File_System;
				if (p->Can_Repair())
					extra += ",repair";
				if (p->Can_Resize())
					extra += ",resize";
			}
		}
		Json::Value item(Json::objectValue);
		item["mount_point"] = part.Mount_Point;
		item["display_name"] = part.Display_Name;
		item["mounted"] = mounted;
		item["extra"] = extra;
		items.append(item);
	}
	Json::Value payload(Json::objectValue);
	payload["items"] = items;
	Fox_Command_Dispatcher::EmitData("list", payload);
}

int Fox_Fifo::Cmd_Mount(const Json::Value& args, bool unmount) {
	std::vector<std::string> a = arg_array(args, "paths");
	if (a.empty()) {
		// No partitions: show what is mountable and its current mount state.
		Emit_Partition_List("mount", false);
		return 0;
	}
	int rc = 0;
	for (const auto& path : a) {
		std::string p = path;
		if (!p.empty() && p[0] != '/')
			p = "/" + p;
		bool ok = unmount ? PartitionManager.UnMount_By_Path(p, true)
		                  : PartitionManager.Mount_By_Path(p, true);
		if (!ok) {
			rc = 1;
		} else {
			gui_print("%s %s\n", unmount ? "Unmounted" : "Mounted", p.c_str());
		}
	}
	return rc;
}

int Fox_Fifo::Cmd_Flash(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "zips");
	// Signature verification, reflash-after-flash and unmount toggles. The
	// verify/reflash fields only override the persistent (UI-configured) value
	// when present, so omitting one keeps the same default the GUI would use.
	bool verify = arg_has(args, "verify") && arg_bool(args, "verify");
	bool no_verify = arg_has(args, "verify") && !arg_bool(args, "verify");
	bool reflash = arg_has(args, "reflash") && arg_bool(args, "reflash");
	bool no_reflash = arg_has(args, "reflash") && !arg_bool(args, "reflash");
	bool unmount_system = arg_bool(args, "unmount_system");
	bool unmount_vendor = arg_bool(args, "unmount_vendor");

	if (a.empty()) {
		gui_print("fox: flash requires a zip path\n");
		return 2;
	}

	if (no_verify)
		DataManager::SetValue(TW_SIGNED_ZIP_VERIFY_VAR, 0);
	else if (verify)
		DataManager::SetValue(TW_SIGNED_ZIP_VERIFY_VAR, 1);
	if (reflash)
		DataManager::SetValue(TW_AUTO_REFLASHTWRP_VAR, 1);
	else if (no_reflash)
		DataManager::SetValue(TW_AUTO_REFLASHTWRP_VAR, 0);
	if (unmount_system)
		DataManager::SetValue(TW_UNMOUNT_SYSTEM, 1);
	if (unmount_vendor)
		DataManager::SetValue(TW_UNMOUNT_VENDOR, 1);

	int rc = 0;
	for (const auto& zip : a) {
		int wipe_cache = 0;
		if (TWinstall_zip(zip.c_str(), &wipe_cache) != 0)
			rc = 1;
	}

	// Reflash OrangeFox after a successful flash when enabled (default mirrors
	// the GUI's tw_auto_reflashtwrp). The direct installer path above does not
	// trigger the auto-reflash that the GUI flash flow performs, so do it here.
	if (rc == 0 && DataManager::GetIntValue(TW_AUTO_REFLASHTWRP_VAR) != 0) {
		twrpRepacker repacker;
		if (!repacker.Flash_Current_Twrp())
			gui_print("fox: warning: OrangeFox reflash failed\n");
	}
	return rc;
}

int Fox_Fifo::Cmd_Backup(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "parts");
	std::string name = arg_str(args, "name");
	std::string path = arg_str(args, "path");
	std::string storage = arg_str(args, "storage");
	bool compress = arg_bool(args, "compress");
	// digest generation is on by default; an explicit digest:false disables it.
	bool no_digest = arg_has(args, "digest") && !arg_bool(args, "digest", true);

	if (a.empty()) {
		// No partitions: show what can be backed up.
		Emit_Partition_List("backup", false);
		return 0;
	}

	std::string backup_list;
	if (!Resolve_Partition_List(a, backup_list))
		return 1;

	// Make sure storages are mounted before resolving the destination, so a real
	// persistent location is available (mirrors Cmd_Restore). Without this the
	// default tw_storage_path can still point at "/" (tmpfs), where a backup
	// completes instantly and silently vanishes on reboot.
	PartitionManager.Mount_All_Storage();

	// Default the destination to internal storage when the caller did not pick a
	// --storage or --path, so a "backup data" request lands on Internal Storage rather
	// than wherever tw_storage_path happens to point.
	bool storage_defaulted = false;
	if (storage.empty() && path.empty()) {
		storage = "internal";
		storage_defaulted = true;
	}

	if (!storage.empty()) {
		if (!Resolve_Storage(storage, path)) {
			if (!storage_defaulted) {
				gui_print("fox: unknown storage '%s' (try 'fox storages')\n", storage.c_str());
				return 1;
			}
			// Internal storage could not be resolved by alias; fall back to the
			// current storage path so the backup still lands somewhere real.
			path = DataManager::GetCurrentStoragePath();
		}
	}
	if (!path.empty()) {
		DataManager::SetValue("tw_storage_path", path);
		DataManager::SetBackupFolder();
	}

	// Guard: refuse to back up to a destination whose partition is not mounted.
	// Writing into an unmounted mount point lands on the underlying tmpfs (RAM),
	// where the backup finishes instantly and is silently lost on reboot. We
	// only check paths that map to a known partition, so an arbitrary --path
	// outside the partition table is left to the caller.
	std::string dest = path.empty() ? DataManager::GetCurrentStoragePath() : path;
	if (!dest.empty() && PartitionManager.Find_Partition_By_Path(dest) != NULL &&
	    !PartitionManager.Is_Mounted_By_Path(dest)) {
		gui_print("fox: backup destination '%s' is not mounted; aborting.\n", dest.c_str());
		gui_print("fox: mount it first, run 'fox decrypt' if storage is encrypted, "
		          "or pass --storage/--path.\n");
		return 1;
	}

	DataManager::SetValue(TW_USE_COMPRESSION_VAR, compress ? 1 : 0);
	DataManager::SetValue(TW_SKIP_DIGEST_GENERATE_VAR, no_digest ? 1 : 0);
	if (!name.empty())
		DataManager::SetValue(TW_BACKUP_NAME, name);
	else
		// Generate a real date/build-based name now. Setting the literal
		// "(Auto Generate)" only works in the GUI, where Run_Backup matches it
		// against the localized gui_lookup("auto_generate") string.
		TWFunc::Auto_Generate_Backup_Name();
	DataManager::SetValue("tw_backup_list", backup_list);

	if (!PartitionManager.Run_Backup(false)) {
		// Distinguish a user cancellation (via the fox cancel channel or the GUI)
		// from a genuine failure so the client can report it cleanly.
		if (PartitionManager.Check_Backup_Cancel() != 0) {
			gui_print("Backup cancelled\n");
			return 130;  // 128 + SIGINT, the shell convention for Ctrl+C
		}
		gui_print("fox: backup failed\n");
		return 1;
	}
	gui_print("Backup complete\n");
	return 0;
}

int Fox_Fifo::Cmd_Restore(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "parts");
	std::string name = arg_str(args, "name");
	std::string path = arg_str(args, "path");
	if (name.empty())
		name = path;
	// digest check is on by default; an explicit digest_check:false skips it.
	bool skip_digest = arg_has(args, "digest_check") && !arg_bool(args, "digest_check", true);

	if (name.empty()) {
		gui_print("fox: restore requires --name <backup name> or --path <absolute path>\n");
		return 2;
	}

	PartitionManager.Mount_All_Storage();
	DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR, skip_digest ? 1 : 0);

	// Resolve the backup folder: absolute path used directly, otherwise resolved
	// against the backups folder of each mounted storage (mirrors ORS restore).
	std::string folder = name;
	if (folder[0] != '/') {
		std::vector<PartitionList> storage_list;
		PartitionManager.Get_Partition_List("storage", &storage_list);
		bool found = false;
		for (const auto& s : storage_list) {
			if (PartitionManager.Is_Mounted_By_Path(s.Mount_Point)) {
				DataManager::SetValue("tw_storage_path", s.Mount_Point);
				std::string folder_var;
				DataManager::GetValue(TW_BACKUPS_FOLDER_VAR, folder_var);
				std::string candidate = folder_var + "/" + name;
				if (TWFunc::Path_Exists(candidate)) {
					folder = candidate;
					found = true;
					break;
				}
			}
		}
		if (!found) {
			gui_print("fox: unable to locate backup '%s'\n", name.c_str());
			return 1;
		}
	} else {
		folder += (folder[folder.size() - 1] == '/') ? "." : "/.";
	}

	if (!TWFunc::Path_Exists(folder)) {
		gui_print("fox: unable to locate backup '%s'\n", folder.c_str());
		return 1;
	}

	DataManager::SetValue("tw_restore", folder);
	PartitionManager.Set_Restore_Files(folder);

	int is_encrypted = 0;
	DataManager::GetValue("tw_restore_encrypted", is_encrypted);
	if (is_encrypted) {
		gui_print("fox: cannot restore an encrypted backup from the CLI\n");
		return 1;
	}

	std::string available;
	DataManager::GetValue("tw_restore_list", available);

	std::string selected;
	if (a.empty()) {
		// No explicit partitions: restore everything in the backup.
		selected = available;
	} else {
		for (const auto& raw : a) {
			std::string p = raw;
			if (!p.empty() && p[0] != '/')
				p = "/" + p;
			if (available.find(p + ";") == std::string::npos) {
				gui_print("fox: partition '%s' is not present in this backup\n", p.c_str());
				return 1;
			}
			selected += p + ";";
		}
	}
	DataManager::SetValue("tw_restore_selected", selected);

	if (!PartitionManager.Run_Restore(folder)) {
		gui_print("fox: restore failed\n");
		return 1;
	}
	gui_print("Restore complete\n");
	return 0;
}

int Fox_Fifo::Cmd_Decrypt(const Json::Value& args) {
	std::string password = arg_str(args, "password");
	std::string user = arg_str(args, "user");
	if (password.empty()) {
		gui_print("fox: decrypt requires a password\n");
		return 2;
	}
	int user_id = user.empty() ? 0 : atoi(user.c_str());

	gui_print("Attempting to decrypt data partition...\n");
	if (PartitionManager.Decrypt_Device(password, user_id) != 0) {
		gui_print("fox: decryption failed\n");
		return 1;
	}
	gui_print("Decryption successful\n");
	return 0;
}

int Fox_Fifo::Cmd_Sideload(const Json::Value& /*args*/) {
	// Reuse the tested ORS sideload handler.
	return OpenRecoveryScript::Run_ORS_Line("sideload");
}

static std::string fallback_property(const std::string& first, const std::string& second) {
	std::string value = TWFunc::Fox_Property_Get(first);
	if (value.empty())
		value = TWFunc::System_Property_Get(second);
	return value;
}

int Fox_Fifo::Cmd_Status(const Json::Value& /*args*/) {
	int is_encrypted = 0;
	int is_decrypted = 0;
	int fox_encrypted = 0;
	DataManager::GetValue(TW_IS_ENCRYPTED, is_encrypted);
	DataManager::GetValue(TW_IS_DECRYPTED, is_decrypted);
	DataManager::GetValue(FOX_ENCRYPTED_DEVICE, fox_encrypted);
	int storage_encrypted = (is_encrypted || fox_encrypted || PartitionManager.Storage_Is_Encrypted()) ? 1 : 0;

	std::string device_model = TWFunc::Fox_Property_Get("ro.orangefox.device_model");
	std::string device_code = TWFunc::Fox_Property_Get("ro.product.device");
	if (device_code.empty())
		device_code = TWFunc::Fox_Property_Get("ro.product.system.device");
	if (device_model.empty())
		device_model = device_code;

	std::string fingerprint = TWFunc::Fox_Property_Get("orangefox.system.fingerprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.system.build.fingerprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.build.fingerprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.build.thumbprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.vendor.build.thumbprint");

	std::string rom_name = TWFunc::System_Property_Get("ro.build.display.id");
	if (rom_name.empty())
		rom_name = TWFunc::System_Property_Get("ro.build.id");
	std::string rom_incremental = TWFunc::System_Property_Get("ro.build.version.incremental");
	std::string rom_sdk = fallback_property("orangefox.rom.sdk", "ro.build.version.sdk");
	std::string android_release = TWFunc::System_Property_Get("ro.build.version.release");

	Json::Value s(Json::objectValue);
	s["release"] = FOX_BUILD;
	s["recovery_version"] = TW_VERSION_STR;
	s["variant"] = FOX_VARIANT;
	s["build_type"] = FOX_BUILD_TYPE;
	s["branch"] = OF_CURRENT_BRANCH;
	s["codebase_sdk"] = TWFunc::Fox_Property_Get("ro.build.version.sdk");
	s["codebase"] = FOX_CURRENT_DEV_STR;
	s["build_date"] = DataManager::GetStrValue("FOX_BUILD_DATE_REAL");
	s["device_model"] = device_model;
	s["device_code"] = device_code;
	s["storage_encrypted"] = storage_encrypted != 0;
	s["storage_decrypted"] = is_decrypted != 0;
	s["dynamic_partitions"] = TWFunc::Fox_Property_Get("orangefox.super.partition") == "true";
	s["boot_slot"] = TWFunc::Fox_Property_Get("ro.boot.slot_suffix");
	s["virtual_ab"] = TWFunc::Fox_Property_Get("ro.virtual_ab.enabled") == "true";
	s["kernel"] = TWFunc::Fox_Property_Get("ro.orangefox.kernel");
	s["rom_name"] = rom_name;
	s["rom_incremental"] = rom_incremental;
	s["rom_sdk"] = rom_sdk;
	s["android_release"] = android_release;
	s["rom_miui"] = TWFunc::Fox_Property_Get("orangefox.miui.rom") == "1";
	s["fingerprint"] = fingerprint;
	Fox_Command_Dispatcher::EmitData("status", s);
	return 0;
}

int Fox_Fifo::Cmd_Reboot(const Json::Value& args) {
	std::string target = arg_str(args, "target");
	if (target.empty())
		target = "system";
	return OpenRecoveryScript::Run_ORS_Line("reboot " + target);
}


int Fox_Fifo::Cmd_Internal(const Json::Value& args) {
	std::string sub = arg_action(args);
	std::string name = arg_str(args, "name");
	if (sub.empty()) {
		gui_print("fox: internal requires a subcommand (get|set|go)\n");
		return 2;
	}
	if (sub == "get") {
		if (name.empty()) {
			gui_print("fox: internal get requires a variable name\n");
			return 2;
		}
		std::string value;
		DataManager::GetValue(name, value);
		gui_print("%s\n", value.c_str());
		return 0;
	}
	if (sub == "set") {
		if (name.empty()) {
			gui_print("fox: internal set requires a variable name\n");
			return 2;
		}
		DataManager::SetValue(name, arg_str(args, "value"));
		return 0;
	}
	if (sub == "go") {
		std::string page = arg_str(args, "page");
		if (page.empty()) {
			gui_print("fox: internal go requires a page name\n");
			return 2;
		}
		gui_changePage(page);
		return 0;
	}
	gui_print("fox: unknown internal subcommand '%s'\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Ors(const Json::Value& args) {
	std::string path = arg_str(args, "path");
	if (path.empty()) {
		gui_print("fox: ors requires a script file path\n");
		return 2;
	}
	return OpenRecoveryScript::Run_ORS_File(path);
}

int Fox_Fifo::Cmd_OrsCmd(const Json::Value& args) {
	std::vector<std::string> raw = arg_array(args, "raw");
	std::string joined;
	for (size_t i = 0; i < raw.size(); i++) {
		if (i)
			joined += " ";
		joined += raw[i];
	}
	if (joined.empty()) {
		gui_print("fox: ors-cmd requires a command line\n");
		return 2;
	}
	return OpenRecoveryScript::Run_ORS_Line(joined);
}

int Fox_Fifo::Cmd_Wipe(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "parts");
	if (a.empty()) {
		// No targets: show what can be wiped.
		Emit_Partition_List("wipe", false);
		return 0;
	}
	int rc = 0;
	for (const auto& raw : a) {
		std::string lower = raw;
		for (auto& c : lower) c = tolower(c);
		if (lower == "dalvik" || lower == "cache" || lower == "dalvik-cache") {
			if (!PartitionManager.Wipe_Dalvik_Cache())
				rc = 1;
			else
				gui_print("Wiped dalvik/cache\n");
			continue;
		}
		std::string p = normalize_partition_spec(raw);
		if (!PartitionManager.Find_Partition_By_Path(p)) {
			gui_print("fox: unknown partition '%s'\n", raw.c_str());
			rc = 1;
			continue;
		}
		if (!PartitionManager.Wipe_By_Path(p))
			rc = 1;
		else
			gui_print("Wiped %s\n", p.c_str());
	}
	return rc;
}

int Fox_Fifo::Cmd_Format_Data(const Json::Value& args) {
	bool confirmed = arg_bool(args, "confirm");
	if (!confirmed) {
		gui_print("fox: format_data erases ALL data and removes encryption.\n");
		gui_print("fox: refusing without an explicit confirm flag\n");
		return 2;
	}
	gui_print("Formatting data...\n");
	if (!PartitionManager.Format_Data()) {
		gui_print("fox: format data failed\n");
		return 1;
	}
	gui_print("Data formatted\n");
	return 0;
}

int Fox_Fifo::Cmd_Reflash(const Json::Value& /*args*/) {
	gui_print("Reflashing OrangeFox...\n");
	twrpRepacker repacker;
	if (!repacker.Flash_Current_Twrp()) {
		gui_print("fox: reflash failed\n");
		return 1;
	}
	gui_print("OrangeFox reflashed\n");
	return 0;
}

int Fox_Fifo::Cmd_Mtp(const Json::Value& args) {
	std::string sub = arg_action(args);
	if (sub.empty())
		sub = "status";
	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["enabled"] = DataManager::GetIntValue("tw_mtp_enabled") != 0;
		Fox_Command_Dispatcher::EmitData("mtp", s);
		return 0;
	}
	if (sub == "enable") {
		if (!PartitionManager.Enable_MTP()) {
			gui_print("fox: failed to enable MTP\n");
			return 1;
		}
		gui_print("MTP enabled\n");
		return 0;
	}
	if (sub == "disable") {
		if (!PartitionManager.Disable_MTP()) {
			gui_print("fox: failed to disable MTP\n");
			return 1;
		}
		gui_print("MTP disabled\n");
		return 0;
	}
	gui_print("fox: unknown mtp subcommand '%s' (status|enable|disable)\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Log(const Json::Value& /*args*/) {
	// Mirror the OrangeFox log export: copy the recovery log into the Fox logs
	// folder and compress it (pigz produces <name>.zip), as in Run_Before_Reboot.
	TWFunc::Create_Dir_Recursive(Fox_Logs_Dir, 0777);

	std::string log_file = Fox_Logs_Dir + "/recovery_" +
	                       std::to_string((long long)time(NULL)) + ".log";
	if (TWFunc::copy_file("/tmp/recovery.log", log_file, 0777) != 0) {
		gui_print("fox: unable to read /tmp/recovery.log\n");
		return 1;
	}

	std::string pigz = Fox_Bin_Dir + "/pigz";
	if (TWFunc::Path_Exists(pigz)) {
		TWFunc::Exec_Cmd(pigz + " -K --best " + log_file);
		std::string zip = log_file + ".zip";
		TWFunc::set_media_rw_permissions(zip);
		gui_print("Saved log to %s\n", zip.c_str());
	} else {
		TWFunc::set_media_rw_permissions(log_file);
		gui_print("Saved log to %s\n", log_file.c_str());
	}
	return 0;
}

int Fox_Fifo::Cmd_Partition(const Json::Value& args) {
	std::string path_in = arg_str(args, "path");
	std::string op = arg_action(args);
	if (path_in.empty()) {
		// No path: list partitions with file system and capability flags.
		Emit_Partition_List("part_option", true);
		return 0;
	}
	if (op.empty()) {
		gui_print("fox: partition requires <path> <repair|resize|change-fs <fs>|wipe>\n");
		return 2;
	}

	std::string path = normalize_partition_spec(path_in);
	if (!PartitionManager.Find_Partition_By_Path(path)) {
		gui_print("fox: unknown partition '%s'\n", path_in.c_str());
		return 1;
	}

	if (op == "repair") {
		if (!PartitionManager.Repair_By_Path(path, true))
			return 1;
		gui_print("Repaired %s\n", path.c_str());
		return 0;
	}
	if (op == "resize") {
		if (!PartitionManager.Resize_By_Path(path, true))
			return 1;
		gui_print("Resized %s\n", path.c_str());
		return 0;
	}
	if (op == "change-fs") {
		std::string fs = arg_str(args, "fs");
		if (fs.empty()) {
			gui_print("fox: partition change-fs requires a file system (e.g. ext4)\n");
			return 2;
		}
		if (!PartitionManager.Wipe_By_Path(path, fs))
			return 1;
		gui_print("Changed %s to %s\n", path.c_str(), fs.c_str());
		return 0;
	}
	if (op == "wipe") {
		if (!PartitionManager.Wipe_By_Path(path))
			return 1;
		gui_print("Wiped %s\n", path.c_str());
		return 0;
	}
	gui_print("fox: unknown partition op '%s' (repair|resize|change-fs|wipe)\n", op.c_str());
	return 2;
}

// Collect *.zip files in 'dir' into 'out' (full paths).
static void collect_zip_files(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		std::string name = ent->d_name;
		if (name == "." || name == "..")
			continue;
		std::string path = dir + "/" + name;
		if (name.size() > 4 && name.substr(name.size() - 4) == ".zip")
			out.push_back(path);
		else if (depth < 4)
			collect_zip_files(path, out, depth + 1);
	}
	closedir(d);
}

int Fox_Fifo::Cmd_Addons(const Json::Value& args) {
	std::vector<std::string> dirs = { Fox_Home_Files, FFiles_dir };
	std::string sub = arg_action(args);
	std::vector<std::string> names = arg_array(args, "names");

	if (sub.empty() || sub == "list") {
		std::vector<std::string> zips;
		for (const auto& dir : dirs)
			collect_zip_files(dir, zips);
		std::sort(zips.begin(), zips.end());
		zips.erase(std::unique(zips.begin(), zips.end()), zips.end());
		Json::Value items(Json::arrayValue);
		for (const auto& z : zips) {
			std::string name = z.substr(z.find_last_of('/') + 1);
			Json::Value item(Json::objectValue);
			// mount_point column carries the path; display the file name.
			item["mount_point"] = z;
			item["display_name"] = name;
			item["mounted"] = false;
			item["extra"] = "";
			items.append(item);
		}
		Json::Value payload(Json::objectValue);
		payload["items"] = items;
		Fox_Command_Dispatcher::EmitData("list", payload);
		return 0;
	}

	if (sub == "install") {
		if (names.empty()) {
			gui_print("fox: addons install requires a zip name or path\n");
			return 2;
		}
		int rc = 0;
		std::vector<std::string> zips;
		for (const auto& dir : dirs)
			collect_zip_files(dir, zips);
		std::sort(zips.begin(), zips.end());
		zips.erase(std::unique(zips.begin(), zips.end()), zips.end());
		for (const std::string& requested : names) {
			std::string target = requested;
			if (target.find('/') == std::string::npos) {
				// Resolve a bare name against the known addon folders.
				bool found = false;
				std::string wanted = target;
				if (wanted.size() <= 4 || wanted.substr(wanted.size() - 4) != ".zip")
					wanted += ".zip";
				for (const auto& zip : zips) {
					std::string basename = zip.substr(zip.find_last_of('/') + 1);
					if (basename == wanted || basename == target) {
						target = zip;
						found = true;
						break;
					}
				}
				if (!found) {
					gui_print("fox: addon '%s' not found\n", requested.c_str());
					rc = 1;
					continue;
				}
			}
			int wipe_cache = 0;
			if (TWinstall_zip(target.c_str(), &wipe_cache) != 0)
				rc = 1;
		}
		return rc;
	}

	gui_print("fox: unknown addons subcommand '%s' (list|install)\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Screencap(const Json::Value& args) {
	// Capture the live framebuffer to a PNG file (tmpfs by default) so a client
	// can read it back over an ADB sync/pull -- no network stack involved.
	std::string path = arg_str(args, "path");
	if (path.empty())
		path = "/tmp/.fox_screen.png";
	if (gr_save_screenshot(path.c_str()) != 0) {
		gui_print("fox: screencap failed\n");
		return 1;
	}
	gui_print("%s\n", path.c_str());
	return 0;
}

int Fox_Fifo::Cmd_Input(const Json::Value& args) {
	// Normally intercepted by the dispatcher's control fast path; handled here
	// too for completeness. Fox_Remote_Input reads the typed args directly.
	std::string output;
	int code = Fox_Remote_Input::RunCommand(args, &output);
	if (!output.empty())
		gui_print("%s", output.c_str());
	return code;
}

int Fox_Fifo::Cmd_Storages(const Json::Value& /*args*/) {
	PartitionManager.Mount_All_Storage();
	std::vector<PartitionList> storage_list;
	PartitionManager.Get_Partition_List("storage", &storage_list);
	std::string current = DataManager::GetCurrentStoragePath();

	Json::Value items(Json::arrayValue);
	for (const auto& storage : storage_list) {
		Json::Value item(Json::objectValue);
		item["mount_point"] = storage.Mount_Point;
		item["display_name"] = storage.Display_Name;
		item["mounted"] = storage.Mount_Point == current;  // "selected" storage
		item["extra"] = storage.isFiles ? "files" : "block";
		items.append(item);
	}
	Json::Value payload(Json::objectValue);
	payload["items"] = items;
	Fox_Command_Dispatcher::EmitData("list", payload);
	return 0;
}


int Fox_Fifo::Run_Command() {
	if (op.empty()) {
		gui_print("fox: no command given\n");
		return 2;
	}

	// Dispatch straight off the typed request: the op selects the handler, which
	// reads its parameters from the args object (no argv round-trip).
	const Json::Value& a = cmd_args;

	if (op == "mount")
		return Cmd_Mount(a, false);
	if (op == "unmount" || op == "umount")
		return Cmd_Mount(a, true);
	if (op == "flash" || op == "install")
		return Cmd_Flash(a);
	if (op == "backup")
		return Cmd_Backup(a);
	if (op == "restore")
		return Cmd_Restore(a);
	if (op == "decrypt")
		return Cmd_Decrypt(a);
	if (op == "sideload")
		return Cmd_Sideload(a);
	if (op == "status")
		return Cmd_Status(a);
	if (op == "reboot")
		return Cmd_Reboot(a);
	if (op == "internal")
		return Cmd_Internal(a);
	if (op == "wipe")
		return Cmd_Wipe(a);
	if (op == "format_data")
		return Cmd_Format_Data(a);
	if (op == "reflash")
		return Cmd_Reflash(a);
	if (op == "mtp")
		return Cmd_Mtp(a);
	if (op == "log")
		return Cmd_Log(a);
	if (op == "partition")
		return Cmd_Partition(a);
	if (op == "addons")
		return Cmd_Addons(a);
	if (op == "storages")
		return Cmd_Storages(a);
	if (op == "input")
		return Cmd_Input(a);
	if (op == "screencap")
		return Cmd_Screencap(a);
	if (op == "ors")
		return Cmd_Ors(a);
	if (op == "ors-cmd")
		return Cmd_OrsCmd(a);

	gui_print("fox: unknown command '%s'\n", op.c_str());
	return 2;
}
