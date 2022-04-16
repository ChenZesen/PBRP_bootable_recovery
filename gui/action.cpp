/*
	Copyright 2013 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	This file is part of PBRP/PitchBlack Recovery Project.

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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <dirent.h>
#include <private/android_filesystem_config.h>
#include <android-base/properties.h>

#include <string>
#include <sstream>
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../twrpRepacker.hpp"
#include "../openrecoveryscript.hpp"

#include "twinstall/adb_install.h"

#include "fuse_sideload.h"
#include "blanktimer.hpp"
#include "twinstall.h"

extern "C" {
#include "../twcommon.h"
#include "../variables.h"
#include "cutils/properties.h"
#include "twinstall/adb_install.h"
};
#include "set_metadata.h"
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"
#include "tw_atomic.hpp"
#include <fstream>

GUIAction::mapFunc GUIAction::mf;
std::set<string> GUIAction::setActionsRunningInCallerThread;
static string zip_queue[10];
static int zip_queue_index;
pid_t sideload_child_pid;
extern GUITerminal* term;
extern std::vector<users_struct> Users_List;

static void *ActionThread_work_wrapper(void *data);

class ActionThread
{
public:
	ActionThread();
	~ActionThread();

	void threadActions(GUIAction *act);
	void run(void *data);
private:
	friend void *ActionThread_work_wrapper(void*);
	struct ThreadData
	{
		ActionThread *this_;
		GUIAction *act;
		ThreadData(ActionThread *this_, GUIAction *act) : this_(this_), act(act) {}
	};

	pthread_t m_thread;
	bool m_thread_running;
	pthread_mutex_t m_act_lock;
};

static ActionThread action_thread;	// for all kinds of longer running actions
static ActionThread cancel_thread;	// for longer running "cancel" actions

static void *ActionThread_work_wrapper(void *data)
{
	static_cast<ActionThread::ThreadData*>(data)->this_->run(data);
	return NULL;
}

ActionThread::ActionThread()
{
	m_thread_running = false;
	pthread_mutex_init(&m_act_lock, NULL);
}

ActionThread::~ActionThread()
{
	pthread_mutex_lock(&m_act_lock);
	if (m_thread_running) {
		pthread_mutex_unlock(&m_act_lock);
		pthread_join(m_thread, NULL);
	} else {
		pthread_mutex_unlock(&m_act_lock);
	}
	pthread_mutex_destroy(&m_act_lock);
}

void ActionThread::threadActions(GUIAction *act)
{
	pthread_mutex_lock(&m_act_lock);
	if (m_thread_running) {
		pthread_mutex_unlock(&m_act_lock);
		LOGERR("Another threaded action is already running -- not running %u actions starting with '%s'\n",
				act->mActions.size(), act->mActions[0].mFunction.c_str());
	} else {
		m_thread_running = true;
		pthread_mutex_unlock(&m_act_lock);
		ThreadData *d = new ThreadData(this, act);
		pthread_create(&m_thread, NULL, &ActionThread_work_wrapper, d);
	}
}

void ActionThread::run(void *data)
{
	ThreadData *d = (ThreadData*)data;
	GUIAction* act = d->act;

	std::vector<GUIAction::Action>::iterator it;
	for (it = act->mActions.begin(); it != act->mActions.end(); ++it)
		act->doAction(*it);

	pthread_mutex_lock(&m_act_lock);
	m_thread_running = false;
	pthread_mutex_unlock(&m_act_lock);
	delete d;
}

GUIAction::GUIAction(xml_node<>* node)
	: GUIObject(node)
{
	xml_node<>* child;
	xml_node<>* actions;
	xml_attribute<>* attr;

	if (!node)  return;

	if (mf.empty()) {
#define ADD_ACTION(n) mf[#n] = &GUIAction::n
#define ADD_ACTION_EX(name, func) mf[name] = &GUIAction::func
		// These actions will be run in the caller's thread
		ADD_ACTION(reboot);
		ADD_ACTION(home);
		ADD_ACTION(key);
		ADD_ACTION(page);
		ADD_ACTION(reload);
		ADD_ACTION(readBackup);
		ADD_ACTION(set);
		ADD_ACTION(exten);
		ADD_ACTION(clear);
		ADD_ACTION(mount);
		ADD_ACTION(unmount);
		ADD_ACTION_EX("umount", unmount);
		ADD_ACTION(restoredefaultsettings);
		ADD_ACTION(copylog);
		ADD_ACTION(compute);
		ADD_ACTION_EX("addsubtract", compute);
		ADD_ACTION(setguitimezone);
		ADD_ACTION(overlay);
		ADD_ACTION(queuezip);
		ADD_ACTION(cancelzip);
		ADD_ACTION(queueclear);
		ADD_ACTION(sleep);
		ADD_ACTION(sleepcounter);
		ADD_ACTION(appenddatetobackupname);
		ADD_ACTION(generatebackupname);
		ADD_ACTION(checkpartitionlist);
		ADD_ACTION(getpartitiondetails);
		ADD_ACTION(screenshot);
		ADD_ACTION(setbrightness);
		ADD_ACTION(flashlight);
		ADD_ACTION(fileexists);
		ADD_ACTION(killterminal);
		ADD_ACTION(checkbackupname);
		ADD_ACTION(adbsideloadcancel);
		ADD_ACTION(fixsu);
		ADD_ACTION(startmtp);
		ADD_ACTION(stopmtp);
		ADD_ACTION(cancelbackup);
		ADD_ACTION(checkpartitionlifetimewrites);
		ADD_ACTION(mountsystemtoggle);
		ADD_ACTION(setlanguage);
		ADD_ACTION(checkforapp);
		ADD_ACTION(togglebacklight);
		ADD_ACTION(enableadb);
		ADD_ACTION(enablefastboot);
		ADD_ACTION(unmapsuperdevices);

		// remember actions that run in the caller thread
		for (mapFunc::const_iterator it = mf.begin(); it != mf.end(); ++it)
			setActionsRunningInCallerThread.insert(it->first);

		// These actions will run in a separate thread
		ADD_ACTION(flash);
		ADD_ACTION(wipe);
		ADD_ACTION(refreshsizes);
		ADD_ACTION(nandroid);
		ADD_ACTION(fixcontexts);
		ADD_ACTION(fixpermissions);
		ADD_ACTION(dd);
		ADD_ACTION(partitionsd);
		ADD_ACTION(cmd);
		ADD_ACTION(terminalcommand);
		ADD_ACTION(decrypt);
		ADD_ACTION(adbsideload);
		ADD_ACTION(openrecoveryscript);
		ADD_ACTION(installsu);
		ADD_ACTION(decrypt_backup);
		ADD_ACTION(repair);
		ADD_ACTION(resize);
		ADD_ACTION(changefilesystem);
		ADD_ACTION(flashimage);
		ADD_ACTION(twcmd);
		ADD_ACTION(setbootslot);
		ADD_ACTION(readfile);
		ADD_ACTION(installapp);
                ADD_ACTION(unpack);
                ADD_ACTION(repack);
		ADD_ACTION(repackimage);
		ADD_ACTION(reflashtwrp);
		ADD_ACTION(fixabrecoverybootloop);
		ADD_ACTION(change_codename);
		ADD_ACTION(getprop);
		ADD_ACTION(flush_up_console);
		ADD_ACTION(change_root);
		ADD_ACTION(change_terminal);
	}

	// First, get the action
	actions = FindNode(node, "actions");
	if (actions)	child = FindNode(actions, "action");
	else			child = FindNode(node, "action");

	if (!child) return;

	while (child)
	{
		Action action;

		attr = child->first_attribute("function");
		if (!attr)  return;

		action.mFunction = attr->value();
		action.mArg = child->value();
		mActions.push_back(action);

		child = child->next_sibling("action");
	}

	// Now, let's get either the key or region
	child = FindNode(node, "touch");
	if (child)
	{
		attr = child->first_attribute("key");
		if (!attr) attr = child->first_attribute("hkey");
		if (attr)
		{
			std::vector<std::string> keys = TWFunc::Split_String(attr->value(), "+");
			for (size_t i = 0; i < keys.size(); ++i)
			{
				const int key = getKeyByName(keys[i]);
				mKeys[std::string(attr->name()) == "hkey" ? key + 200 : key] = false;
			}
		}
		else
		{
			attr = child->first_attribute("x");
			if (!attr)  return;
			mActionX = atol(attr->value());
			attr = child->first_attribute("y");
			if (!attr)  return;
			mActionY = atol(attr->value());
			attr = child->first_attribute("w");
			if (!attr)  return;
			mActionW = atol(attr->value());
			attr = child->first_attribute("h");
			if (!attr)  return;
			mActionH = atol(attr->value());
		}
	}
}

int GUIAction::NotifyTouch(TOUCH_STATE state, int x __unused, int y __unused)
{
	if (state == TOUCH_RELEASE)
		doActions();

	return 0;
}

int GUIAction::NotifyKey(int key, bool down)
{
	std::map<int, bool>::iterator itr = mKeys.find(key);
	if (itr == mKeys.end())
		return 1;

	bool prevState = itr->second;
	itr->second = down;

	// If there is only one key for this action, wait for key up so it
	// doesn't trigger with multi-key actions.
	// Else, check if all buttons are pressed, then consume their release events
	// so they don't trigger one-button actions and reset mKeys pressed status
	if (mKeys.size() == 1) {
		if ((!down && prevState) || mime > 500) {
			doActions();
			if (mime) {
#ifndef TW_NO_HAPTICS
				DataManager::Vibrate("tw_button_vibrate");
#endif
				mime = 0;
			}
			return 0;
		}
	} else if (down) {
		for (itr = mKeys.begin(); itr != mKeys.end(); ++itr) {
			if (!itr->second)
				return 1;
		}

		// Passed, all req buttons are pressed, reset them and consume release events
		HardwareKeyboard *kb = PageManager::GetHardwareKeyboard();
		for (itr = mKeys.begin(); itr != mKeys.end(); ++itr) {
			kb->ConsumeKeyRelease(itr->first);
			itr->second = false;
		}

		doActions();
		if (mime) {
#ifndef TW_NO_HAPTICS
			DataManager::Vibrate("tw_button_vibrate");
#endif
			mime = 0;
		}
		return 0;
	}

	return 1;
}

int GUIAction::NotifyVarChange(const std::string& varName, const std::string& value)
{
	GUIObject::NotifyVarChange(varName, value);

	if (varName.empty() && !isConditionValid() && mKeys.empty() && !mActionW)
		doActions();
	else if ((varName.empty() || IsConditionVariable(varName)) && isConditionValid() && isConditionTrue())
		doActions();

	return 0;
}

void GUIAction::simulate_progress_bar(void)
{
	gui_msg("simulating=Simulating actions...");
	for (int i = 0; i < 5; i++)
	{
		if (PartitionManager.stop_backup.get_value()) {
			DataManager::SetValue("tw_cancel_backup", 1);
			gui_msg("backup_cancel=Backup Cancelled");
			DataManager::SetValue("ui_progress", 0);
			PartitionManager.stop_backup.set_value(0);
			return;
		}
		usleep(500000);
		DataManager::SetValue("ui_progress", i * 20);
	}
}

int GUIAction::flash_zip(std::string filename, int* wipe_cache)
{
	int ret_val = 0;

	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("ui_portion_size", 0);
	DataManager::SetValue("ui_portion_start", 0);

	if (filename.empty())
	{
		LOGERR("No file specified.\n");
		return -1;
	}

	if (!TWFunc::Path_Exists(filename)) {
		if (!PartitionManager.Mount_By_Path(filename, true)) {
			return -1;
		}
		if (!TWFunc::Path_Exists(filename)) {
			gui_msg(Msg(msg::kError, "unable_to_locate=Unable to locate {1}.")(filename));
			return -1;
		}
	}

	if (simulate) {
		simulate_progress_bar();
	} else {
		char apex_enabled[PROPERTY_VALUE_MAX];
		property_get("twrp.apex.flattened", apex_enabled, "");
		if (strcmp(apex_enabled, "true") == 0) {
			umount("/apex");
		}
		ret_val = TWinstall_zip(filename.c_str(), wipe_cache, (bool) DataManager::GetIntValue(TW_SKIP_DIGEST_CHECK_VAR));
		PartitionManager.Unlock_Block_Partitions();
		// Now, check if we need to ensure TWRP remains installed...
		struct stat st;
		if (stat("/system/bin/installTwrp", &st) == 0)
		{
			DataManager::SetValue("tw_operation", "Configuring TWRP");
			DataManager::SetValue("tw_partition", "");
			gui_msg("config_twrp=Configuring TWRP...");
			if (TWFunc::Exec_Cmd("/system/bin/installTwrp reinstall") < 0)
			{
				gui_msg("config_twrp_err=Unable to configure TWRP with this kernel.");
			}
		}
	}

	// Done
	DataManager::SetValue("ui_progress", 100);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("ui_portion_size", 0);
	DataManager::SetValue("ui_portion_start", 0);
	return ret_val;
}

GUIAction::ThreadType GUIAction::getThreadType(const GUIAction::Action& action)
{
	string func = gui_parse_text(action.mFunction);
	bool needsThread = setActionsRunningInCallerThread.find(func) == setActionsRunningInCallerThread.end();
	if (needsThread) {
		if (func == "cancelbackup")
			return THREAD_CANCEL;
		else
			return THREAD_ACTION;
	}
	return THREAD_NONE;
}

int GUIAction::doActions()
{
	if (mActions.size() < 1)
		return -1;

	// Determine in which thread to run the actions.
	// Do it for all actions at once before starting, so that we can cancel the whole batch if the thread is already busy.
	ThreadType threadType = THREAD_NONE;
	std::vector<Action>::iterator it;
	for (it = mActions.begin(); it != mActions.end(); ++it) {
		ThreadType tt = getThreadType(*it);
		if (tt == THREAD_NONE)
			continue;
		if (threadType == THREAD_NONE)
			threadType = tt;
		else if (threadType != tt) {
			LOGERR("Can't mix normal and cancel actions in the same list.\n"
				"Running the whole batch in the cancel thread.\n");
			threadType = THREAD_CANCEL;
			break;
		}
	}

	// Now run the actions in the desired thread.
	switch (threadType) {
		case THREAD_ACTION:
			action_thread.threadActions(this);
			break;

		case THREAD_CANCEL:
			cancel_thread.threadActions(this);
			break;

		default: {
			// no iterators here because theme reloading might kill our object
			const size_t cnt = mActions.size();
			for (size_t i = 0; i < cnt; ++i)
				doAction(mActions[i]);
		}
	}

	return 0;
}

int GUIAction::doAction(Action action)
{
	DataManager::GetValue(TW_SIMULATE_ACTIONS, simulate);

	std::string function = gui_parse_text(action.mFunction);
	std::string arg = gui_parse_text(action.mArg);

	// find function and execute it
	mapFunc::const_iterator funcitr = mf.find(function);
	if (funcitr != mf.end())
		return (this->*funcitr->second)(arg);

	LOGERR("Unknown action '%s'\n", function.c_str());
	return -1;
}

void GUIAction::operation_start(const string operation_name)
{
	LOGINFO("operation_start: '%s'\n", operation_name.c_str());
	time(&Start);
	DataManager::SetValue(TW_ACTION_BUSY, 1);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("ui_portion_size", 0);
	DataManager::SetValue("ui_portion_start", 0);
	DataManager::SetValue("tw_operation", operation_name);
	DataManager::SetValue("tw_operation_state", 0);
	DataManager::SetValue("tw_operation_status", 0);
	bool tw_ab_device = TWFunc::get_log_dir() != CACHE_LOGS_DIR;
	DataManager::SetValue("tw_ab_device", tw_ab_device);
}

void GUIAction::operation_end(const int operation_status)
{
	time_t Stop;
	int simulate_fail;
	DataManager::SetValue("ui_progress", 100);
	if (simulate) {
		DataManager::GetValue(TW_SIMULATE_FAIL, simulate_fail);
		if (simulate_fail != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	} else {
		if (operation_status != 0) {
			DataManager::SetValue("tw_operation_status", 1);
		}
		else {
			DataManager::SetValue("tw_operation_status", 0);
		}
	}
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	blankTimer.resetTimerAndUnblank();
	property_set("twrp.action_complete", "1");
	time(&Stop);

#ifndef TW_NO_HAPTICS
	if ((int) difftime(Stop, Start) > 10)
		DataManager::Vibrate("tw_action_vibrate");
#endif

	LOGINFO("operation_end - status=%d\n", operation_status);
}

int GUIAction::reboot(std::string arg)
{
	sync();
	DataManager::SetValue("tw_gui_done", 1);
	DataManager::SetValue("tw_reboot_arg", arg);

	return 0;
}

int GUIAction::home(std::string arg __unused)
{
	gui_changePage("main");
	return 0;
}

int GUIAction::key(std::string arg)
{
	const int key = getKeyByName(arg);
	PageManager::NotifyKey(key, true);
	PageManager::NotifyKey(key, false);
	return 0;
}

int GUIAction::page(std::string arg)
{
	property_set("twrp.action_complete", "0");
	std::string page_name = gui_parse_text(arg);
	DataManager::SetValue("pb_current_page", PageManager::GetCurrentPage());
	return gui_changePage(page_name);
}

int GUIAction::reload(std::string arg __unused)
{
	PageManager::RequestReload();
	// The actual reload is handled in pages.cpp in PageManager::RunReload()
	// The reload will occur on the next Update or Render call and will
	// be performed in the rendoer thread instead of the action thread
	// to prevent crashing which could occur when we start deleting
	// GUI resources in the action thread while we attempt to render
	// with those same resources in another thread.
	return 0;
}

int GUIAction::readBackup(std::string arg __unused)
{
	string Restore_Name;

	DataManager::GetValue("tw_restore", Restore_Name);
	PartitionManager.Set_Restore_Files(Restore_Name);
	return 0;
}

int GUIAction::set(std::string arg)
{
	if (arg.find('=') != string::npos)
	{
		string varName = arg.substr(0, arg.find('='));
		string value = arg.substr(arg.find('=') + 1, string::npos);

		DataManager::GetValue(value, value);
		DataManager::SetValue(varName, value);
	}
	else
		DataManager::SetValue(arg, "1");
	return 0;
}

int GUIAction::exten(std::string arg)
{
	if (arg.length() != string::npos)
	{
		string extn = arg.substr(arg.find_last_of(".")+1);
		string mpExtn = "pb_file_extn";
		DataManager::SetValue(mpExtn, extn);
	}
	else
		LOGERR("Exten: Null argument \n");
	return 0;
}

int GUIAction::clear(std::string arg)
{
	DataManager::SetValue(arg, "0");
	return 0;
}

int GUIAction::mount(std::string arg)
{
	if (arg == "usb") {
		DataManager::SetValue(TW_ACTION_BUSY, 1);
		if (!simulate)
			PartitionManager.usb_storage_enable();
		else
			gui_msg("simulating=Simulating actions...");
	} else if (!simulate) {
		PartitionManager.Mount_By_Path(arg, true);
		PartitionManager.Add_MTP_Storage(arg);
	} else
		gui_msg("simulating=Simulating actions...");
	return 0;
}

int GUIAction::unmount(std::string arg)
{
	if (arg == "usb") {
		if (!simulate)
			PartitionManager.usb_storage_disable();
		else
			gui_msg("simulating=Simulating actions...");
		DataManager::SetValue(TW_ACTION_BUSY, 0);
	} else if (!simulate) {
		PartitionManager.UnMount_By_Path(arg, true);
	} else
		gui_msg("simulating=Simulating actions...");
	return 0;
}

int GUIAction::restoredefaultsettings(std::string arg __unused)
{
	operation_start("Restore Defaults");
	if (simulate) // Simulated so that people don't accidently wipe out the "simulation is on" setting
		gui_msg("simulating=Simulating actions...");
	else {
		DataManager::ResetDefaults();
		PartitionManager.Update_System_Details();
		PartitionManager.Mount_Current_Storage(true);
	}
	operation_end(0);
	return 0;
}

int GUIAction::copylog(std::string arg __unused)
{
	operation_start("Copy Log");
	if (!simulate)
	{
		time_t tm;
		char path[256];
		int path_len;

		string dst, curr_storage, cache_strg;
		int copy_logcat_log = 0;
		int copy_kernel_log = 0;

		DataManager::GetValue("tw_include_logcat_log", copy_logcat_log);
		DataManager::GetValue("tw_include_kernel_log", copy_kernel_log);
		PartitionManager.Mount_Current_Storage(true);
		curr_storage = DataManager::GetCurrentStoragePath();
		cache_strg = TWFunc::get_log_dir() + "/PBRP/logs/";

		snprintf(path, sizeof(path), "%s/PBRP/logs/recovery_%s", curr_storage.c_str(), arg.c_str());
		curr_storage += "/PBRP/logs";
		if (!TWFunc::Path_Exists(curr_storage))
			TWFunc::Recursive_Mkdir(curr_storage);
		curr_storage = path;

		tm = time(NULL);
		path_len = strlen(path);

		strftime(path+path_len, sizeof(path)-path_len, "_%Y-%m-%d-%H-%M-%S.log", localtime(&tm));
		dst = string(path);
		TWFunc::copy_file("/tmp/recovery.log", dst.c_str(), 0755);
		if (DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0)
		{
			LOGINFO("PBRP: Data Encrypted\n");
			gui_msg(Msg("pb_copy_log_cache=Copying Logs to Cache as well."));
			if (!TWFunc::Path_Exists(cache_strg))
				TWFunc::Recursive_Mkdir(cache_strg);
			cache_strg += dst.substr(dst.find_last_of("/")+1);
			TWFunc::copy_file("/tmp/recovery.log", cache_strg.c_str(), 0755);
		}

		tw_set_default_metadata(dst.c_str());
                if (copy_logcat_log || DataManager::GetIntValue("pb_inlclude_logcat_logging")) {
                        std::string logcatDst = path;
                        logcatDst.replace(logcatDst.find_last_of("/")+1,8,"logcat");
                        std::string logcatCmd = "/system/bin/logcat -d";
                        std::string result;
                        TWFunc::Exec_Cmd(logcatCmd, result);
                        TWFunc::write_to_file(logcatDst, result);
                        if (DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0) {
                                cache_strg.replace(cache_strg.find_last_of("/")+1,8,"logcat");
                                TWFunc::copy_file(logcatDst, cache_strg.c_str(), 0755);
                        }
                        gui_msg(Msg("copy_logcat_log=Copied logcat log to {1}")(logcatDst));
                        tw_set_default_metadata(logcatDst.c_str());
                }
		if (copy_kernel_log || DataManager::GetIntValue("pb_inlclude_dmesg_logging")) {
			std::string dmesgDst = path;
			dmesgDst.replace(dmesgDst.find_last_of("/")+1,8,"dmesg");
			std::string dmesgCmd = "/system/bin/dmesg";
			std::string result;
			TWFunc::Exec_Cmd(dmesgCmd, result);
			TWFunc::write_to_file(dmesgDst, result);
			if (DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0) {
				cache_strg.replace(cache_strg.find_last_of("/")+1,8,"dmesg");
				TWFunc::copy_file(dmesgDst, cache_strg.c_str(), 0755);
			}
			gui_msg(Msg("copy_kernel_log=Copied kernel log to {1}")(dmesgDst));
			tw_set_default_metadata(dmesgDst.c_str());
		}
		sync();
		gui_msg(Msg("copy_log=Copied recovery log to {1}")(dst));
	} else
		simulate_progress_bar();
	operation_end(0);
	return 0;
}


int GUIAction::compute(std::string arg)
{
	if (arg.find("+") != string::npos)
	{
		string varName = arg.substr(0, arg.find('+'));
		string string_to_add = arg.substr(arg.find('+') + 1, string::npos);
		int amount_to_add = atoi(string_to_add.c_str());
		int value;

		DataManager::GetValue(varName, value);
		DataManager::SetValue(varName, value + amount_to_add);
		return 0;
	}
	if (arg.find("-") != string::npos)
	{
		string varName = arg.substr(0, arg.find('-'));
		string string_to_subtract = arg.substr(arg.find('-') + 1, string::npos);
		int amount_to_subtract = atoi(string_to_subtract.c_str());
		int value;

		DataManager::GetValue(varName, value);
		value -= amount_to_subtract;
		if (value <= 0)
			value = 0;
		DataManager::SetValue(varName, value);
		return 0;
	}
	if (arg.find("*") != string::npos)
	{
		string varName = arg.substr(0, arg.find('*'));
		string multiply_by_str = gui_parse_text(arg.substr(arg.find('*') + 1, string::npos));
		int multiply_by = atoi(multiply_by_str.c_str());
		int value;

		DataManager::GetValue(varName, value);
		DataManager::SetValue(varName, value*multiply_by);
		return 0;
	}
	if (arg.find("/") != string::npos)
	{
		string varName = arg.substr(0, arg.find('/'));
		string divide_by_str = gui_parse_text(arg.substr(arg.find('/') + 1, string::npos));
		int divide_by = atoi(divide_by_str.c_str());
		int value;

		if (divide_by != 0)
		{
			DataManager::GetValue(varName, value);
			DataManager::SetValue(varName, value/divide_by);
		}
		return 0;
	}
	LOGERR("Unable to perform compute '%s'\n", arg.c_str());
	return -1;
}

int GUIAction::setguitimezone(std::string arg __unused)
{
	string SelectedZone;
	DataManager::GetValue(TW_TIME_ZONE_GUISEL, SelectedZone); // read the selected time zone into SelectedZone
	string Zone = SelectedZone.substr(0, SelectedZone.find(';')); // parse to get time zone
	string DSTZone = SelectedZone.substr(SelectedZone.find(';') + 1, string::npos); // parse to get DST component

	int dst;
	DataManager::GetValue(TW_TIME_ZONE_GUIDST, dst); // check wether user chose to use DST

	string offset;
	DataManager::GetValue(TW_TIME_ZONE_GUIOFFSET, offset); // pull in offset

	string NewTimeZone = Zone;
	if (offset != "0")
		NewTimeZone += ":" + offset;

	if (dst != 0)
		NewTimeZone += DSTZone;

	DataManager::SetValue(TW_TIME_ZONE_VAR, NewTimeZone);
	DataManager::update_tz_environment_variables();
	return 0;
}

int GUIAction::overlay(std::string arg)
{
	return gui_changeOverlay(arg);
}

int GUIAction::queuezip(std::string arg __unused)
{
	if (zip_queue_index >= 10) {
		gui_msg("max_queue=Maximum zip queue reached!");
		return 0;
	}
	DataManager::GetValue("tw_filename", zip_queue[zip_queue_index]);
	if (strlen(zip_queue[zip_queue_index].c_str()) > 0) {
		zip_queue_index++;
		DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
	}
	return 0;
}

int GUIAction::cancelzip(std::string arg __unused)
{
	if (zip_queue_index <= 0) {
		gui_msg("min_queue=Minimum zip queue reached!");
		return 0;
	} else {
		zip_queue_index--;
		DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
	}
	return 0;
}

int GUIAction::queueclear(std::string arg __unused)
{
	zip_queue_index = 0;
	DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
	return 0;
}

int GUIAction::sleep(std::string arg)
{
	operation_start("Sleep");
	usleep(atoi(arg.c_str()));
	operation_end(0);
	return 0;
}

int GUIAction::sleepcounter(std::string arg)
{
	operation_start("SleepCounter");
	// Ensure user notices countdown in case it needs to be cancelled
	blankTimer.resetTimerAndUnblank();
	int total = atoi(arg.c_str());
	for (int t = total; t > 0; t--) {
		int progress = (int)(((float)(total-t)/(float)total)*100.0);
		DataManager::SetValue("ui_progress", progress);
		::sleep(1);
		DataManager::SetValue("tw_sleep", t-1);
	}
	DataManager::SetValue("ui_progress", 100);
	operation_end(0);
	return 0;
}

int GUIAction::appenddatetobackupname(std::string arg __unused)
{
	operation_start("AppendDateToBackupName");
	string Backup_Name;
	DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
	Backup_Name += TWFunc::Get_Current_Date();
	if (Backup_Name.size() > MAX_BACKUP_NAME_LEN)
		Backup_Name.resize(MAX_BACKUP_NAME_LEN);
	DataManager::SetValue(TW_BACKUP_NAME, Backup_Name);
	PageManager::NotifyKey(KEY_END, true);
	PageManager::NotifyKey(KEY_END, false);
	operation_end(0);
	return 0;
}

int GUIAction::generatebackupname(std::string arg __unused)
{
	operation_start("GenerateBackupName");
	TWFunc::Auto_Generate_Backup_Name();
	operation_end(0);
	return 0;
}

int GUIAction::checkpartitionlist(std::string arg)
{
	string List, part_path;
	int count = 0;

	if (arg.empty())
		arg = "tw_wipe_list";
	DataManager::GetValue(arg, List);
	LOGINFO("checkpartitionlist list '%s'\n", List.c_str());
	if (!List.empty()) {
		size_t start_pos = 0, end_pos = List.find(";", start_pos);
		while (end_pos != string::npos && start_pos < List.size()) {
			part_path = List.substr(start_pos, end_pos - start_pos);
			LOGINFO("checkpartitionlist part_path '%s'\n", part_path.c_str());
			if (part_path == "/and-sec" || part_path == "DALVIK" || part_path == "INTERNAL" || part_path == "SUBSTRATUM" || part_path == "MAGISK" || part_path == "FBE") {
				// Do nothing
			} else {
				count++;
			}
			start_pos = end_pos + 1;
			end_pos = List.find(";", start_pos);
		}
		DataManager::SetValue("tw_check_partition_list", count);
	} else {
		DataManager::SetValue("tw_check_partition_list", 0);
	}
	return 0;
}

int GUIAction::getpartitiondetails(std::string arg)
{
	string List, part_path;

	if (arg.empty())
		arg = "tw_wipe_list";
	DataManager::GetValue(arg, List);
	LOGINFO("getpartitiondetails list '%s'\n", List.c_str());
	if (!List.empty()) {
		size_t start_pos = 0, end_pos = List.find(";", start_pos);
		part_path = List;
		while (end_pos != string::npos && start_pos < List.size()) {
			part_path = List.substr(start_pos, end_pos - start_pos);
			LOGINFO("getpartitiondetails part_path '%s'\n", part_path.c_str());
			if (part_path == "/and-sec" || part_path == "DALVIK" || part_path == "INTERNAL" || part_path == "SUBSTRATUM" || part_path == "MAGISK" || part_path == "FBE") {
				// Do nothing
			} else {
				DataManager::SetValue("tw_partition_path", part_path);
				break;
			}
			start_pos = end_pos + 1;
			end_pos = List.find(";", start_pos);
		}
		if (!part_path.empty()) {
			TWPartition* Part = PartitionManager.Find_Partition_By_Path(part_path);
			if (Part) {
				unsigned long long mb = 1048576;

				DataManager::SetValue("tw_partition_name", Part->Display_Name);
				DataManager::SetValue("tw_partition_mount_point", Part->Mount_Point);
				DataManager::SetValue("tw_partition_file_system", Part->Current_File_System);
				DataManager::SetValue("tw_partition_size", Part->Size / mb);
				DataManager::SetValue("tw_partition_used", Part->Used / mb);
				DataManager::SetValue("tw_partition_free", Part->Free / mb);
				DataManager::SetValue("tw_partition_backup_size", Part->Backup_Size / mb);
				DataManager::SetValue("tw_partition_removable", Part->Removable);
				DataManager::SetValue("tw_partition_is_present", Part->Is_Present);

				if (Part->Can_Repair())
					DataManager::SetValue("tw_partition_can_repair", 1);
				else
					DataManager::SetValue("tw_partition_can_repair", 0);
				if (Part->Can_Resize())
					DataManager::SetValue("tw_partition_can_resize", 1);
				else
					DataManager::SetValue("tw_partition_can_resize", 0);
				if (TWFunc::Path_Exists("/system/bin/mkfs.fat"))
					DataManager::SetValue("tw_partition_vfat", 1);
				else
					DataManager::SetValue("tw_partition_vfat", 0);
				if (TWFunc::Path_Exists("/system/bin/mkexfatfs"))
					DataManager::SetValue("tw_partition_exfat", 1);
				else
					DataManager::SetValue("tw_partition_exfat", 0);
				if (TWFunc::Path_Exists("/system/bin/mkfs.f2fs") || TWFunc::Path_Exists("/system/bin/make_f2fs"))
					DataManager::SetValue("tw_partition_f2fs", 1);
				else
					DataManager::SetValue("tw_partition_f2fs", 0);
				if (TWFunc::Path_Exists("/system/bin/mke2fs"))
					DataManager::SetValue("tw_partition_ext", 1);
				else
					DataManager::SetValue("tw_partition_ext", 0);
				return 0;
			} else {
				LOGERR("Unable to locate partition: '%s'\n", part_path.c_str());
			}
		}
	}
	DataManager::SetValue("tw_partition_name", "");
	DataManager::SetValue("tw_partition_file_system", "");
	// Set this to 0 to prevent trying to partition this device, just in case
	DataManager::SetValue("tw_partition_removable", 0);
	return 0;
}

int GUIAction::screenshot(std::string arg __unused)
{
	time_t tm;
	char path[256];
	int path_len;
	uid_t uid = AID_MEDIA_RW;
	gid_t gid = AID_MEDIA_RW;

	const std::string storage = DataManager::GetCurrentStoragePath();
	if (PartitionManager.Is_Mounted_By_Path(storage)) {
		snprintf(path, sizeof(path), "%s/PBRP/Screenshots/", storage.c_str());
	} else {
		strcpy(path, "/tmp/");
	}

	if (!TWFunc::Create_Dir_Recursive(path, 0775, uid, gid))
		return 0;

	tm = time(NULL);
	path_len = strlen(path);

	// Screenshot_PBRP_2014-01-01-18-21-38.png
	strftime(path+path_len, sizeof(path)-path_len, "Screenshot_PBRP_%Y-%m-%d-%H-%M-%S.png", localtime(&tm));

	int res = gr_save_screenshot(path);
	if (res == 0) {
		chmod(path, 0666);
		chown(path, uid, gid);

		gui_msg(Msg("screenshot_saved=Screenshot was saved to {1}")(path));

		// blink to notify that the screenshot was taken
		gr_color(255, 255, 255, 255);
		gr_fill(0, 0, gr_fb_width(), gr_fb_height());
		gr_flip();
		gui_forceRender();
	} else {
		gui_err("screenshot_err=Failed to take a screenshot!");
	}
	return 0;
}

int GUIAction::setbrightness(std::string arg)
{
	return TWFunc::Set_Brightness(arg);
}

int GUIAction::fileexists(std::string arg)
{
	struct stat st;
	string newpath = arg + "/.";

	operation_start("FileExists");
	if (stat(arg.c_str(), &st) == 0 || stat(newpath.c_str(), &st) == 0)
		operation_end(0);
	else
		operation_end(1);
	return 0;
}

void GUIAction::backup_before_flash()
{
    char getvalue[PROPERTY_VALUE_MAX];
    property_get("ro.boot.fastboot", getvalue, "");
    std::string bootmode(getvalue);
	if (((DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) || DataManager::GetIntValue("pb_theming_mode") == 1)
		 && bootmode != "1") {
		if (simulate) {
			simulate_progress_bar();
		} else {
			TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
			std::string target_image = "/tmp/boot.img";
			PartitionSettings part_settings;
			part_settings.Part = Boot;
			part_settings.Backup_Folder = "/tmp/";
			part_settings.adbbackup = false;
			part_settings.generate_digest = false;
			part_settings.generate_md5 = false;
			part_settings.PM_Method = PM_BACKUP;
			part_settings.progress = NULL;
			pid_t not_a_pid = 0;
			if (!Boot->Backup(&part_settings, &not_a_pid))
            		{
            			return;
            		}
			else {
				std::string backed_up_image = part_settings.Backup_Folder;
				backed_up_image += Boot->Backup_FileName;
				target_image = "/tmp/boot.img";
				if (rename(backed_up_image.c_str(), target_image.c_str()) != 0) {
					LOGERR("Failed to rename '%s' to '%s'\n", backed_up_image.c_str(), target_image.c_str());
				}
			}
		}
		if (DataManager::GetIntValue("pb_theming_mode") == 1)
			DataManager::SetValue("pb_theming_mode", "0");
		gui_msg("done=Done.");
	}
}

int GUIAction::reinject_after_flash()
{
    char getvalue[PROPERTY_VALUE_MAX];
    property_get("ro.boot.fastboot", getvalue, "");
    twrpRepacker repacker;
    std::string bootmode(getvalue);
	if (((DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) || DataManager::GetIntValue("pb_theming_mode") == 1)
		 && bootmode != "1") {
        if (!TWFunc::Path_Exists("/tmp/boot.img")) {
            LOGERR("Backup image doesn't exist so TWRP is unable to restore it!");
            return 0;
        }
		gui_msg("injecttwrp=Restoring TWRP in boot image...");
		int op_status = 1;
		operation_start("Repack Image");
		if (!simulate)
		{
			std::string path = "/tmp/boot.img";
			Repack_Options_struct Repack_Options;
			Repack_Options.Disable_Verity = false;
			Repack_Options.Disable_Force_Encrypt = false;
			Repack_Options.Backup_First = false;
			Repack_Options.Type = REPLACE_RAMDISK;
			if (!repacker.Repack_Image_And_Flash(path, Repack_Options))
				return 0;
            string cmd = "rm -f " + path;
		    TWFunc::Exec_Cmd(cmd);
		} else
			simulate_progress_bar();
		op_status = 0;
		if (DataManager::GetIntValue("pb_theming_mode") == 1)
			DataManager::SetValue("pb_theming_mode", "0");
		operation_end(op_status);
        return 1;
	}
    return 0;
}

int GUIAction::ozip_decrypt(string zip_path)
{
	if (!TWFunc::Path_Exists("/system/bin/ozip_decrypt")) {            
            return 1;
        }
	gui_msg("ozip_decrypt_decryption=Starting Ozip Decryption...");
	int ret = TWFunc::Exec_Cmd("ozip_decrypt " + (string)TW_OZIP_DECRYPT_KEY + " '" + zip_path + "'");
	gui_msg("ozip_decrypt_finish=Ozip Decryption Finished!");
	return ret;
}

int GUIAction::keypressed()
{
	return TWFunc::Exec_Cmd("/system/bin/keycheck", false, true);
}

int GUIAction::keycheck(std::string zip, int w __unused)
{
	string insf, pb_installed;
	insf = "/tmp/pb/installed";
	int pbRet;

	if (!TWFunc::Path_Exists(insf) && zip != "recovery.img")
		return -2;

	pb_installed = zip != "recovery.img" ? TWFunc::File_Property_Get(insf, "installed") : "1";
#ifdef AB_OTA_UPDATER
	int ramdisk_patched[2], slot = PartitionManager.Get_Active_Slot_Display() == "A" ? 0 : 1;
	ramdisk_patched[0] = zip != "recovery.img" ? atoi(TWFunc::File_Property_Get(insf, "boot_a").c_str()) : 1;
	ramdisk_patched[1] = zip != "recovery.img" ? atoi(TWFunc::File_Property_Get(insf, "boot_b").c_str()) : 1;
#endif
	pbRet = atoi(pb_installed.c_str());
	if (pbRet && TWFunc::Path_Exists("/system/bin/keycheck"))
	{
#ifdef AB_OTA_UPDATER
		if (ramdisk_patched[slot])
		{
			gui_print_color("normal", "* * * * * * * * * * * * * * * * * *\n");
			if (zip != "recovery.img")
				gui_highlight("pb_flashed_=* Magisk Patched Ramdisk Detected!!!");
			gui_msg(Msg(msg::kHighlight, "pb_flashed=* New PBRP Flashed, press Volume {1}")("Up For Flashing Magisk."));
			gui_msg(Msg(msg::kHighlight, "pb_rb_msg=*Volume {1} to Finish")("Down"));
			gui_print_color("normal", "* * * * * * * * * * * * * * * * * *\n");
			pbRet = keypressed();
			if (pbRet == 42) {
				TWFunc::SetPerformanceMode(true);
				if (TWFunc::Path_Exists(pb_installed = DataManager::GetCurrentStoragePath() + "/PBRP/tools/magisk.zip"))
					flash_zip(pb_installed, &w);
				else
					flash_zip("/sdcard/PBRP/tools/magisk.zip", &w);
				TWFunc::SetPerformanceMode(false);
			}
		}
#endif
		usleep(50000);
		gui_print_color("normal", "* * * * * * * * * * * * * * * * * *\n");
		gui_msg(Msg(msg::kHighlight, "pb_flashed=* New PBRP Flashed, press Volume {1}")("Down to Reboot to Recovery."));
		gui_msg(Msg(msg::kHighlight, "pb_rb_msg=*Volume {1} to Finish")("Up"));
		gui_print_color("normal", "* * * * * * * * * * * * * * * * * *\n");
		pbRet = keypressed();
		if (pbRet == 41) {
			gui_highlight("pb_saving_log=Preserving Logs...\n");
			copylog(zip);
			operation_end(0);
			DataManager::SetValue("tw_sleep","5");
			DataManager::SetValue("tw_install_reboot_recovery", "1");
			gui_changePage(gui_parse_text("flash_sleep_and_reboot"));
		}
	}

	if (zip != "recovery.img")
		unlink(insf.c_str());

	return pbRet;
}

int GUIAction::flash(std::string arg)
{
	backup_before_flash();
	int i, ret_val = 0, wipe_cache = 0;
	string zip_filename = "";
	// We're going to jump to this page first, like a loading page
	gui_changePage(arg);
	for (i=0; i<zip_queue_index; i++) {
		string zip_path = zip_queue[i];
		size_t slashpos = zip_path.find_last_of('/');
		zip_filename = (slashpos == string::npos) ? zip_path : zip_path.substr(slashpos + 1);
		operation_start("Flashing");
		if((zip_path.substr(zip_path.size() - 4, 4))=="ozip")
		{
			int ret = ozip_decrypt(zip_path);
			if (ret == -2)
			{
				LOGERR("Key is not compatibile\n");
				break;
			}
			else if (ret != -1)
			{
				zip_filename = (zip_filename.substr(0, zip_filename.size() - 4)).append("zip");
				zip_path = (zip_path.substr(0, zip_path.size() - 4)).append("zip");
				if (!TWFunc::Path_Exists(zip_path)) {
					LOGERR("Unable to find decrypted zip\n");
					break;
				}
			}
		}
		DataManager::SetValue("tw_filename", zip_path);
		DataManager::SetValue("tw_file", zip_filename);
		DataManager::SetValue(TW_ZIP_INDEX, (i + 1));

		TWFunc::SetPerformanceMode(true);
		if (PartitionManager.Get_Super_Status())
			PartitionManager.UnMount_Main_Partitions();
		ret_val = flash_zip(zip_path, &wipe_cache);
		TWFunc::SetPerformanceMode(false);

		if (ret_val != 0) {
			gui_msg(Msg(msg::kError, "zip_err=Error installing zip file '{1}'")(zip_path));
			ret_val = 1;
			break;
		}
		keycheck(zip_filename, wipe_cache);
	}
	zip_queue_index = 0;

	if (wipe_cache) {
		gui_msg("zip_wipe_cache=One or more zip requested a cache wipe -- Wiping cache now.");
		PartitionManager.Wipe_By_Path("/cache");
	}

        if (reinject_after_flash() == 0) {
		PartitionManager.Update_System_Details();
	}
	if (DataManager::GetIntValue(PB_INSTALL_PREBUILT_ZIP) != 1)
	{
		if (DataManager::GetIntValue(PB_CALL_DEACTIVATION) != 0 && ret_val != 1)//get to know whether everything is ok or not
		{
			TWFunc::Deactivation_Process();
		}
		DataManager::SetValue(PB_CALL_DEACTIVATION, 0);
	}
	gui_highlight("pb_saving_log=Preserving Logs...\n");
	copylog(zip_filename);
	operation_end(ret_val);
	// This needs to be after the operation_end call so we change pages before we change variables that we display on the screen
	DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
	return 0;
}

int GUIAction::wipe(std::string arg)
{
	operation_start("Format");
	DataManager::SetValue("tw_partition", arg);
	int ret_val = false;

	if (simulate) {
		simulate_progress_bar();
	} else {
		if (arg == "data")
			ret_val = PartitionManager.Factory_Reset();
		else if (arg == "battery")
			ret_val = PartitionManager.Wipe_Battery_Stats();
		else if (arg == "rotate")
			ret_val = PartitionManager.Wipe_Rotate_Data();
		else if (arg == "dalvik")
			ret_val = PartitionManager.Wipe_Dalvik_Cache();
		else if (arg == "DATAMEDIA") {
			ret_val = PartitionManager.Format_Data();
		} else if (arg == "INTERNAL") {
			int has_datamedia;

			DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
			if (has_datamedia) {
				ret_val = PartitionManager.Wipe_Media_From_Data();
			} else {
				ret_val = PartitionManager.Wipe_By_Path(DataManager::GetSettingsStoragePath());
			}
		} else if (arg == "EXTERNAL") {
			string External_Path;

			DataManager::GetValue(TW_EXTERNAL_PATH, External_Path);
			ret_val = PartitionManager.Wipe_By_Path(External_Path);
		} else if (arg == "ANDROIDSECURE") {
			ret_val = PartitionManager.Wipe_Android_Secure();
		} else if (arg == "LIST") {
			string Wipe_List, wipe_path;
			bool skip = false;
			ret_val = true;

			DataManager::GetValue("tw_wipe_list", Wipe_List);
			LOGINFO("wipe list '%s'\n", Wipe_List.c_str());
			if (!Wipe_List.empty()) {
				size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
				while (end_pos != string::npos && start_pos < Wipe_List.size()) {
					wipe_path = Wipe_List.substr(start_pos, end_pos - start_pos);
					LOGINFO("wipe_path '%s'\n", wipe_path.c_str());
					if (wipe_path == "/and-sec") {
						if (!PartitionManager.Wipe_Android_Secure()) {
							gui_msg("and_sec_wipe_err=Unable to wipe android secure");
							ret_val = false;
							break;
						} else {
							skip = true;
						}
					} else if (wipe_path == "DALVIK") {
						if (!PartitionManager.Wipe_Dalvik_Cache()) {
							gui_err("dalvik_wipe_err=Failed to wipe dalvik");
							ret_val = false;
							break;
						} else {
							skip = true;
						}
                                        } else if (wipe_path == "SUBSTRATUM") {
                                                if (!PartitionManager.Wipe_Substratum_Overlays()) {
                                                        gui_err("pb_substratum_wipe_err=Failed to wipe substratum overlays");
                                                        ret_val = false;
			                                break;
                                                } else {
			                                skip = true;
                                                }
                                        } else if (wipe_path == "MAGISK") {
                                                if (!PartitionManager.Wipe_Magisk_Modules()) {
                                                        gui_err("pb_magisk_wipe_err=Failed to wipe magisk modules");
                                                        ret_val = false;
                                                        break;
                                                } else {
			                                skip = true;
                                                }
                                        } else if (wipe_path == "FBE" && DataManager::GetIntValue(TW_IS_FBE)) {
                                                if (!PartitionManager.Wipe_FBE_Cache()) {
                                                        gui_err("pb_fbe_wipe_err=Failed to wipe fbe cache");
                                                        ret_val = false;
                                                        break;
                                                } else {
			                                skip = true;
                                                }
                                        } else if (wipe_path == "INTERNAL") {
						if (!PartitionManager.Wipe_Media_From_Data()) {
							ret_val = false;
							break;
						} else {
							skip = true;
						}
					}
					if (!skip) {
						if (!PartitionManager.Wipe_By_Path(wipe_path)) {
							gui_msg(Msg(msg::kError, "unable_to_wipe=Unable to wipe {1}.")(wipe_path));
							ret_val = false;
							break;
						} else if (wipe_path == DataManager::GetSettingsStoragePath()) {
							arg = wipe_path;
						}
					} else {
						skip = false;
					}
					start_pos = end_pos + 1;
					end_pos = Wipe_List.find(";", start_pos);
				}
			}
		} else
			ret_val = PartitionManager.Wipe_By_Path(arg);
#ifndef TW_OEM_BUILD
		if (arg == DataManager::GetSettingsStoragePath()) {
			// If we wiped the settings storage path, recreate the TWRP folder and dump the settings
			string Storage_Path = DataManager::GetSettingsStoragePath();

			if (PartitionManager.Mount_By_Path(Storage_Path, true)) {
				LOGINFO("Making PBRP folder and saving settings.\n");
				Storage_Path += "/PBRP";
				mkdir(Storage_Path.c_str(), 0777);
				DataManager::Flush();
			} else {
				LOGERR("Unable to recreate PBRP folder and save settings.\n");
			}
		}
#endif
	}
	PartitionManager.Update_System_Details();
	if (ret_val)
		ret_val = 0; // 0 is success
	else
		ret_val = 1; // 1 is failure
	operation_end(ret_val);
	return 0;
}

int GUIAction::refreshsizes(std::string arg __unused)
{
	operation_start("Refreshing Sizes");
	if (simulate) {
		simulate_progress_bar();
	} else
		PartitionManager.Update_System_Details();
	operation_end(0);
	return 0;
}

int GUIAction::nandroid(std::string arg)
{
	if (simulate) {
		PartitionManager.stop_backup.set_value(0);
		DataManager::SetValue("tw_partition", "Simulation");
		simulate_progress_bar();
		operation_end(0);
	} else {
		operation_start("Nandroid");
		int ret = 0;

		if (arg == "backup") {
			string Backup_Name;
			DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
			string auto_gen = gui_lookup("auto_generate", "(Auto Generate)");
			if (Backup_Name == auto_gen || Backup_Name == gui_lookup("curr_date", "(Current Date)") || Backup_Name == "0" || Backup_Name == "(" || PartitionManager.Check_Backup_Name(Backup_Name, true, true) == 0) {
				ret = PartitionManager.Run_Backup(false);
				DataManager::SetValue("tw_encrypt_backup", 0); // reset value so we don't encrypt every subsequent backup
				if (!PartitionManager.stop_backup.get_value()) {
					if (ret == false)
						ret = 1; // 1 for failure
					else
						ret = 0; // 0 for success
					DataManager::SetValue("tw_cancel_backup", 0);
				} else {
					DataManager::SetValue("tw_cancel_backup", 1);
					gui_msg("backup_cancel=Backup Cancelled");
					ret = 0;
				}
			} else {
				operation_end(1);
				return -1;
			}
			DataManager::SetValue(TW_BACKUP_NAME, auto_gen);
		} else if (arg == "restore") {
			string Restore_Name;
			int gui_adb_backup;

			DataManager::GetValue("tw_restore", Restore_Name);
			DataManager::GetValue("tw_enable_adb_backup", gui_adb_backup);
			if (gui_adb_backup) {
				DataManager::SetValue("tw_operation_state", 1);
				if (TWFunc::stream_adb_backup(Restore_Name) == 0)
					ret = 0; // success
				else
					ret = 1; // failure
				DataManager::SetValue("tw_enable_adb_backup", 0);
				ret = 0; // assume success???
			} else {
				if (PartitionManager.Run_Restore(Restore_Name))
					ret = 0; // success
				else
					ret = 1; // failure
			}
		} else {
			operation_end(1); // invalid arg specified, fail
			return -1;
		}
		operation_end(ret);
		return ret;
	}
	return 0;
}

int GUIAction::cancelbackup(std::string arg __unused) {
	if (simulate) {
		PartitionManager.stop_backup.set_value(1);
	}
	else {
		int op_status = PartitionManager.Cancel_Backup();
		if (op_status != 0)
			op_status = 1; // failure
	}

	return 0;
}

int GUIAction::fixcontexts(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Fix Contexts");
	LOGINFO("fix contexts started!\n");
	if (simulate) {
		simulate_progress_bar();
	} else {
		op_status = PartitionManager.Fix_Contexts();
		if (op_status != 0)
			op_status = 1; // failure
	}
	operation_end(op_status);
	return 0;
}

int GUIAction::fixpermissions(std::string arg)
{
	return fixcontexts(arg);
}

int GUIAction::dd(std::string arg)
{
	operation_start("imaging");

	if (simulate) {
		simulate_progress_bar();
	} else {
		string cmd = "dd " + arg;
		TWFunc::Exec_Cmd(cmd);
	}
	operation_end(0);
	return 0;
}

int GUIAction::partitionsd(std::string arg __unused)
{
	operation_start("Partition SD Card");
	int ret_val = 0;

	if (simulate) {
		simulate_progress_bar();
	} else {
		int allow_partition;
		DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
		if (allow_partition == 0) {
			gui_err("no_real_sdcard=This device does not have a real SD Card! Aborting!");
		} else {
			if (!PartitionManager.Partition_SDCard())
				ret_val = 1; // failed
		}
	}
	operation_end(ret_val);
	return 0;

}

int GUIAction::cmd(std::string arg)
{
	int op_status = 0;

	operation_start("Command");
	LOGINFO("Running command: '%s'\n", arg.c_str());
	if (simulate) {
		simulate_progress_bar();
	} else {
		op_status = TWFunc::Exec_Cmd(arg);
		if (op_status != 0)
			op_status = 1;
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::terminalcommand(std::string arg)
{
	int op_status = 0;
	string cmdpath, command;

	DataManager::GetValue("tw_terminal_location", cmdpath);
	operation_start("CommandOutput");
	gui_print("%s # %s\n", cmdpath.c_str(), arg.c_str());
	if (simulate) {
		simulate_progress_bar();
		operation_end(op_status);
	} else if (arg == "exit") {
		LOGINFO("Exiting terminal\n");
		operation_end(op_status);
		page("main");
	} else {
		command = "cd \"" + cmdpath + "\" && " + arg + " 2>&1";;
		LOGINFO("Actual command is: '%s'\n", command.c_str());
		DataManager::SetValue("tw_terminal_state", 1);
		DataManager::SetValue("tw_background_thread_running", 1);
		FILE* fp;
		char line[512];

		fp = popen(command.c_str(), "r");
		if (fp == NULL) {
			LOGERR("Error opening command to run (%s).\n", strerror(errno));
		} else {
			int fd = fileno(fp), has_data = 0, check = 0, keep_going = -1;
			struct timeval timeout;
			fd_set fdset;

			while (keep_going)
			{
				FD_ZERO(&fdset);
				FD_SET(fd, &fdset);
				timeout.tv_sec = 0;
				timeout.tv_usec = 400000;
				has_data = select(fd+1, &fdset, NULL, NULL, &timeout);
				if (has_data == 0) {
					// Timeout reached
					DataManager::GetValue("tw_terminal_state", check);
					if (check == 0) {
						keep_going = 0;
					}
				} else if (has_data < 0) {
					// End of execution
					keep_going = 0;
				} else {
					// Try to read output
					if (fgets(line, sizeof(line), fp) != NULL)
						gui_print("%s", line); // Display output
					else
						keep_going = 0; // Done executing
				}
			}
			fclose(fp);
		}
		DataManager::SetValue("tw_operation_status", 0);
		DataManager::SetValue("tw_operation_state", 1);
		DataManager::SetValue("tw_terminal_state", 0);
		DataManager::SetValue("tw_background_thread_running", 0);
		DataManager::SetValue(TW_ACTION_BUSY, 0);
	}
	return 0;
}

int GUIAction::killterminal(std::string arg __unused)
{
	LOGINFO("Sending kill command...\n");
	operation_start("KillCommand");
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue("tw_terminal_state", 0);
	DataManager::SetValue("tw_background_thread_running", 0);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	return 0;
}

int GUIAction::checkbackupname(std::string arg __unused)
{
	int op_status = 0;

	operation_start("CheckBackupName");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string Backup_Name;
		DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
		op_status = PartitionManager.Check_Backup_Name(Backup_Name, true, true);
		if (op_status != 0)
			op_status = 1;
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::decrypt(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Decrypt");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string Password;
		string userID;
		DataManager::GetValue("tw_crypto_password", Password);

		if (DataManager::GetIntValue(TW_IS_FBE)) {  // for FBE
			DataManager::GetValue("tw_crypto_user_id", userID);
			if (userID != "") {
				op_status = PartitionManager.Decrypt_Device(Password, atoi(userID.c_str()));
				if (userID != "0") {
					if (op_status != 0)
						op_status = 1;
					operation_end(op_status);
	          		return 0;
				}
			} else {
				LOGINFO("User ID not found\n");
				op_status = 1;
			}
		::sleep(1);
		} else {  // for FDE
			op_status = PartitionManager.Decrypt_Device(Password);
		}

		if (op_status != 0)
			op_status = 1;
		else {
			DataManager::SetValue(TW_IS_ENCRYPTED, 0);

			int has_datamedia;

			// Check for a custom theme and load it if exists
			DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
			if (has_datamedia != 0) {
				if (tw_get_default_metadata(DataManager::GetSettingsStoragePath().c_str()) != 0) {
					LOGINFO("Failed to get default contexts and file mode for storage files.\n");
				} else {
					LOGINFO("Got default contexts and file mode for storage files.\n");
				}
			}
			PartitionManager.Decrypt_Adopted();
		}
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::adbsideload(std::string arg __unused)
{
	operation_start("Sideload");
	if (simulate) {
		simulate_progress_bar();
		operation_end(0);
	} else {
		gui_msg("start_sideload=Starting ADB sideload feature...");
		bool mtp_was_enabled = TWFunc::Toggle_MTP(false);

		// wait for the adb connection
		Device::BuiltinAction reboot_action = Device::REBOOT_BOOTLOADER;
		int ret = twrp_sideload("/", &reboot_action);
		sideload_child_pid = GetMiniAdbdPid();
		DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui now that the zip install is going to start

		if (ret != 0) {
			if (ret == -2)
				gui_msg("need_new_adb=You need adb 1.0.32 or newer to sideload to this device.");
			ret = 1; // failure
		} else {
			int wipe_cache = 0;
			int wipe_dalvik = 0;
			DataManager::GetValue("tw_wipe_dalvik", wipe_dalvik);
			if (wipe_cache || DataManager::GetIntValue("tw_wipe_cache"))
				PartitionManager.Wipe_By_Path("/cache");
			if (wipe_dalvik)
				PartitionManager.Wipe_Dalvik_Cache();
		}
		TWFunc::Toggle_MTP(mtp_was_enabled);
		operation_end(ret);
	}
	return 0;
}

int GUIAction::adbsideloadcancel(std::string arg __unused)
{
	struct stat st;
	DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui
	gui_msg("cancel_sideload=Cancelling ADB sideload...");
	LOGINFO("Signaling child sideload process to exit.\n");
	// Calling stat() on this magic filename signals the minadbd
	// subprocess to shut down.
	stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &st);
	sideload_child_pid = GetMiniAdbdPid();
	if (!sideload_child_pid) {
		LOGERR("Unable to get child ID\n");
		return 0;
	}
	::sleep(1);
	LOGINFO("Killing child sideload process.\n");
	kill(sideload_child_pid, SIGTERM);
	int status;
	LOGINFO("Waiting for child sideload process to exit.\n");
	waitpid(sideload_child_pid, &status, 0);
	sideload_child_pid = 0;
	DataManager::SetValue("tw_page_done", "1"); // For OpenRecoveryScript support
	return 0;
}

int GUIAction::openrecoveryscript(std::string arg __unused)
{
	operation_start("OpenRecoveryScript");
	if (simulate) {
		simulate_progress_bar();
		operation_end(0);
	} else {
		int op_status = OpenRecoveryScript::Run_OpenRecoveryScript_Action();
		operation_end(op_status);
	}
	return 0;
}

int GUIAction::installsu(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Install SuperSU");
	if (simulate) {
		simulate_progress_bar();
	} else {
		LOGERR("Installing SuperSU was deprecated from TWRP.\n");
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::fixsu(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Fixing Superuser Permissions");
	if (simulate) {
		simulate_progress_bar();
	} else {
		LOGERR("Fixing su permissions was deprecated from TWRP.\n");
		LOGERR("4.3+ ROMs with SELinux will always lose su perms.\n");
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::decrypt_backup(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Try Restore Decrypt");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string Restore_Path, Filename, Password;
		DataManager::GetValue("tw_restore", Restore_Path);
		Restore_Path += "/";
		DataManager::GetValue("tw_restore_password", Password);
		TWFunc::SetPerformanceMode(true);
		if (TWFunc::Try_Decrypting_Backup(Restore_Path, Password))
			op_status = 0; // success
		else
			op_status = 1; // fail
		TWFunc::SetPerformanceMode(false);
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::repair(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Repair Partition");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string part_path;
		DataManager::GetValue("tw_partition_mount_point", part_path);
		if (PartitionManager.Repair_By_Path(part_path, true)) {
			op_status = 0; // success
		} else {
			op_status = 1; // fail
		}
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::resize(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Resize Partition");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string part_path;
		DataManager::GetValue("tw_partition_mount_point", part_path);
		if (PartitionManager.Resize_By_Path(part_path, true)) {
			op_status = 0; // success
		} else {
			op_status = 1; // fail
		}
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::changefilesystem(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Change File System");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string part_path, file_system;
		DataManager::GetValue("tw_partition_mount_point", part_path);
		DataManager::GetValue("tw_action_new_file_system", file_system);
		if (PartitionManager.Wipe_By_Path(part_path, file_system)) {
			op_status = 0; // success
		} else {
			gui_err("change_fs_err=Error changing file system.");
			op_status = 1; // fail
		}
	}
	PartitionManager.Update_System_Details();
	operation_end(op_status);
	return 0;
}

int GUIAction::startmtp(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Start MTP");
	if (PartitionManager.Enable_MTP())
		op_status = 0; // success
	else
		op_status = 1; // fail

	operation_end(op_status);
	return 0;
}

int GUIAction::stopmtp(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Stop MTP");
	if (PartitionManager.Disable_MTP())
		op_status = 0; // success
	else
		op_status = 1; // fail

	operation_end(op_status);
	return 0;
}

int GUIAction::flashimage(std::string arg __unused)
{
	DataManager::SetValue("ui_progress", 0);
	int op_status = 0;

	operation_start("Flash Image");
	string path, filename, partition;
	DataManager::GetValue("tw_zip_location", path);
	DataManager::GetValue("tw_file", filename);
	if (simulate) {
		simulate_progress_bar();
	} else {
	if (PartitionManager.Flash_Image(path, filename))
		op_status = 0; // success
	else
		op_status = 1; // fail
	}
	// Start Deactivation on flashing either boot.img, system.img or vendor.img
	if (DataManager::GetIntValue(PB_CALL_DEACTIVATION) != 0)
	{
		TWFunc::Deactivation_Process();
	}
	DataManager::SetValue(PB_CALL_DEACTIVATION, 0);
	DataManager::GetValue("tw_flash_partition", partition);
	if (partition == "/repack_ramdisk;" || partition == "/repack_kernel;" || partition == "/recovery;")
		keycheck("recovery.img");
	operation_end(op_status);
	DataManager::SetValue("ui_progress", 100);
	DataManager::SetValue("ui_progress", 0);
	return 0;
}

int GUIAction::twcmd(std::string arg)
{
	operation_start("TWRP CLI Command");
	if (simulate)
		simulate_progress_bar();
	else
		OpenRecoveryScript::Run_CLI_Command(arg.c_str());
	operation_end(0);
	return 0;
}

int GUIAction::getKeyByName(std::string key)
{
	if (key == "home")		return KEY_HOMEPAGE;  // note: KEY_HOME is cursor movement (like KEY_END)
	else if (key == "menu")		return KEY_MENU;
	else if (key == "back")	 	return KEY_BACK;
	else if (key == "search")	return KEY_SEARCH;
	else if (key == "voldown")	return KEY_VOLUMEDOWN;
	else if (key == "volup")	return KEY_VOLUMEUP;
	else if (key == "power") {
		int ret_val;
		DataManager::GetValue(TW_POWER_BUTTON, ret_val);
		if (!ret_val)
			return KEY_POWER;
		else
			return ret_val;
	}

	return atol(key.c_str());
}

int GUIAction::checkpartitionlifetimewrites(std::string arg)
{
	int op_status = 0;
	TWPartition* sys = PartitionManager.Find_Partition_By_Path(arg);

	operation_start("Check Partition Lifetime Writes");
	if (sys) {
		if (sys->Check_Lifetime_Writes() != 0)
			DataManager::SetValue("tw_lifetime_writes", 1);
		else
			DataManager::SetValue("tw_lifetime_writes", 0);
		op_status = 0; // success
	} else {
		DataManager::SetValue("tw_lifetime_writes", 1);
		op_status = 1; // fail
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::mountsystemtoggle(std::string arg)
{
	int op_status = 0;
	bool remount_system = PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path());
	bool remount_vendor = PartitionManager.Is_Mounted_By_Path("/vendor");

	operation_start("Toggle System Mount");
	if (!PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), true)) {
		op_status = 1; // fail
	} else {
		TWPartition* Part = PartitionManager.Find_Partition_By_Path(PartitionManager.Get_Android_Root_Path());
		if (Part) {
			if (arg == "0") {
				DataManager::SetValue("tw_mount_system_ro", 0);
				Part->Change_Mount_Read_Only(false);
			} else {
				DataManager::SetValue("tw_mount_system_ro", 1);
				Part->Change_Mount_Read_Only(true);
			}
			if (remount_system) {
				Part->Mount(true);
			}
			op_status = 0; // success
		} else {
			op_status = 1; // fail
		}
		Part = PartitionManager.Find_Partition_By_Path("/vendor");
		if (Part) {
			if (arg == "0") {
				Part->Change_Mount_Read_Only(false);
			} else {
				Part->Change_Mount_Read_Only(true);
			}
			if (remount_vendor) {
				Part->Mount(true);
			}
			op_status = 0; // success
		} else {
			op_status = 1; // fail
		}
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::setlanguage(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Set Language");
	PageManager::LoadLanguage(DataManager::GetStrValue("tw_language"));
	PageManager::RequestReload();
	op_status = 0; // success

	operation_end(op_status);
	return 0;
}

int GUIAction::togglebacklight(std::string arg __unused)
{
	if (!mime)
		blankTimer.toggleBlank();
	return 0;
}

int GUIAction::setbootslot(std::string arg)
{
	operation_start("Set Boot Slot");
	if (!simulate) {
		if (!PartitionManager.UnMount_By_Path("/vendor", false)) {
			// PartitionManager failed to unmount /vendor, this should not happen,
			// but in case it does, do a lazy unmount
			LOGINFO("WARNING: vendor partition could not be unmounted normally!\n");
			umount2("/vendor", MNT_DETACH);
			PartitionManager.Set_Active_Slot(arg);
		} else {
			PartitionManager.Set_Active_Slot(arg);
		}
	} else {
		simulate_progress_bar();
	}
	operation_end(0);
	return 0;
}

int GUIAction::checkforapp(std::string arg __unused)
{
	operation_start("Check for TWRP App");
	if (!simulate)
	{
		string sdkverstr = TWFunc::System_Property_Get("ro.build.version.sdk");
		int sdkver = 0;
		if (!sdkverstr.empty()) {
			sdkver = atoi(sdkverstr.c_str());
		}
		if (sdkver <= 13) {
			if (sdkver == 0)
				LOGINFO("Unable to read sdk version from build prop\n");
			else
				LOGINFO("SDK version too low for TWRP app (%i < 14)\n", sdkver);
			DataManager::SetValue("tw_app_install_status", 1); // 0 = no status, 1 = not installed, 2 = already installed or do not install
			goto exit;
		}
		if (PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false)) {
			string base_path = PartitionManager.Get_Android_Root_Path();
			if (TWFunc::Path_Exists(PartitionManager.Get_Android_Root_Path() + "/system"))
				base_path += "/system"; // For devices with system as a root file system (e.g. Pixel)
			string install_path = base_path + "/priv-app";
			if (!TWFunc::Path_Exists(install_path))
				install_path = base_path + "/app";
			install_path += "/twrpapp";
			if (TWFunc::Path_Exists(install_path)) {
				LOGINFO("App found at '%s'\n", install_path.c_str());
				DataManager::SetValue("tw_app_install_status", 2); // 0 = no status, 1 = not installed, 2 = already installed or do not install
				goto exit;
			}
		}
		if (PartitionManager.Mount_By_Path("/data", false)) {
			const char parent_path[] = "/data/app";
			const char app_prefix[] = "me.twrp.twrpapp-";
			DIR *d = opendir(parent_path);
			if (d) {
				struct dirent *p;
				while ((p = readdir(d))) {
					if (p->d_type != DT_DIR || strlen(p->d_name) < strlen(app_prefix) || strncmp(p->d_name, app_prefix, strlen(app_prefix)))
						continue;
					closedir(d);
					LOGINFO("App found at '%s/%s'\n", parent_path, p->d_name);
					DataManager::SetValue("tw_app_install_status", 2); // 0 = no status, 1 = not installed, 2 = already installed or do not install
					goto exit;
				}
				closedir(d);
			}
		} else {
			LOGINFO("Data partition cannot be mounted during app check\n");
			DataManager::SetValue("tw_app_install_status", 2); // 0 = no status, 1 = not installed, 2 = already installed or do not install
		}
	} else
		simulate_progress_bar();
	LOGINFO("App not installed\n");
	DataManager::SetValue("tw_app_install_status", 1); // 0 = no status, 1 = not installed, 2 = already installed
exit:
	operation_end(0);
	return 0;
}

int GUIAction::readfile(std::string arg __unused)
{
	if (simulate)
	{
		simulate_progress_bar();
	}
	else {
		operation_start("Started Process Read File");
		string name = "";
		DataManager::GetValue("tw_filename1", name);
		ifstream file(name);
		if (file.is_open()) {
			gui_msg(Msg(msg::kProcess, "pb_start_read=Started Process Read {1}")(name));
			string line;
			while (getline(file, line)) {
				gui_print("%s", line.c_str());
			}
			file.close();
		}
		gui_msg(Msg(msg::kProcess, "pb_end_read=Ended Process Read {1}")(name));
	}
	operation_end(0);
	return 0;
}
int GUIAction::installapp(std::string arg __unused)
{
	int op_status = 1;
	operation_start("Install TWRP App");
	if (!simulate)
	{
		if (DataManager::GetIntValue("tw_mount_system_ro") > 0 || DataManager::GetIntValue("tw_app_install_system") == 0) {
			if (PartitionManager.Mount_By_Path("/data", true)) {
				string install_path = "/data/app";
				string context = "u:object_r:apk_data_file:s0";
				if (!TWFunc::Path_Exists(install_path)) {
					if (mkdir(install_path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) {
						LOGERR("Error making %s directory: %s\n", install_path.c_str(), strerror(errno));
						goto exit;
					}
					if (chown(install_path.c_str(), 1000, 1000)) {
						LOGERR("chown %s error: %s\n", install_path.c_str(), strerror(errno));
						goto exit;
					}
					if (setfilecon(install_path.c_str(), (security_context_t)context.c_str()) < 0) {
						LOGERR("setfilecon %s error: %s\n", install_path.c_str(), strerror(errno));
						goto exit;
					}
				}
				install_path += "/me.twrp.twrpapp-1";
				if (mkdir(install_path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) {
					LOGERR("Error making %s directory: %s\n", install_path.c_str(), strerror(errno));
					goto exit;
				}
				if (chown(install_path.c_str(), 1000, 1000)) {
					LOGERR("chown %s error: %s\n", install_path.c_str(), strerror(errno));
					goto exit;
				}
				if (setfilecon(install_path.c_str(), (security_context_t)context.c_str()) < 0) {
					LOGERR("setfilecon %s error: %s\n", install_path.c_str(), strerror(errno));
					goto exit;
				}
				install_path += "/base.apk";
				if (TWFunc::copy_file("/system/bin/me.twrp.twrpapp.apk", install_path, 0644)) {
					LOGERR("Error copying apk file\n");
					goto exit;
				}
				if (chown(install_path.c_str(), 1000, 1000)) {
					LOGERR("chown %s error: %s\n", install_path.c_str(), strerror(errno));
					goto exit;
				}
				if (setfilecon(install_path.c_str(), (security_context_t)context.c_str()) < 0) {
					LOGERR("setfilecon %s error: %s\n", install_path.c_str(), strerror(errno));
					goto exit;
				}
				sync();
				sync();
			}
		} else {
			if (PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), true)) {
				string base_path = PartitionManager.Get_Android_Root_Path();
				if (TWFunc::Path_Exists(PartitionManager.Get_Android_Root_Path() + "/system"))
					base_path += "/system"; // For devices with system as a root file system (e.g. Pixel)
				string install_path = base_path + "/priv-app";
				string context = "u:object_r:system_file:s0";
				if (!TWFunc::Path_Exists(install_path))
					install_path = base_path + "/app";
				if (TWFunc::Path_Exists(install_path)) {
					install_path += "/twrpapp";
					LOGINFO("Installing app to '%s'\n", install_path.c_str());
					if (mkdir(install_path.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == 0) {
						if (setfilecon(install_path.c_str(), (security_context_t)context.c_str()) < 0) {
							LOGERR("setfilecon %s error: %s\n", install_path.c_str(), strerror(errno));
							goto exit;
						}
						install_path += "/me.twrp.twrpapp.apk";
						if (TWFunc::copy_file("/system/bin/me.twrp.twrpapp.apk", install_path, 0644)) {
							LOGERR("Error copying apk file\n");
							goto exit;
						}
						if (setfilecon(install_path.c_str(), (security_context_t)context.c_str()) < 0) {
							LOGERR("setfilecon %s error: %s\n", install_path.c_str(), strerror(errno));
							goto exit;
						}
						sync();
						sync();
						PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), true);
						op_status = 0;
					} else {
						LOGERR("Error making app directory '%s': %s\n", strerror(errno));
					}
				}
			}
		}
	} else
		simulate_progress_bar();
exit:
	operation_end(0);
	return 0;
}

int GUIAction::unpack(std::string arg __unused)
{
	operation_start("Prepartion to unpack");
	if (simulate) {
		simulate_progress_bar();
	} else {
		if (DataManager::GetIntValue("tw_has_boot_slots")) {
			DataManager::SetValue("pb_theming_mode", "1");
			backup_before_flash();
			TWFunc::Unpack_Image("/tmp/boot.img", false);
		}
		else
			TWFunc::Unpack_Image("/recovery");
	}
	operation_end(0);
	return 0;
}

int GUIAction::repack(std::string arg __unused)
{
	operation_start("Repacking done");
	if (simulate) {
		simulate_progress_bar();
	} else {
		DataManager::SetValue("pb_theming_mode", "1");
		if (DataManager::GetIntValue("tw_has_boot_slots")) {
			if(!TWFunc::Repack_Image("/tmp/boot.img", false))
				LOGINFO("Repack: failed to repack Ramdisk\n");
			reinject_after_flash();
			DataManager::SetValue("pb_theming_mode", "0");
		}
		else
			TWFunc::Repack_Image("/recovery");
	}
	operation_end(0);
	return 0;
}

int GUIAction::flashlight(std::string arg __unused)
{
	int br_value = DataManager::GetIntValue("pb_bright_value");
	string str_val, file, flashp1 = "/sys/class/leds", flashp2 = "/flashlight", flashpath;
	string bright = "/brightness";
	string switch_path = TWFunc::Path_Exists(flashp1 + "/led:switch" + bright) ? (flashp1 + "/led:switch") : (flashp1 + "/led:switch_0");
	DIR* d;
	struct dirent* de __attribute__((unused));
#ifdef PB_MAX_BRIGHT_VALUE
	br_value = PB_MAX_BRIGHT_VALUE;
	DataManager::SetValue("pb_torch_brightness_slider", "0");
#endif
#ifdef PB_TORCH_PATH
	flashpath = PB_TORCH_PATH;
	LOGINFO("flashlight: Custom Node located at '%s'\n", flashpath.c_str());
	if (TWFunc::Path_Exists(flashpath))
	{
		d = opendir(flashpath.c_str());
		if (d != NULL) {
			flashpath += bright;
			DataManager::SetValue("pb_flashlight_theme_support", "1");
		}
	}
#else
	flashpath = flashp1 + flashp2 + bright;
	if (!TWFunc::Path_Exists(flashpath))
	{
		d = opendir(flashp1.c_str());
		if (d == NULL)
		{
			LOGINFO("Unable to open '%s'\n", flashp1.c_str());
			return 0;
		}
		while ((de = readdir(d)) != NULL)
		{
			file = de->d_name;
			if(file.find("torch") != string::npos || file.find("torch_"))
			{
				flashpath = flashp1 + "/" + file + bright;
				break;
			}
		}
		closedir (d);
		LOGINFO("Detected Node located at  '%s'\n", flashpath.c_str());
		DataManager::SetValue("pb_flashlight_theme_support", "1");
	}
#endif
	str_val="";
	if (TWFunc::Path_Exists(flashpath)) {
		LOGINFO("Flashlight Node Located at '%s'\n", flashpath.c_str());
		if (DataManager::GetIntValue("pb_torch_on") == 1)
		{
			LOGINFO("Flashlight Turning Off\n");
			if (TWFunc::Path_Exists(switch_path))
				TWFunc::write_to_file(switch_path + bright, "0");
			TWFunc::write_to_file(flashpath, "0");
			DataManager::SetValue("pb_torch_on", "0");
		}
		else
		{
			LOGINFO("Flashlight Turning On\n");
			LOGINFO("Flashlight: Brightning value '%d'\n", br_value);
			TWFunc::write_to_file(flashpath, std::to_string(br_value));
			if (TWFunc::Path_Exists(switch_path))
				TWFunc::write_to_file(switch_path + bright, "1");
			DataManager::SetValue("pb_torch_on", "1");
		}
	} else {
		LOGINFO("Incorrect Flashlight Path\n");
		DataManager::SetValue("pb_torch_brightness_slider", "0");
	}
	return 0;
}

int GUIAction::repackimage(std::string arg __unused)
{
	int op_status = 1;
	twrpRepacker repacker;

	operation_start("Repack Image");
	if (!simulate)
	{
		std::string path = DataManager::GetStrValue("tw_filename");
		Repack_Options_struct Repack_Options;
		Repack_Options.Disable_Verity = false;
		Repack_Options.Disable_Force_Encrypt = false;
		Repack_Options.Backup_First = DataManager::GetIntValue("tw_repack_backup_first") != 0;
		if (DataManager::GetIntValue("tw_repack_kernel") == 1)
			Repack_Options.Type = REPLACE_KERNEL;
		else
			Repack_Options.Type = REPLACE_RAMDISK;
		if (!repacker.Repack_Image_And_Flash(path, Repack_Options))
			goto exit;
	} else
		simulate_progress_bar();
	op_status = 0;
exit:
	operation_end(op_status);
	return 0;
}

int GUIAction::reflashtwrp(std::string arg __unused)
{
	int op_status = 1;
	twrpRepacker repacker;

	operation_start("Repack Image");
	if (!simulate)
	{
		if (!repacker.Flash_Current_Twrp())
		goto exit;
	} else
		simulate_progress_bar();
	op_status = 0;
exit:
	operation_end(op_status);
	return 0;
}
int GUIAction::fixabrecoverybootloop(std::string arg __unused)
{
	int op_status = 1;
	twrpRepacker repacker;

	operation_start("Repack Image");
	if (!simulate)
	{
		if (!TWFunc::Path_Exists("/system/bin/magiskboot")) {
			LOGERR("Image repacking tool not present in this TWRP build!");
			goto exit;
		}
		DataManager::SetProgress(0);
		TWPartition* part = PartitionManager.Find_Partition_By_Path("/boot");
		if (part)
			gui_msg(Msg("unpacking_image=Unpacking {1}...")(part->Display_Name));
		else {
			gui_msg(Msg(msg::kError, "unable_to_locate=Unable to locate {1}.")("/boot"));
			goto exit;
		}
		if (!repacker.Backup_Image_For_Repack(part, REPACK_ORIG_DIR, DataManager::GetIntValue("tw_repack_backup_first") != 0, gui_lookup("repack", "Repack")))
			goto exit;
		DataManager::SetProgress(.25);
		gui_msg("fixing_recovery_loop_patch=Patching kernel...");
		std::string command = "cd " REPACK_ORIG_DIR " && /system/bin/magiskboot hexpatch kernel 77616E745F696E697472616D667300 736B69705F696E697472616D667300";
		if (TWFunc::Exec_Cmd(command) != 0) {
			gui_msg(Msg(msg::kError, "fix_recovery_loop_patch_error=Error patching kernel."));
			goto exit;
		}
		std::string header_path = REPACK_ORIG_DIR;
		header_path += "header";
		if (TWFunc::Path_Exists(header_path)) {
			command = "cd " REPACK_ORIG_DIR " && sed -i \"s|$(grep '^cmdline=' header | cut -d= -f2-)|$(grep '^cmdline=' header | cut -d= -f2- | sed -e 's/skip_override//' -e 's/  */ /g' -e 's/[ \t]*$//')|\" header";
			if (TWFunc::Exec_Cmd(command) != 0) {
				gui_msg(Msg(msg::kError, "fix_recovery_loop_patch_error=Error patching kernel."));
				goto exit;
			}
		}
		DataManager::SetProgress(.5);
		gui_msg(Msg("repacking_image=Repacking {1}...")(part->Display_Name));
		command = "cd " REPACK_ORIG_DIR " && /system/bin/magiskboot repack " REPACK_ORIG_DIR "boot.img";
		if (TWFunc::Exec_Cmd(command) != 0) {
			gui_msg(Msg(msg::kError, "repack_error=Error repacking image."));
			goto exit;
		}
		DataManager::SetProgress(.75);
		std::string path = REPACK_ORIG_DIR;
		std::string file = "new-boot.img";
		DataManager::SetValue("tw_flash_partition", "/boot;");
		if (!PartitionManager.Flash_Image(path, file)) {
			LOGINFO("Error flashing new image\n");
			goto exit;
		}
		DataManager::SetProgress(1);
		TWFunc::removeDir(REPACK_ORIG_DIR, false);
	} else
		simulate_progress_bar();
	op_status = 0;
exit:
	operation_end(op_status);
	return 0;
}

int GUIAction::change_codename(std::string arg __unused)
{
	operation_start("Codename Changing");
	string new_codename = arg;
	DataManager::SetProgress(0);
	//Removal of Newlines from values
	new_codename.erase(std::remove(new_codename.begin(), new_codename.end(), '\n'), new_codename.end());
	DataManager::SetProgress(50);
	TWFunc::Property_Override(PB_PROP_DEVICE, new_codename);
	DataManager::SetValue("pb_device", new_codename);
	DataManager::SetProgress(100);
	operation_end(0);
	return 0;
}

int GUIAction::getprop(std::string arg)
{
	DataManager::SetValue(PB_PROP_VALUE, TWFunc::getprop(arg));
	return 0;
}

int GUIAction::flush_up_console(std::string arg __unused)
{
	GUIConsole::Clear_For_Retranslation();
	return 0;
}

int GUIAction::change_root(std::string arg __unused)
{
	PartitionManager.Change_System_Root(DataManager::GetIntValue(PB_MOUNT_SYSTEM_AS_ROOT));
	return 0;
}

int GUIAction::change_terminal(std::string arg) {
//	uint8_t argc[] = new uint8_t[arg.size()];

	if (term != NULL) {
		for (uint8_t iter = 0; iter < arg.size(); iter++)
			term->NotifyCharInput(arg.at(iter));
		term->NotifyCharInput(13);
	}
	else
		LOGINFO("error\n");
	return 0;
}
int GUIAction::enableadb(std::string arg __unused) {
	android::base::SetProperty("sys.usb.config", "none");
	android::base::SetProperty("sys.usb.config", "adb");
	return 0;
}

int GUIAction::enablefastboot(std::string arg __unused) {
	android::base::SetProperty("sys.usb.config", "none");
	android::base::SetProperty("sys.usb.config", "fastboot");
	return 0;
}

int GUIAction::unmapsuperdevices(std::string arg __unused) {
	int op_status = 1;

	operation_start("Remove Super Devices");
	if (simulate) {
		simulate_progress_bar();
	} else {
		if (PartitionManager.Unmap_Super_Devices()) {
			op_status = 0;
		}
	}

	operation_end(op_status);
	return 0;
}