/*
	Copyright 2012 to 2017 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	Copyright (C) 2018-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>
#include <iostream>
#include <fstream>

#include <string.h>
#include <stdio.h>
#include <cutils/properties.h>

#include <android-base/unique_fd.h>
#include <map>
#include <set>
#include <sys/statvfs.h>
#include <update_engine/update_metadata.pb.h>

#include "twcommon.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"

#include "otautil/sysutil.h"
#include <ziparchive/zip_archive.h>
#include "twinstall/install.h"
#include "twinstall/verifier.h"
#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrpDigestDriver.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrp-functions.hpp"
#include "orangefox.hpp"
#include "gui/gui.hpp"
#include "gui/pages.hpp"
#include "gui/blanktimer.hpp"
#include "twinstall.h"
#include "installcommand.h"
#include "../twrpRepacker.hpp"
extern "C" {
	#include "gui/gui.h"
}

#define AB_OTA "payload_properties.txt"

enum zip_type {
	UNKNOWN_ZIP_TYPE = 0,
	UPDATE_BINARY_ZIP_TYPE,
	AB_OTA_ZIP_TYPE,
	TWRP_THEME_ZIP_TYPE
};

static int Install_Theme(const char* path, ZipArchiveHandle Zip) {
#ifdef TW_OEM_BUILD // We don't do custom themes in OEM builds
	return INSTALL_CORRUPT;
#else
	std::string binary_name("ui.xml");
	ZipEntry64 binary_entry;
	if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
		return INSTALL_CORRUPT;
	}
	if (!PartitionManager.Mount_Settings_Storage(true))
		return INSTALL_ERROR;
	std::string theme_path = DataManager::GetCurrentStoragePath() + "/theme";
	if (!TWFunc::Path_Exists(theme_path)) {
		if (!TWFunc::Recursive_Mkdir(theme_path)) {
			return INSTALL_ERROR;
		}
	}
	theme_path += "/ui.zip";
	if (TWFunc::copy_file(path, theme_path, 0644) != 0) {
		return INSTALL_ERROR;
	}
	LOGINFO("Installing custom theme '%s' to '%s'\n", path, theme_path.c_str());
	PageManager::RequestReload();
	return INSTALL_SUCCESS;
#endif
}

static int Prepare_Update_Binary(const char *path, ZipArchiveHandle Zip) {
	if (TWFunc::Block_Operations_Until_Reboot())
		return INSTALL_ERROR;

	char arches[PATH_MAX];
	property_get("ro.product.cpu.abilist", arches, "error");
	if (strcmp(arches, "error") == 0)
		property_get("ro.product.cpu.abi", arches, "error");
	vector<string> split = TWFunc::split_string(arches, ',', true);
	std::vector<string>::iterator arch;
	std::string base_name = UPDATE_BINARY_NAME;
	base_name += "-";
	ZipEntry64 binary_entry;
	std::string update_binary_string(UPDATE_BINARY_NAME);
	if (FindEntry(Zip, update_binary_string, &binary_entry) != 0) {
		for (arch = split.begin(); arch != split.end(); arch++) {
			std::string temp = base_name + *arch;
			std::string binary_name(temp.c_str());
			if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
				std::string binary_name(temp.c_str());
				break;
			}
		}
	}
	LOGINFO("Extracting updater binary '%s'\n", UPDATE_BINARY_NAME);
	unlink(TMP_UPDATER_BINARY_PATH);
	android::base::unique_fd fd(
		open(TMP_UPDATER_BINARY_PATH, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755));
	if (fd == -1) {
		return INSTALL_ERROR;
	}
	int32_t err = ExtractEntryToFile(Zip, &binary_entry, fd);
	if (err != 0) {
		LOGERR("Could not extract '%s'\n", UPDATE_BINARY_NAME);
		return INSTALL_ERROR;
	}

	// -------------- OrangeFox: start ---------------- //
	int Fox_Ret = Fox_Prepare_Update_Binary(path, Zip);
	if (Fox_Ret != INSTALL_SUCCESS) {
	   return Fox_Ret;
	}
	// -------------- OrangeFox: end ---------------- //

	// If exists, extract file_contexts from the zip file
	std::string file_contexts("file_contexts");
	ZipEntry64 file_contexts_entry;
	if (FindEntry(Zip, file_contexts, &file_contexts_entry) != 0) {
		LOGINFO("Zip does not contain SELinux file_contexts file in its root.\n");
	} else {
		const string output_filename = "/file_contexts";
		LOGINFO("Zip contains SELinux file_contexts file in its root. Extracting to %s\n", output_filename.c_str());
		android::base::unique_fd fd(
			open(output_filename.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
		if (fd == -1) {
			return INSTALL_ERROR;
		}
		if (ExtractEntryToFile(Zip, &file_contexts_entry, fd)) {
			LOGERR("Could not extract '%s'\n", output_filename.c_str());
			return INSTALL_ERROR;
		}
	}
	return INSTALL_SUCCESS;
}


static int Run_Update_Binary(const char *path, int* wipe_cache, zip_type ztype) {
	int ret_val, pipe_fd[2], status, zip_verify;
	int aroma_running = 0;
	char buffer[1024];
	FILE* child_data;
	pipe(pipe_fd);

	std::vector<std::string> args;
    if (ztype == UPDATE_BINARY_ZIP_TYPE) {
		ret_val = update_binary_command(path, 0, pipe_fd[1], &args);
    } else if (ztype == AB_OTA_ZIP_TYPE) {
		ret_val = abupdate_binary_command(path, 0, pipe_fd[1], &args);
	} else {
		LOGERR("Unknown zip type %i\n", ztype);
		ret_val = INSTALL_CORRUPT;
	}
    if (ret_val) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return ret_val;
    }

	// Convert the vector to a NULL-terminated char* array suitable for execv.
	const char* chr_args[args.size() + 1];
	chr_args[args.size()] = NULL;
	for (size_t i = 0; i < args.size(); i++)
		chr_args[i] = args[i].c_str();

	pid_t pid = fork();
	if (pid == 0) {
		close(pipe_fd[0]);
		execve(chr_args[0], const_cast<char**>(chr_args), environ);
		printf("E:Can't execute '%s': %s\n", chr_args[0], strerror(errno));
		_exit(-1);
	}
	close(pipe_fd[1]);

	*wipe_cache = 0;

	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
	child_data = fdopen(pipe_fd[0], "r");
	while (fgets(buffer, sizeof(buffer), child_data) != NULL) {
		char* command = strtok(buffer, " \n");
		if (command == NULL) {
			continue;
		} else if (strcmp(command, "progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			char* seconds_char = strtok(NULL, " \n");

			float fraction_float = strtof(fraction_char, NULL);
			int seconds_float = strtol(seconds_char, NULL, 10);

			if (zip_verify)
				DataManager::ShowProgress(fraction_float * (1 - VERIFICATION_PROGRESS_FRACTION), seconds_float);
			else
				DataManager::ShowProgress(fraction_float, seconds_float);
		} else if (strcmp(command, "set_progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			float fraction_float = strtof(fraction_char, NULL);
			DataManager::_SetProgress(fraction_float);
		} else if (strcmp(command, "ui_print") == 0) {
			char* display_value = strtok(NULL, "\n");
	  		if (display_value) {
	      		     if (strcmp(display_value, "AROMA Filemanager Finished...") == 0 && (aroma_running == 1)) {
		  		aroma_running = 0;
		  		gui_changeOverlay("");
		  		TWFunc::copy_file(Fox_aroma_cfg, Fox_sdcard_aroma_cfg, 0644);
			     }
	      		    gui_print("%s", display_value);
	      		    if (strcmp(display_value, "(c) 2013-2015 by amarullz.com") == 0 && (aroma_running == 0)) {
		  		aroma_running = 1;
		  		gui_changeOverlay("black_out");
		  		TWFunc::copy_file(Fox_aroma_cfg, Fox_sdcard_aroma_cfg, 0644);
			     }
	    		}
	  		else {
	      			gui_print("\n");
	    		}
		} else if (strcmp(command, "wipe_cache") == 0) {
			*wipe_cache = 1;
		} else if (strcmp(command, "clear_display") == 0) {
			// Do nothing, not supported by TWRP
		} else if (strcmp(command, "log") == 0) {
			printf("%s\n", strtok(NULL, "\n"));
		} else {
			LOGERR("unknown command [%s]\n", command);
		}
	}
	fclose(child_data);

	int waitrc = TWFunc::Wait_For_Child(pid, &status, "Updater");

  	// Should never happen, but in case of crash or other unexpected condition
  	if (aroma_running == 1) {
      		gui_changeOverlay("");
    	}

  	// if updater-script doesn't find the correct device
  	if (WEXITSTATUS (status) == TW_ERROR_WRONG_DEVICE) {
       		gui_print_color("error", "\nPossible causes of this error:\n  1. Wrong device\n  2. Wrong firmware\n  3. Corrupt zip\n  4. System not mounted\n  5. Bugged updater-script.\n\nSearch online for \"error %i\". ",
       			TW_ERROR_WRONG_DEVICE);
       		gui_print_color("error", "Check \"/tmp/recovery.log\", and look above, for the specific cause of this error.\n\n");
     	}

  	if (waitrc != 0) {
      		set_miui_install_status(OTA_CORRUPT, false);
      		return INSTALL_ERROR;
    	}

	return INSTALL_SUCCESS;
}


// ---------------------------------------------------------------------
// high-speed A/B flash
//
// The normal AB install path hands the payload to update_engine, which
// decompresses and hashes every operation one at a time on a single
// thread -- that's the slow part, not the disk write. This path pulls the
// partitions out in parallel with payload-dumper-go first, then flashes
// the finished images through the same Flash_Image() code manual image
// flashing already uses, and finally flips the active slot by hand since
// update_engine never ran to do it for us.
//
// This only ever runs when the user explicitly turned it on for this zip,
// and it only ever touches system/vendor/product/boot-type partitions --
// firmware (bootloader, modem, trustzone...) is never written by this
// path, so a failed write here can't leave the device unable to reach
// fastboot. If anything goes wrong, the install just stops -- there's no
// silent fallback to the normal path once a partition may have been
// written, because at that point falling back could mean two different
// code paths both trying to modify the same slot.
// ---------------------------------------------------------------------

namespace {

const char* kHighSpeedDumperPath = "/system/bin/payload-dumper-static";
const char* kHighSpeedScratchDir = "/data/hsf_extract";

// how much extra free space to require on top of what the partitions
// actually need, so extraction doesn't run /data down to zero -- 5% or
// 256MB, whichever is bigger
const uint64_t kHighSpeedStorageMarginMin = 256ULL * 1024 * 1024;

const size_t kPayloadMagicSize = 4;
const size_t kPayloadVersionSize = 8;
const size_t kPayloadManifestSizeSize = 8;
const size_t kPayloadSignatureSizeSize = 4;

// firmware and bootloader-ish partitions -- never flashed by this path,
// on purpose. flash these the normal way, separately.
const std::set<std::string>& HighSpeedExcludedPartitions() {
	static const std::set<std::string> excluded = {
		"recovery", "abl", "aop", "aop_config", "bluetooth", "countrycode", "cpucp",
		"cpucp_dtb", "devcfg", "dsp", "featenabler", "hyp", "imagefv",
		"keymaster", "modem", "modemfirmware", "multiimgqti", "qupfw",
		"shrm", "tz", "uefi", "uefisecapp", "xbl", "xbl_config", "xbl_ramdump",
	};
	return excluded;
}

struct HighSpeedPartitionPlan {
	std::string name;          // bare partition name from the payload manifest
	std::string target_path;   // the TWRP path Find_Partition_By_Path() expects
	uint64_t expanded_size = 0; // decompressed size, straight from the manifest
};

uint64_t ReadBigEndian64(const uint8_t* p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

// this is a plain lookup table, not something derived automatically --
// the payload only knows partition group names, it has no idea what path
// TWRP mounts them under, so this has to be kept in sync by hand with
// whatever partitions your builds actually ship
std::string ResolveTargetPath(const std::string& bare_name) {
	static const std::map<std::string, std::string> physical = {
		{"boot", "/boot"}, {"dtbo", "/dtbo"}, {"init_boot", "/init_boot"},
		{"vbmeta", "/vbmeta"}, {"vbmeta_system", "/vbmeta_system"},
		{"vendor_boot", "/vendor_boot"},
	};
	static const std::map<std::string, std::string> logical = {
		{"odm", "/odm"}, {"product", "/product"}, {"system", "/system"},
		{"system_dlkm", "/system_dlkm"}, {"system_ext", "/system_ext"},
		{"vendor", "/vendor"}, {"vendor_dlkm", "/vendor_dlkm"},
	};
	auto p = physical.find(bare_name);
	if (p != physical.end())
		return p->second;
	auto l = logical.find(bare_name);
	if (l != logical.end())
		return l->second;
	return "";
}

// the currently running recovery IS OrangeFox, and we never touch the
// booted slot's recovery partition -- so once the ROM partitions are
// done, just copy the running recovery straight onto the other slot.
bool ReflashSelfToOtherSlot(const std::string& source_slot, const std::string& target_slot) {
	std::string source_suffix = (source_slot == "A") ? "_a" : "_b";
	std::string target_suffix = (target_slot == "A") ? "_a" : "_b";
	std::string source_path = "/dev/block/bootdevice/by-name/recovery" + source_suffix;
	std::string target_path = "/dev/block/bootdevice/by-name/recovery" + target_suffix;
	std::string cmd = "dd if='" + source_path + "' of='" + target_path + "' bs=1M";
	if (TWFunc::Exec_Cmd(cmd) != 0) {
		LOGERR("high-speed: failed copying OrangeFox from '%s' to '%s'\n",
			source_path.c_str(), target_path.c_str());
		return false;
	}
	sync();
	LOGINFO("high-speed: copied OrangeFox onto the other slot's recovery ('%s' -> '%s')\n",
		source_path.c_str(), target_path.c_str());
	return true;
}

// payload-dumper-go can't decode these op types yet, so we check for them
// up front instead of finding out partway through extraction
bool OpTypeIsSupported(chromeos_update_engine::InstallOperation::Type type) {
	using Op = chromeos_update_engine::InstallOperation;
	switch (type) {
		case Op::PUFFDIFF:
		case Op::ZUCCHINI:
		case Op::LZ4DIFF_BSDIFF:
		case Op::LZ4DIFF_PUFFDIFF:
			return false;
		default:
			return true;
	}
}

// reads payload.bin's header straight out of the zip and parses the
// manifest, so we know which partitions exist and whether anything in
// there is a format we can't handle -- before touching disk at all
bool ProbeHighSpeedPayload(const std::string& package_path, ZipArchiveHandle zip,
                            std::vector<HighSpeedPartitionPlan>* plan) {
	plan->clear();

	ZipEntry64 payload_entry;
	if (FindEntry(zip, "payload.bin", &payload_entry) != 0) {
		LOGERR("high-speed probe: payload.bin not found in zip\n");
		return false;
	}

	// we read payload.bin's bytes straight off disk at its zip offset
	// below, which only lines up correctly if the entry is stored
	// uncompressed inside the zip -- most OTA tooling does this already,
	// but if it isn't, bail rather than read garbage
	if (payload_entry.method != 0) {
		LOGERR("high-speed probe: payload.bin is compressed inside the zip, can't read it directly\n");
		return false;
	}

	size_t to_read = std::min<uint64_t>(payload_entry.uncompressed_length, 4 * 1024 * 1024);
	std::vector<uint8_t> buf(to_read);

	android::base::unique_fd fd(open(package_path.c_str(), O_RDONLY | O_CLOEXEC));
	if (fd == -1) {
		LOGERR("high-speed probe: couldn't open the zip file\n");
		return false;
	}

	size_t got = 0;
	while (got < to_read) {
		ssize_t r = pread(fd.get(), buf.data() + got, to_read - got, payload_entry.offset + got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			LOGERR("high-speed probe: read failed (%s)\n", strerror(errno));
			return false;
		}
		if (r == 0) {
			LOGERR("high-speed probe: hit end of file early while reading payload.bin\n");
			return false;
		}
		got += r;
	}

	size_t off = 0;
	if (to_read < kPayloadMagicSize + kPayloadVersionSize + kPayloadManifestSizeSize ||
	    memcmp(buf.data(), "CrAU", kPayloadMagicSize) != 0) {
		LOGERR("high-speed probe: payload.bin doesn't start with the expected magic\n");
		return false;
	}
	off += kPayloadMagicSize;

	uint64_t version = ReadBigEndian64(buf.data() + off);
	off += kPayloadVersionSize;
	if (version != 2) {
		LOGERR("high-speed probe: payload version %llu isn't one we understand\n",
			(unsigned long long)version);
		return false;
	}

	uint64_t manifest_size = ReadBigEndian64(buf.data() + off);
	off += kPayloadManifestSizeSize;

	if (off + kPayloadSignatureSizeSize > to_read) {
		LOGERR("high-speed probe: payload header is truncated\n");
		return false;
	}
	off += kPayloadSignatureSizeSize;

	if (off + manifest_size > to_read) {
		// the manifest is bigger than our first read grabbed -- read again
		// with a buffer sized for the whole thing
		to_read = off + manifest_size;
		buf.assign(to_read, 0);
		got = 0;
		while (got < to_read) {
			ssize_t r = pread(fd.get(), buf.data() + got, to_read - got, payload_entry.offset + got);
			if (r < 0) {
				if (errno == EINTR)
					continue;
				LOGERR("high-speed probe: read failed while re-reading the full manifest (%s)\n", strerror(errno));
				return false;
			}
			if (r == 0) {
				LOGERR("high-speed probe: hit end of file early while re-reading the manifest\n");
				return false;
			}
			got += r;
		}
	}

	chromeos_update_engine::DeltaArchiveManifest manifest;
	if (!manifest.ParseFromArray(buf.data() + off, manifest_size)) {
		LOGERR("high-speed probe: couldn't parse the payload manifest\n");
		return false;
	}

	if (manifest.minor_version() != 0) {
		// a nonzero minor version means this is a delta payload, which
		// references an old partition we don't have lying around in the
		// shape this path expects -- full payloads only here
		LOGERR("high-speed probe: this is a delta payload, not a full one -- not supported here\n");
		return false;
	}

	for (const auto& part : manifest.partitions()) {
		if (HighSpeedExcludedPartitions().count(part.partition_name())) {
			LOGINFO("high-speed probe: skipping firmware partition '%s', flash it separately\n",
				part.partition_name().c_str());
			continue;
		}

		for (const auto& op : part.operations()) {
			if (!OpTypeIsSupported(op.type())) {
				LOGERR("high-speed probe: partition '%s' uses an operation type we can't decode\n",
					part.partition_name().c_str());
				return false;
			}
		}

		HighSpeedPartitionPlan entry;
		entry.name = part.partition_name();
		entry.target_path = ResolveTargetPath(entry.name);
		entry.expanded_size = part.new_partition_info().size();
		if (entry.target_path.empty()) {
			LOGERR("high-speed probe: don't know where partition '%s' should be flashed, stopping rather than guess\n",
				entry.name.c_str());
			return false;
		}
		plan->push_back(std::move(entry));
	}

	return !plan->empty();
}

}  // namespace

enum class HighSpeedResult {
	kNotApplicable,  // the toggle was off, or this isn't an A/B payload zip -- run the normal path
	kAborted,        // toggle was on and something went wrong -- stop, don't fall back
	kSucceeded,
};

// deletes the scratch extraction folder whenever this goes out of scope,
// win or lose, so a multi-gigabyte pile of extracted images never sits
// around on /data after the install finishes or fails
struct HighSpeedScratchGuard {
	~HighSpeedScratchGuard() { TWFunc::removeDir(kHighSpeedScratchDir, false); }
};

// Flash_Image() writes to whatever slot is currently "active" as far as
// TWPartitionManager is concerned, and that's the slot recovery itself
// booted from. We need to write the *other* slot, so this overrides which
// slot Flash_Image() targets for as long as we're flashing, and always
// puts it back afterward -- including if something throws or we return
// early, since it cleans up in its destructor either way.
struct HighSpeedSlotOverride {
	std::string original;
	bool active = false;
	void Enter(const std::string& target) {
		original = PartitionManager.Get_Active_Slot_Display();
		PartitionManager.Override_Active_Slot(target);
		active = true;
	}
	void Restore() {
		if (!active)
			return;
		PartitionManager.Override_Active_Slot(original);
		active = false;
	}
	~HighSpeedSlotOverride() { Restore(); }
};

HighSpeedResult TryHighSpeedAbInstall(const std::string& package, ZipArchiveHandle zip) {
	if (!DataManager::GetIntValue("tw_high_speed_flash"))
		return HighSpeedResult::kNotApplicable;

	ZipEntry64 unused;
	if (FindEntry(zip, "payload_properties.txt", &unused) != 0)
		return HighSpeedResult::kNotApplicable;

	// from here on the toggle is on and this really is an A/B payload zip,
	// so every way out below is kAborted -- we never quietly drop back to
	// the normal path once we've gotten this far
	if (access(kHighSpeedDumperPath, X_OK) != 0) {
		gui_err("hs_dumper_missing=High-speed flash is turned on but the dumper tool isn't on this build. Stopping.");
		return HighSpeedResult::kAborted;
	}

	gui_print("High-speed flash: reading the payload...\n");
	std::vector<HighSpeedPartitionPlan> plan;
	if (!ProbeHighSpeedPayload(package, zip, &plan)) {
		gui_err("hs_probe_failed=This package doesn't work with high-speed flash (delta payload or an operation type we can't decode). Turn off high-speed flash for this package and try again.");
		return HighSpeedResult::kAborted;
	}

	// if a normal install got interrupted before, libsnapshot can leave
	// state behind here that makes Ensure_Logical_Partition_Writable's
	// "is an update in progress" check refuse to touch anything -- clear
	// it the same way a fresh update_engine run always does at the start
	PartitionManager.Mount_By_Path("/metadata", false);
	if (TWFunc::Path_Exists("/metadata/ota"))
		TWFunc::removeDir("/metadata/ota", false);

	// only bother cleaning up if something's actually there -- removeDir
	// on a path that doesn't exist yet just prints a scary-looking "no
	// such file" line for no reason
	if (access(kHighSpeedScratchDir, F_OK) == 0)
		TWFunc::removeDir(kHighSpeedScratchDir, false);
	if (mkdir(kHighSpeedScratchDir, 0700) != 0 && errno != EEXIST) {
		gui_err("hs_scratch_mkdir_failed=Couldn't create the scratch folder for high-speed extraction.");
		return HighSpeedResult::kAborted;
	}
	HighSpeedScratchGuard scratch_guard;

	uint64_t required_bytes = 0;
	std::vector<std::string> names;
	{
		// pass the dumper the biggest partitions first -- it starts them
		// in the order we give it, and the biggest one is what decides
		// how long the whole extraction takes, so it shouldn't be sitting
		// queued behind a bunch of small ones
		std::vector<const HighSpeedPartitionPlan*> by_size;
		for (auto& part : plan) {
			required_bytes += part.expanded_size;
			by_size.push_back(&part);
		}
		std::stable_sort(by_size.begin(), by_size.end(),
			[](const HighSpeedPartitionPlan* a, const HighSpeedPartitionPlan* b) {
				return a->expanded_size > b->expanded_size;
			});
		for (auto* part : by_size)
			names.push_back(part->name);
	}

	uint64_t margin = std::max<uint64_t>(kHighSpeedStorageMarginMin, required_bytes / 20);
	uint64_t needed_bytes = required_bytes + margin;

	struct statvfs vfs {};
	if (statvfs(kHighSpeedScratchDir, &vfs) != 0) {
		gui_err("hs_statvfs_failed=Couldn't check free space for high-speed flash. Stopping.");
		return HighSpeedResult::kAborted;
	}
	uint64_t available_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
	if (available_bytes < needed_bytes) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			"hs_low_storage=Not enough free space for high-speed flash: need about %llu MB, only %llu MB free. Free up space or turn off high-speed flash.",
			(unsigned long long)(needed_bytes / (1024 * 1024)),
			(unsigned long long)(available_bytes / (1024 * 1024)));
		gui_err(msg);
		return HighSpeedResult::kAborted;
	}

	std::string joined_names;
	for (size_t i = 0; i < names.size(); i++) {
		if (i > 0)
			joined_names += ",";
		joined_names += names[i];
	}

	gui_print("High-speed flash: extracting partitions, this takes some seconds...\n");

	time_t extract_start, extract_stop;
	time(&extract_start);

	const char* dumper_args[] = {
		kHighSpeedDumperPath,
		"-c", "8",
		"-p", joined_names.c_str(),
		"-o", kHighSpeedScratchDir,
		package.c_str(),
		nullptr
	};

	int status = 0;
	pid_t pid = fork();
	if (pid == 0) {
		// silence the dumper's normal progress output, but leave stderr
		// alone -- it inherits recovery's own stderr, which goes to
		// recovery.log, so if the dumper crashes or hits a fatal error we
		// actually see why instead of just getting a bare nonzero exit code
		int null_fd = open("/dev/null", O_WRONLY);
		if (null_fd != -1) {
			dup2(null_fd, STDOUT_FILENO);
			close(null_fd);
		}
		execv(dumper_args[0], const_cast<char**>(dumper_args));
		_exit(127);
	} else if (pid < 0) {
		gui_err("hs_dumper_launch_failed=Couldn't start the high-speed extractor. Stopping.");
		return HighSpeedResult::kAborted;
	}
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		gui_err("hs_extraction_failed=High-speed extraction failed, or a checksum didn't match. No partitions were touched.");
		return HighSpeedResult::kAborted;
	}

	time(&extract_stop);
	int extract_secs = (int) difftime(extract_stop, extract_start);
	gui_print("High-speed flash: extraction done in %ds, flashing the inactive slot...\n", extract_secs);

	std::string original_slot = PartitionManager.Get_Active_Slot_Display();
	std::string target_slot = (original_slot == "A") ? "B" : "A";

	HighSpeedSlotOverride slot_override;
	slot_override.Enter(target_slot);

	// Flash_Image(directory, filename) doesn't take the target partition
	// as an argument -- it reads it from tw_flash_partition, the same way
	// the normal manual "flash image" screen sets it up
	DataManager::SetValue("tw_flash_both_slots", 0);
	bool flash_ok = true;
	std::string failed_part;
	for (auto& part : plan) {
		std::string directory = kHighSpeedScratchDir;
		std::string filename = part.name + ".img";
		DataManager::SetValue("tw_flash_partition", part.target_path + ";");
		DataManager::SetValue("tw_partition", part.target_path);
		if (!PartitionManager.Flash_Image(directory, filename)) {
			flash_ok = false;
			failed_part = part.name;
			break;
		}
	}
	DataManager::SetValue("tw_flash_partition", "");
	slot_override.Restore();  // back to the booted slot before we touch the bootloader's active-slot setting

	if (!flash_ok) {
		gui_err(("hs_flash_failed=High-speed flash failed writing '" + failed_part +
			"'. The slot you booted from wasn't touched and the active slot wasn't changed, "
			"You Can Either Flash Using Normal Method, Or Reboot To Previous System Normally.").c_str());
		return HighSpeedResult::kAborted;
	}

	if (!ReflashSelfToOtherSlot(original_slot, target_slot))
		gui_warn("hs_recovery_copy_failed=High-speed flash: couldn't copy OrangeFox onto the other slot's recovery. "
		         "You may need to flash it there manually before switching to that slot.");

	// update_engine never ran, so nothing told the bootloader to switch
	// slots -- that's the one piece of its job we still have to do
	// ourselves, and only now that every partition has actually succeeded
	PartitionManager.Set_Active_Slot(target_slot);
	if (PartitionManager.Get_Active_Slot_Display() != target_slot) {
		gui_err(("hs_slot_switch_failed=Every partition was written and verified, but switching to slot " + target_slot +
			" didn't take. Switch to it manually from the slot menu before rebooting.").c_str());
		return HighSpeedResult::kAborted;
	}

	if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false)) {
		PartitionManager.Unlock_Block_Partitions();
		PartitionManager.Prepare_All_Super_Volumes();
		gui_warn("mount_vab_partitions=Devices on super may not mount until after rebooting recovery.");
	}

        gui_print("1 min flash is cool right, it's done (^-^), now reboot to recovery again\n\n");
	return HighSpeedResult::kSucceeded;
}
// ---------------------------------------------------------------------

int TWinstall_zip(const char *path, int *wipe_cache, bool check_for_digest)
{
  int ret_val, zip_verify = 1, unmount_system = 1, reflashtwrp = 0, unmount_vendor = 1;
  bool run_rom_scripts = false;

  if (TWFunc::Block_Operations_Until_Reboot())
	return INSTALL_CORRUPT;

  if (strcmp(path, "error") == 0)
    {
      LOGERR("Failed to get adb sideload file: '%s'\n", path);
      return INSTALL_CORRUPT;
    }

  if (DataManager::GetIntValue(FOX_INSTALL_PREBUILT_ZIP) == 1)
     {
         DataManager::SetValue(FOX_ZIP_INSTALLER_CODE, 0); // internal zip = standard zip installer
         DataManager::SetValue(FOX_ZIP_INSTALLER_TREBLE, 0);
     }
  else
    {
	gui_msg(Msg("installing_zip=Installing zip file '{1}'")(path));
	if (strlen(path) < 9 || strncmp(path, "/sideload", 9) != 0) {
		string digest_str;
		string Full_Filename = path;

		if (check_for_digest) {
			gui_msg("check_for_digest=Checking for Digest file...");
			if (*path != '@' && !twrpDigestDriver::Check_File_Digest(Full_Filename)) {
				LOGERR("Aborting zip install: Digest verification failed\n");
				return INSTALL_CORRUPT;
			}
		}
	}
    }

  DataManager::GetValue(TW_UNMOUNT_SYSTEM, unmount_system);
  DataManager::GetValue(TW_UNMOUNT_VENDOR, unmount_vendor);

#ifndef TW_OEM_BUILD
  DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
#endif

  DataManager::SetProgress(0);

	auto package = Package::CreateMemoryPackage(path);
	if (!package) {
		return INSTALL_CORRUPT;
	}

	if (zip_verify) {
		gui_msg("verify_zip_sig=Verifying zip signature...");
		static constexpr const char* CERTIFICATE_ZIP_FILE = "/system/etc/security/otacerts.zip";
		std::vector<Certificate> loaded_keys = LoadKeysFromZipfile(CERTIFICATE_ZIP_FILE);
		if (loaded_keys.empty()) {
			LOGERR("Failed to load keys\n");
			return -1;
		}
		LOGINFO("%zu key(s) loaded from %s\n", loaded_keys.size(), CERTIFICATE_ZIP_FILE);

		ret_val = verify_file(package.get(), loaded_keys, std::bind(&DataManager::SetProgress, std::placeholders::_1));
		if (ret_val != VERIFY_SUCCESS) {
			LOGINFO("Zip signature verification failed: %i\n", ret_val);
			gui_err("verify_zip_fail=Zip signature verification failed!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			return -1;
		} else {
			gui_msg("verify_zip_done=Zip signature verified successfully.");
		}
    }
    
    ZipArchiveHandle Zip = package->GetZipArchiveHandle();
    if (!Zip) {
      set_miui_install_status(OTA_CORRUPT, true);
      gui_err("zip_corrupt=Zip file is corrupt!");
      return INSTALL_CORRUPT;
    }

    if (unmount_system) {
	if (PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path())) {
		gui_msg("unmount_system=Unmounting System...");
		if (PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false)) {
			//unlink(PartitionManager.Get_Android_Root_Path().c_str());
			//mkdir(PartitionManager.Get_Android_Root_Path().c_str(), 0755);
		}
		else {
			gui_msg("unmount_system_err=Failed to unmount System");
		        return -1;
		}
	}
   }

   if (unmount_vendor) {
	if (PartitionManager.Is_Mounted_By_Path("/vendor")) {
		gui_msg("unmount_vendor=Unmounting Vendor...");
		if (PartitionManager.UnMount_By_Path("/vendor", false)) {
		   	//unlink("/vendor");
		   	//mkdir("/vendor", 0755);
		} else {
			gui_msg("unmount_vendor_err=Failed to unmount Vendor");
			return -1;
		}
	}
   }

   // DJ9, 20200622: try to avoid a situation where blockimg will bomb out when trying to create a stash
   if (TWFunc::Path_Exists("/cache/.") && !TWFunc::Path_Exists("/cache/recovery/.") && !TWFunc::Path_Exists("/data/cache/.")) {
	LOGINFO("Recreating the /cache/recovery/ folder ...\n");
	if (!TWFunc::Recursive_Mkdir("/cache/recovery", false))
	   LOGERR("Could not create /cache/recovery - blockimg may have problems with creating stashes\n");
   }
   // DJ9

  time_t start, stop;
  time(&start);

  std::string update_binary_name(UPDATE_BINARY_NAME);
  ZipEntry64 update_binary_entry;
  if (FindEntry(Zip, update_binary_name, &update_binary_entry) == 0) {
		LOGINFO("Update binary zip\n");
		// Additionally verify the compatibility of the package.
		if (!Fox_Skip_Treble_Compatibility_Check() && !verify_package_compatibility(Zip)) {
			gui_err("zip_compatible_err=Zip Treble compatibility error!");
			ret_val = INSTALL_CORRUPT;
		} else {
			ret_val = Prepare_Update_Binary(path, Zip);
			if (ret_val == INSTALL_SUCCESS) {
				usleep(32);
				run_rom_scripts = ((DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE) != 0) // only run after flashing a ROM
	  			&& (DataManager::GetIntValue(FOX_INSTALL_PREBUILT_ZIP) != 1)); // don't run for built-in zips

				if (run_rom_scripts && TWFunc::Path_Exists(FOX_PRE_ROM_FLASH_SCRIPT)) {
					TWFunc::RunFoxScript(FOX_PRE_ROM_FLASH_SCRIPT, path);
	  			}

				ret_val = Run_Update_Binary(path, wipe_cache, UPDATE_BINARY_ZIP_TYPE);

				if (DataManager::GetIntValue("fox_processing_asserts") != 0) {
					TWFunc::Fox_Property_Set("ro.product.device", DataManager::GetStrValue("fox_product_device"));
					DataManager::SetValue("fox_processing_asserts", "0");
					LOGINFO("\nDevice code name restored\n");
				}
			}
		}
	} else {
		std::string ab_binary_name(AB_OTA);
		ZipEntry64 ab_binary_entry;
		if (FindEntry(Zip, ab_binary_name, &ab_binary_entry) == 0) {
			LOGINFO("AB zip\n");

			HighSpeedResult hs_res = TryHighSpeedAbInstall(path, Zip);
			if (hs_res == HighSpeedResult::kSucceeded) {
				DataManager::SetValue(FOX_ZIP_INSTALLER_CODE, 1); // mark as custom ROM install
				ret_val = INSTALL_SUCCESS;
			} else if (hs_res == HighSpeedResult::kAborted) {
				ret_val = INSTALL_ERROR;
			} else {
				// high-speed flash wasn't on for this install -- run the normal
				// update_engine path exactly like before
				gui_msg(Msg(msg::kHighlight, "flash_ab_inactive=Flashing A/B zip to inactive slot: {1}")(PartitionManager.Get_Active_Slot_Display()=="A"?"B":"A"));
				// We need this so backuptool can do its magic
				bool system_mount_state = PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path());
				bool vendor_mount_state = PartitionManager.Is_Mounted_By_Path("/vendor");
				PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
				PartitionManager.Mount_By_Path("/vendor", false);
				TWFunc::copy_file("/system/bin/sh", "/tmp/sh", 0755);
				mount("/tmp/sh", "/system/bin/sh", "auto", MS_BIND, NULL);

				run_rom_scripts = true;
				usleep(32);

				if (run_rom_scripts && TWFunc::Path_Exists(FOX_PRE_ROM_FLASH_SCRIPT)) {
					TWFunc::RunFoxScript(FOX_PRE_ROM_FLASH_SCRIPT, path);
				}

				TWFunc::IsRecoveryOverwritten(true);

				ret_val = Run_Update_Binary(path, wipe_cache, AB_OTA_ZIP_TYPE);

				DataManager::SetValue(FOX_ZIP_INSTALLER_CODE, 1); // mark as custom ROM install

				umount("/system/bin/sh");
				unlink("/tmp/sh");
				if (!vendor_mount_state)
					PartitionManager.UnMount_By_Path("/vendor", false);
				if (!system_mount_state)
					PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
				if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false)) {
					PartitionManager.Unlock_Block_Partitions();
					PartitionManager.Prepare_All_Super_Volumes();
					gui_warn("mount_vab_partitions=Devices on super may not mount until after rebooting recovery.");
				}
				gui_warn("flash_ab_reboot=To flash additional zips, please reboot recovery to switch to the updated slot.");
			}
		} else {
			std::string binary_name("ui.xml");
			ZipEntry64 binary_entry;
			if (FindEntry(Zip, binary_name, &binary_entry) == 0) {
				LOGINFO("OrangeFox theme zip\n");
				ret_val = Install_Theme(path, Zip);
			} else {
				ret_val = INSTALL_CORRUPT;
			}
		}
   }

  time(&stop);
  int total_time = (int) difftime(stop, start);

  if (ret_val == INSTALL_CORRUPT)
    {
        set_miui_install_status(OTA_CORRUPT, true);
        gui_err("invalid_zip_format=Invalid zip file format!");
    }
  else
  if (ret_val == INSTALL_ERROR)
     {
	set_miui_install_status(OTA_ERROR, false);
     }
  else // success - so let us see whether we need to run OTA_BAK
  {
     // if MIUI-specific features have been disabled
     if (Fox_Skip_OTA()) // yes
     {
         //LOGINFO("OrangeFox: not running the incremental OTA backup (OTA_BAK).\n");
     }  
     else // else let us proceed with the OTA stuff
     if (DataManager::GetIntValue(FOX_INCREMENTAL_OTA_FAIL) != 1)
     {
      	if (DataManager::GetIntValue(FOX_INCREMENTAL_PACKAGE) == 1 && DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE) != 0)
      	  {
      	    if (TWinstall_Run_OTA_BAK (true)) // true, because the value of Fox_Zip_Installer_Code to be set
      	      {
	        if (Fox_OTA_Backup_Stock_Boot_Image()) // whether to create an additional backup of the stock boot image
	           {
	      		usleep(2048);
	      		string ota_folder = DataManager::GetStrValue("ota_bak_folder");
	      		usleep(2048);
	      		if (ota_folder.empty())
	      		   ota_folder = FOX_OTA_PATH;
			string ota_bootimg = ota_folder + "/boot.img";
			if (TWFunc::Path_Exists(boot_bak_img)) {
			   if (TWFunc::copy_file(boot_bak_img, ota_bootimg, 0644) == 0) {
			   	LOGINFO("OrangeFox: stock boot image extracted into the OTA directory.\n");
			     }
			   unlink(boot_bak_img.c_str());
		 	}
	           }
      	      }
      	  }

      	DataManager::SetValue(FOX_METADATA_PRE_BUILD, 0);
      	DataManager::SetValue(FOX_MIUI_ZIP_TMP, 0);
      	DataManager::SetValue(FOX_INCREMENTAL_OTA_FAIL, 0);
      	DataManager::SetValue(FOX_LOADED_FINGERPRINT, 0);
      	DataManager::SetValue(FOX_RUN_SURVIVAL_BACKUP, 0);
      
     } // end of OTA stuff
    LOGINFO("Install took %i second(s).\n", total_time);
   }

   if (ret_val == INSTALL_SUCCESS)
      set_miui_install_status(OTA_SUCCESS, false);

   usleep(32);
   if (DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE) != 0) // just flashed a ROM
   {
      usleep(16);
      TWFunc::Check_OrangeFox_Overwrite_FromROM(false, path);
   }

   if (run_rom_scripts && TWFunc::Path_Exists(FOX_POST_ROM_FLASH_SCRIPT)) {
   	usleep(2048);
   	TWFunc::RunFoxScript(FOX_POST_ROM_FLASH_SCRIPT, path);
   	sleep(1);
   	DataManager::SetValue("found_fox_overwriting_rom", "0");
   	TWFunc::Fox_Property_Set("found_fox_overwriting_rom", "");
   }

  return ret_val;
}
