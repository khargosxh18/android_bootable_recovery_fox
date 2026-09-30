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
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <dirent.h>
#include <algorithm>
#include <android-base/properties.h>
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
// thread -- that's the slow part, not the disk write. This path instead
// lets payload-dumper-go decode the partitions in parallel and write them
// DIRECTLY into the target slot's block nodes: the dumper's output
// directory is a small farm of symlinks (<name>.img -> real block node),
// so extraction is the flash, there is no scratch copy on /data and no
// second write pass. Because the nodes must already exist at their final
// size, the Super group for the target slot is rebuilt BEFORE extraction.
// Finally the active slot is flipped by hand since update_engine never ran
// to do it for us.
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

const char* kHighSpeedDumperPath = "/system/bin/payload-dumper-go";
// directory of symlinks handed to the dumper as its output dir. lives on
// the recovery ramdisk (tmpfs), holds only tiny symlinks, never real data
const char* kHighSpeedLinkDir = "/tmp/hsf_links";

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
	bool is_logical = false;   // true if this one lives in Super (needs the group rebuild)
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
std::string ResolveTargetPath(const std::string& bare_name, bool* is_logical = nullptr) {
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
	if (p != physical.end()) {
		if (is_logical) *is_logical = false;
		return p->second;
	}
	auto l = logical.find(bare_name);
	if (l != logical.end()) {
		if (is_logical) *is_logical = true;
		return l->second;
	}
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

// which slot recovery itself booted from -- that slot must never be
// written. ro.boot.slot_suffix is authoritative (the slot menu can change
// the *bootloader's* active slot without changing what we booted from);
// fall back to the manager's notion of the active slot only if the
// property is missing. returns "A", "B", or "" if it can't be known.
std::string DetermineBootedSlot() {
	std::string prop = android::base::GetProperty("ro.boot.slot_suffix", "");
	std::string display = PartitionManager.Get_Active_Slot_Display();
	if (prop == "_a" || prop == "_b") {
		std::string from_prop = (prop == "_a") ? "A" : "B";
		if (!display.empty() && display != from_prop)
			LOGINFO("high-speed: active slot display is '%s' but recovery booted from '%s' -- treating '%s' as the booted slot\n",
				display.c_str(), from_prop.c_str(), from_prop.c_str());
		return from_prop;
	}
	if (display == "A" || display == "B")
		return display;
	return "";
}

// resolves symlinks and only succeeds if the final target is a block device
bool ResolveBlockNode(const std::string& path, std::string* real) {
	char buf[PATH_MAX];
	if (realpath(path.c_str(), buf) == nullptr)
		return false;
	struct stat st;
	if (stat(buf, &st) != 0 || !S_ISBLK(st.st_mode))
		return false;
	*real = buf;
	return true;
}

bool GetBlockDeviceSize(const std::string& path, uint64_t* size) {
	android::base::unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
	if (fd == -1)
		return false;
	uint64_t bytes = 0;
	if (ioctl(fd.get(), BLKGETSIZE64, &bytes) != 0)
		return false;
	*size = bytes;
	return true;
}

// works out the exact node a physical (boot/dtbo/vbmeta...) partition
// must be written to on the TARGET slot, without touching any global slot
// state. Primary_Block_Device is the bare fstab path, so appending the
// target suffix gives the node directly (same pattern as
// ReflashSelfToOtherSlot). refuses anything that isn't a real block
// device, and refuses if it would resolve to the booted slot's node.
bool ResolvePhysicalTarget(const HighSpeedPartitionPlan& part, const std::string& booted_suffix,
                           const std::string& target_suffix, std::string* node) {
	std::string base;
	TWPartition* twp = PartitionManager.Find_Partition_By_Path(part.target_path);
	if (twp) {
		if (!twp->Is_SlotSelect()) {
			LOGERR("high-speed: '%s' is not a slot-select partition here, refusing to guess its target\n",
				part.name.c_str());
			return false;
		}
		base = twp->Get_Primary_Block_Device();
	} else {
		base = "/dev/block/bootdevice/by-name/" + part.name;
		LOGINFO("high-speed: '%s' isn't in the fstab, using the by-name path\n", part.name.c_str());
	}
	if (base.empty()) {
		LOGERR("high-speed: no block device known for '%s'\n", part.name.c_str());
		return false;
	}
	auto ends_with = [&](const char* suf) {
		return base.size() > 2 && base.compare(base.size() - 2, 2, suf) == 0;
	};
	if (ends_with("_a") || ends_with("_b")) {
		LOGERR("high-speed: '%s' already looks slot-suffixed ('%s'), refusing to guess\n",
			part.name.c_str(), base.c_str());
		return false;
	}

	std::string target_real;
	if (!ResolveBlockNode(base + target_suffix, &target_real)) {
		LOGERR("high-speed: target node '%s%s' is missing or not a block device\n",
			base.c_str(), target_suffix.c_str());
		return false;
	}
	std::string booted_real;
	if (ResolveBlockNode(base + booted_suffix, &booted_real) && booted_real == target_real) {
		LOGERR("high-speed: target node for '%s' resolves to the booted slot's node, refusing\n",
			part.name.c_str());
		return false;
	}
	*node = target_real;
	LOGINFO("high-speed: '%s' -> '%s%s' (%s)\n", part.name.c_str(), base.c_str(),
		target_suffix.c_str(), target_real.c_str());
	return true;
}

// empties and removes the symlink farm. unlinkat() on a symlink removes
// the link itself and never follows it, so this can't reach a block node.
void RemoveHighSpeedLinkFarm() {
	DIR* d = opendir(kHighSpeedLinkDir);
	if (d) {
		int dfd = dirfd(d);
		struct dirent* e;
		while ((e = readdir(d)) != nullptr) {
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			unlinkat(dfd, e->d_name, 0);
		}
		closedir(d);
	}
	rmdir(kHighSpeedLinkDir);
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
		entry.target_path = ResolveTargetPath(entry.name, &entry.is_logical);
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

// removes the symlink farm whenever this goes out of scope, win or lose.
// only tiny symlinks ever live there -- the real data goes straight to
// the block nodes, so there is nothing big to clean up on /data anymore.
struct HighSpeedLinkFarmGuard {
	~HighSpeedLinkFarmGuard() { RemoveHighSpeedLinkFarm(); }
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

	// ---- which slot are we allowed to write? -------------------------
	// the slot recovery booted from is never written. if we can't tell
	// which one that is, stop before touching anything.
	const std::string booted_slot = DetermineBootedSlot();
	if (booted_slot.empty()) {
		gui_err("hs_slot_unknown=Couldn't tell which slot is currently booted, so high-speed flash can't pick a safe target. Stopping.");
		return HighSpeedResult::kAborted;
	}
	const std::string target_slot = (booted_slot == "A") ? "B" : "A";
	const std::string booted_suffix = (booted_slot == "A") ? "_a" : "_b";
	const std::string target_suffix = (target_slot == "A") ? "_a" : "_b";
	LOGINFO("high-speed: booted slot %s, writing slot %s\n", booted_slot.c_str(), target_slot.c_str());

	// no partition may appear twice in the plan
	{
		std::set<std::string> seen_names;
		for (auto& part : plan) {
			if (!seen_names.insert(part.name).second) {
				LOGERR("high-speed: partition '%s' appears twice in the payload\n", part.name.c_str());
				gui_err("hs_duplicate_part=The payload lists the same partition twice. Stopping before anything is written.");
				return HighSpeedResult::kAborted;
			}
		}
	}

	// ---- pre-flight, BEFORE anything destructive ---------------------
	// resolve and validate every physical target (exists, is a block
	// device, isn't the booted slot's node, big enough for the image).
	// nothing has been modified at this point, so any failure here is free.
	std::map<std::string, std::string> nodes;  // partition name -> real block node
	std::set<std::string> seen_nodes;
	for (auto& part : plan) {
		if (part.is_logical)
			continue;
		std::string node;
		if (!ResolvePhysicalTarget(part, booted_suffix, target_suffix, &node)) {
			gui_err(("hs_target_invalid=Couldn't find a safe target for '" + part.name +
				"' on slot " + target_slot + ". Nothing was written. See the log for details.").c_str());
			return HighSpeedResult::kAborted;
		}
		uint64_t node_size = 0;
		if (!GetBlockDeviceSize(node, &node_size)) {
			LOGERR("high-speed: couldn't read the size of '%s'\n", node.c_str());
			gui_err(("hs_target_size_unknown=Couldn't read the size of the target for '" + part.name +
				"'. Nothing was written.").c_str());
			return HighSpeedResult::kAborted;
		}
		if (node_size < part.expanded_size) {
			LOGERR("high-speed: image for '%s' is %llu bytes but the target node is only %llu\n",
				part.name.c_str(), (unsigned long long)part.expanded_size, (unsigned long long)node_size);
			gui_err(("hs_target_too_small=The image for '" + part.name +
				"' is bigger than its partition on slot " + target_slot + ". Nothing was written.").c_str());
			return HighSpeedResult::kAborted;
		}
		if (!seen_nodes.insert(node).second) {
			LOGERR("high-speed: two partitions resolve to the same node '%s'\n", node.c_str());
			gui_err("hs_target_collision=Two partitions resolved to the same target. Nothing was written.");
			return HighSpeedResult::kAborted;
		}
		nodes[part.name] = node;
	}

	// if a normal install got interrupted before, libsnapshot can leave
	// state behind here that makes Ensure_Logical_Partition_Writable's
	// "is an update in progress" check refuse to touch anything -- clear
	// it the same way a fresh update_engine run always does at the start
	PartitionManager.Mount_By_Path("/metadata", false);
	if (TWFunc::Path_Exists("/metadata/ota"))
		TWFunc::removeDir("/metadata/ota", false);

	// fresh, empty symlink farm for the dumper's output dir
	RemoveHighSpeedLinkFarm();
	if (mkdir(kHighSpeedLinkDir, 0700) != 0) {
		LOGERR("high-speed: couldn't create '%s' (%s)\n", kHighSpeedLinkDir, strerror(errno));
		gui_err("hs_scratch_mkdir_failed=Couldn't create the link folder for high-speed extraction. Nothing was written.");
		return HighSpeedResult::kAborted;
	}
	HighSpeedLinkFarmGuard farm_guard;

	// pass the dumper the biggest partitions first -- it starts them in
	// the order we give it, and the biggest one decides how long the
	// whole extraction takes, so it shouldn't sit queued behind small ones
	std::string joined_names;
	{
		std::vector<const HighSpeedPartitionPlan*> by_size;
		for (auto& part : plan)
			by_size.push_back(&part);
		std::stable_sort(by_size.begin(), by_size.end(),
			[](const HighSpeedPartitionPlan* a, const HighSpeedPartitionPlan* b) {
				return a->expanded_size > b->expanded_size;
			});
		for (size_t i = 0; i < by_size.size(); i++) {
			if (i > 0)
				joined_names += ",";
			joined_names += by_size[i]->name;
		}
	}

	// ---- first destructive step: lay out the target slot's Super group
	// in ONE pass, at final sizes, so the dm nodes exist at the right size
	// before the dumper opens them. this only rewrites the TARGET slot's
	// metadata; the booted slot's partitions are never touched.
	std::map<std::string, std::string> mapped_paths;
	{
		std::vector<std::pair<std::string, uint64_t>> logical_sizes;
		for (auto& part : plan) {
			if (part.is_logical)
				logical_sizes.push_back({part.name, part.expanded_size});
		}
		if (!logical_sizes.empty())
			gui_print("High-speed flash: laying out the Super partitions for slot %s...\n", target_slot.c_str());
		if (!PartitionManager.Rebuild_Logical_Group_For_High_Speed_Flash(target_suffix, logical_sizes, &mapped_paths)) {
			gui_err(("hs_group_rebuild_failed=Couldn't lay out the Super partition group for this ROM. The slot you booted from wasn't touched and the active slot wasn't changed; slot " +
				target_slot + " may be incomplete. See the log above for details.").c_str());
			return HighSpeedResult::kAborted;
		}
	}

	// validate every logical node the rebuild just mapped
	for (auto& part : plan) {
		if (!part.is_logical)
			continue;
		auto it = mapped_paths.find(part.name);
		if (it == mapped_paths.end()) {
			LOGERR("high-speed: no mapped node was returned for '%s'\n", part.name.c_str());
			gui_err(("hs_map_missing='" + part.name + "' wasn't mapped after the Super layout. Nothing has been extracted; the active slot wasn't changed.").c_str());
			return HighSpeedResult::kAborted;
		}
		std::string node;
		if (!ResolveBlockNode(it->second, &node)) {
			LOGERR("high-speed: mapped path '%s' for '%s' isn't a block device\n",
				it->second.c_str(), part.name.c_str());
			gui_err(("hs_map_invalid=The mapped target for '" + part.name +
				"' isn't a valid block device. Nothing has been extracted; the active slot wasn't changed.").c_str());
			return HighSpeedResult::kAborted;
		}
		uint64_t node_size = 0;
		if (!GetBlockDeviceSize(node, &node_size) || node_size < part.expanded_size) {
			LOGERR("high-speed: mapped node '%s' for '%s' is %llu bytes, need %llu\n",
				node.c_str(), part.name.c_str(), (unsigned long long)node_size,
				(unsigned long long)part.expanded_size);
			gui_err(("hs_map_too_small=The mapped target for '" + part.name +
				"' is smaller than its image. Nothing has been extracted; the active slot wasn't changed.").c_str());
			return HighSpeedResult::kAborted;
		}
		if (!seen_nodes.insert(node).second) {
			LOGERR("high-speed: mapped node '%s' collides with another target\n", node.c_str());
			gui_err("hs_target_collision=Two partitions resolved to the same target. Nothing has been extracted; the active slot wasn't changed.");
			return HighSpeedResult::kAborted;
		}
		nodes[part.name] = node;
	}

	// build the farm: <name>.img -> real block node. every node was
	// verified above as an existing block device, so the dumper's open()
	// can never fall into creating a regular file under /dev/block
	for (auto& part : plan) {
		std::string link = std::string(kHighSpeedLinkDir) + "/" + part.name + ".img";
		if (symlink(nodes[part.name].c_str(), link.c_str()) != 0) {
			LOGERR("high-speed: couldn't link '%s' -> '%s' (%s)\n", link.c_str(),
				nodes[part.name].c_str(), strerror(errno));
			gui_err("hs_link_failed=Couldn't set up the extraction targets. Nothing has been extracted; the active slot wasn't changed.");
			return HighSpeedResult::kAborted;
		}
	}

	gui_print("High-speed flash: writing partitions straight to slot %s, this takes some seconds...\n", target_slot.c_str());

	time_t extract_start, extract_stop;
	time(&extract_start);

	const char* dumper_args[] = {
		kHighSpeedDumperPath,
		"-c", "8",
		"-q", // Silence payload-dumper-go's terminal animation spam
		"-p", joined_names.c_str(),
		"-o", kHighSpeedLinkDir,
		package.c_str(),
		nullptr
	};

	int status = 0;
	pid_t pid = fork();
	if (pid == 0) {
		execv(dumper_args[0], const_cast<char**>(dumper_args));
		_exit(127);
	} else if (pid < 0) {
		gui_err("hs_dumper_launch_failed=Couldn't start the high-speed extractor. Nothing was written; the active slot wasn't changed.");
		return HighSpeedResult::kAborted;
	}
	pid_t waited;
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		LOGERR("high-speed: dumper failed (wait=%d status=0x%x)\n", (int)waited, status);
		gui_err(("hs_extraction_failed=High-speed extraction failed, or a checksum didn't match. The slot you booted from wasn't touched and the active slot wasn't changed, but slot " +
			target_slot + " is now incomplete. You Can Either Flash Using Normal Method, Or Reboot To Previous System Normally.").c_str());
		return HighSpeedResult::kAborted;
	}

	// the dumper writes through the page cache and doesn't fsync device
	// nodes. flush every target explicitly, then a global sync, and only
	// call it success if every flush went through.
	gui_print("High-speed flash: flushing writes to storage...\n");
	for (auto& kv : nodes) {
		android::base::unique_fd fd(open(kv.second.c_str(), O_RDONLY | O_CLOEXEC));
		if (fd == -1 || fsync(fd.get()) != 0) {
			LOGERR("high-speed: couldn't flush '%s' (%s)\n", kv.second.c_str(), strerror(errno));
			gui_err(("hs_flush_failed=Couldn't confirm that '" + kv.first +
				"' was fully written to storage. The active slot wasn't changed; slot " + target_slot +
				" should not be booted.").c_str());
			return HighSpeedResult::kAborted;
		}
	}
	sync();

	time(&extract_stop);
	int extract_secs = (int) difftime(extract_stop, extract_start);
	gui_print("High-speed flash: slot %s written and flushed in %ds.\n", target_slot.c_str(), extract_secs);

	if (!ReflashSelfToOtherSlot(booted_slot, target_slot))
		gui_warn("hs_recovery_copy_failed=High-speed flash: couldn't copy OrangeFox onto the other slot's recovery. "
		         "You may need to flash it there manually before switching to that slot.");

	// update_engine never ran, so nothing told the bootloader to switch
	// slots -- that's the one piece of its job we still have to do
	// ourselves, and only now that every partition has actually succeeded
	PartitionManager.Set_Active_Slot(target_slot);
	if (PartitionManager.Get_Active_Slot_Display() != target_slot) {
		gui_err(("hs_slot_switch_failed=Every partition was written and flushed, but switching to slot " + target_slot +
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
