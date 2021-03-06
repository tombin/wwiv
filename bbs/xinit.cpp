/**************************************************************************/
/*                                                                        */
/*                              WWIV Version 5.x                          */
/*             Copyright (C)1998-2017, WWIV Software Services             */
/*                                                                        */
/*    Licensed  under the  Apache License, Version  2.0 (the "License");  */
/*    you may not use this  file  except in compliance with the License.  */
/*    You may obtain a copy of the License at                             */
/*                                                                        */
/*                http://www.apache.org/licenses/LICENSE-2.0              */
/*                                                                        */
/*    Unless  required  by  applicable  law  or agreed to  in  writing,   */
/*    software  distributed  under  the  License  is  distributed on an   */
/*    "AS IS"  BASIS, WITHOUT  WARRANTIES  OR  CONDITIONS OF ANY  KIND,   */
/*    either  express  or implied.  See  the  License for  the specific   */
/*    language governing permissions and limitations under the License.   */
/*                                                                        */
/**************************************************************************/
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif  // _WIN32

#include "bbs/arword.h"
#include "bbs/bbs.h"
#include "bbs/bbsutl.h"
#include "bbs/connect1.h"
#include "bbs/events.h"
#include "bbs/execexternal.h"
#include "bbs/vars.h"
#include "bbs/conf.h"
#include "bbs/datetime.h"
#include "bbs/instmsg.h"
#include "bbs/message_file.h"
#include "bbs/netsup.h"
#include "bbs/sysoplog.h"
#include "bbs/pause.h"
#include "bbs/utility.h"
#include "bbs/wconstants.h"
#include "bbs/workspace.h"
#include "sdk/status.h"
#include "core/datafile.h"
#include "core/inifile.h"
#include "core/log.h"
#include "core/strings.h"
#include "core/os.h"
#include "core/textfile.h"
#include "core/version.h"
#include "core/wwivassert.h"
#include "core/wwivport.h"

#include "sdk/config.h"
#include "sdk/filenames.h"
#include "sdk/names.h"
#include "sdk/networks.h"
#include "sdk/subxtr.h"
#include "sdk/msgapi/msgapi.h"
#include "sdk/msgapi/message_api_wwiv.h"

// Additional INI file function and structure
#include "bbs/xinitini.h"

struct ini_flags_type {
  int     strnum;
  bool    sense;
  uint32_t value;
};

using std::chrono::seconds;
using std::string;
using wwiv::bbs::TempDisablePause;
using namespace wwiv::core;
using namespace wwiv::os;
using namespace wwiv::strings;
using namespace wwiv::sdk;

uint32_t GetFlagsFromIniFile(IniFile& pIniFile, ini_flags_type * fs, int nFlagNumber, uint32_t flags);

void StatusManagerCallback(int i) {
  switch (i) {
  case WStatus::fileChangeNames:
  {
    // re-read names.lst
    if (a()->names()) {
      // We may not have the BBS initialized yet, so only
      // re-read the names file if it's changed from another node.
      a()->names()->Load();
    }
  } break;
  case WStatus::fileChangeUpload:
    break;
  case WStatus::fileChangePosts:
    a()->subchg = 1;
    break;
  case WStatus::fileChangeEmail:
    emchg = true;
    break;
  case WStatus::fileChangeNet:
  {
    set_net_num(a()->net_num());
  }
  break;
  }
}

// Turns a string into a bitmapped unsigned short flag for use with
// ExecuteExternalProgram calls.
static uint16_t str2spawnopt(const std::string& s) {
  uint16_t return_val = EFLAG_NONE;
  string ts = s;
  StringUpperCase(&ts);

  if (ts.find("NOHUP") != std::string::npos) {
    return_val |= EFLAG_NOHUP;
  }
  if (ts.find("COMIP") != std::string::npos) {
    return_val |= EFLAG_COMIO;
  }
  if (ts.find("FOSSIL") != std::string::npos) {
    return_val |= EFLAG_FOSSIL; // RF20020929
  }
  if (ts.find("NETPROG") != std::string::npos) {
    return_val |= EFLAG_NETPROG;
  }
  if (ts.find("STDIO") != std::string::npos) {
    return_val |= EFLAG_STDIO;
  }
  return return_val;
}

// Takes string s and creates restrict val
static uint16_t str2restrict(const char *s) {
  const char *rs = restrict_string;
  char s1[81];

  strcpy(s1, s);
  strupr(s1);
  uint16_t r = 0;
  for (int i = strlen(rs) - 1; i >= 0; i--) {
    if (strchr(s1, rs[i])) {
      r |= (1 << i);
    }
  }

  return r;
}

// Reads WWIV.INI info from [WWIV] subsection, overrides some config.dat
// settings (as appropriate), updates config.dat with those values. Also
// tries to read settings from [WWIV-<instnum>] subsection - this overrides
// those in [WWIV] subsection.

struct eventinfo_t {
  const char *name;
  unsigned short eflags;
};

// See #defines SPAWNOPT_XXXX in vardec.h for these.
static eventinfo_t eventinfo[] = {
  {"TIMED",         EFLAG_NONE},
  {"NEWUSER",       EFLAG_NONE },
  {"BEGINDAY",      EFLAG_NONE },
  {"LOGON",         EFLAG_NONE},
  {"ULCHK",         EFLAG_NOHUP},
  {"CHAT",          EFLAG_FOSSIL},  // UNUSED (5)
  {"PROT_SINGLE",   EFLAG_NONE },
  {"PROT_BATCH",    EFLAG_NONE},
  {"CHAT",          EFLAG_NONE },
  {"ARCH_E",        EFLAG_NONE},
  {"ARCH_L",        EFLAG_NONE},
  {"ARCH_A",        EFLAG_NONE},
  {"ARCH_D",        EFLAG_NONE},
  {"ARCH_K",        EFLAG_NONE},
  {"ARCH_T",        EFLAG_NONE},
  {"NET_CMD1",      EFLAG_NETPROG},
  {"NET_CMD2",      EFLAG_NETPROG},
  {"LOGOFF",        EFLAG_NONE},
  {"",              EFLAG_NONE}, // UNUSED (18)
  {"NETWORK",       EFLAG_NETPROG},
};

static const char *get_key_str(int n, const char *index = nullptr) {
  static char str[255];
  if (!index) {
    return INI_OPTIONS_ARRAY[n];
  }
  sprintf(str, "%s[%s]", INI_OPTIONS_ARRAY[ n ], index);
  return str;
}

static void ini_init_str(IniFile& ini, size_t key_idx, std::string& s) {
  s = ini.value<string>(get_key_str(key_idx));
}

#define INI_GET_ASV(s, f, func, d) \
{ \
  const std::string ss = ini.value<std::string>(get_key_str(INI_STR_SIMPLE_ASV,s)); \
  if (!ss.empty()) { \
    asv.f = func (ss.c_str()); \
  } else { \
    asv.f = d; \
  }  \
}

#define NEL(s) (sizeof(s) / sizeof((s)[0]))

static ini_flags_type sysinfo_flags[] = {
  {INI_STR_FORCE_FBACK, false, OP_FLAGS_FORCE_NEWUSER_FEEDBACK},
  {INI_STR_CHECK_DUP_PHONES, false, OP_FLAGS_CHECK_DUPE_PHONENUM},
  {INI_STR_HANGUP_DUP_PHONES, false, OP_FLAGS_HANGUP_DUPE_PHONENUM},
  {INI_STR_USE_SIMPLE_ASV, false, OP_FLAGS_SIMPLE_ASV},
  {INI_STR_POSTTIME_COMPENS, false, OP_FLAGS_POSTTIME_COMPENSATE},
  {INI_STR_SHOW_HIER, false, OP_FLAGS_SHOW_HIER},
  {INI_STR_IDZ_DESC, false, OP_FLAGS_IDZ_DESC},
  {INI_STR_SETLDATE, false, OP_FLAGS_SETLDATE},
  {INI_STR_READ_CD_IDZ, false, OP_FLAGS_READ_CD_IDZ},
  {INI_STR_FSED_EXT_DESC, false, OP_FLAGS_FSED_EXT_DESC},
  {INI_STR_FAST_TAG_RELIST, false, OP_FLAGS_FAST_TAG_RELIST},
  {INI_STR_MAIL_PROMPT, false, OP_FLAGS_MAIL_PROMPT},
  {INI_STR_SHOW_CITY_ST, false, OP_FLAGS_SHOW_CITY_ST},
  //{INI_STR_NEW_EXTRACT, false, OP_FLAGS_NEW_EXTRACT},
  {INI_STR_FAST_SEARCH, false, OP_FLAGS_FAST_SEARCH},
  {INI_STR_NET_CALLOUT, false, OP_FLAGS_NET_CALLOUT},
  {INI_STR_WFC_SCREEN, false, OP_FLAGS_WFC_SCREEN},
  {INI_STR_FIDO_PROCESS, false, OP_FLAGS_FIDO_PROCESS},
  {INI_STR_NET_PROCESS, false, OP_FLAGS_NET_PROCESS},
  //{INI_STR_USER_REGISTRATION, false, OP_FLAGS_USER_REGISTRATION},
  {INI_STR_MSG_TAG, false, OP_FLAGS_MSG_TAG},
  {INI_STR_CHAIN_REG, false, OP_FLAGS_CHAIN_REG},
  {INI_STR_CAN_SAVE_SSM, false, OP_FLAGS_CAN_SAVE_SSM},
  {INI_STR_EXTRA_COLOR, false, OP_FLAGS_EXTRA_COLOR},
  {INI_STR_USE_FORCE_SCAN, false, OP_FLAGS_USE_FORCESCAN},
  {INI_STR_NEWUSER_MIN, false, OP_FLAGS_NEWUSER_MIN},
};

static ini_flags_type sysconfig_flags[] = {
  {INI_STR_2WAY_CHAT, false, sysconfig_2_way},
  {INI_STR_NO_NEWUSER_FEEDBACK, false, sysconfig_no_newuser_feedback},
  {INI_STR_TITLEBAR, false, sysconfig_titlebar},
  {INI_STR_LOG_DOWNLOADS, false, sysconfig_log_dl},
  {INI_STR_CLOSE_XFER, false, sysconfig_no_xfer},
  {INI_STR_ALL_UL_TO_SYSOP, false, sysconfig_all_sysop},
  {INI_STR_BEEP_CHAT, true, unused_sysconfig_no_beep},
  {INI_STR_TWO_COLOR_CHAT, false, unused_sysconfig_two_color},
  {INI_STR_ALLOW_ALIASES, true, sysconfig_no_alias},
  {INI_STR_USE_LIST, false, unused_sysconfig_list},
  {INI_STR_EXTENDED_USERINFO, false, sysconfig_extended_info},
  {INI_STR_FREE_PHONE, false, sysconfig_free_phone}
};

void Application::ReadINIFile(IniFile& ini) {
  // Setup default  data
  chatname_color_ = 95;
  message_color_ = 2;
  max_batch = 50;
  max_extend_lines = 10;
  max_chains = 50;
  max_gfilesec = 32;
  mail_who_field_len = 35;

  for (size_t i = 0; i < NEL(eventinfo); i++) {
    spawn_opts[ i ] = eventinfo[ i ].eflags;
  }

  // put in default Application::flags
  SetConfigFlags(OP_FLAGS_FIDO_PROCESS);
  if (instance_number() == 1) {
    // By default allow node1 to process wwivnet packets.
    SetConfigFlags(OP_FLAGS_NET_PROCESS);
  }

  if (ok_modem_stuff) {
    SetConfigFlag(OP_FLAGS_NET_CALLOUT);
  } else {
    ClearConfigFlag(OP_FLAGS_NET_CALLOUT);
  }

  // found something
  // pull out event flags
  for (size_t i = 0; i < NEL(spawn_opts); i++) {
    const string key_name = StringPrintf("%s[%s]", get_key_str(INI_STR_SPAWNOPT), eventinfo[i].name);
    const auto ss = ini.value<string>(key_name);
    if (!ss.empty()) {
      spawn_opts[i] = str2spawnopt(ss);
    }
  }

  // pull out newuser colors
  for (int i = 0; i < 10; i++) {
    auto num = ini.value<uint8_t>(StringPrintf("%s[%d]", get_key_str(INI_STR_NUCOLOR), i));
    if (num != 0) {
      newuser_colors[i] = num;
    }
    num = ini.value<uint8_t>(StringPrintf("%s[%d]", get_key_str(INI_STR_NUCOLORBW), i));
    if (num != 0) {
      newuser_bwcolors[i] = num;
    }
  }

  SetCarbonCopyEnabled(ini.value<bool>("ALLOW_CC_BCC"));

  // pull out sysop-side colors
  localIO()->SetTopScreenColor(ini.value<int>(
    get_key_str(INI_STR_TOPCOLOR), localIO()->GetTopScreenColor()));
  localIO()->SetUserEditorColor(ini.value<int>(
    get_key_str(INI_STR_F1COLOR), localIO()->GetUserEditorColor()));
  localIO()->SetEditLineColor(ini.value<int>(
    get_key_str(INI_STR_EDITLINECOLOR), localIO()->GetEditLineColor()));
  chatname_color_ = ini.value<int>(
    get_key_str(INI_STR_CHATSELCOLOR),GetChatNameSelectionColor());

  // pull out sizing options
  max_batch = ini.value<uint16_t>(get_key_str(INI_STR_MAX_BATCH), max_batch);
  max_extend_lines = ini.value<uint16_t>(get_key_str(INI_STR_MAX_EXTEND_LINES), max_extend_lines);
  max_chains = ini.value<uint16_t>(get_key_str(INI_STR_MAX_CHAINS), max_chains);
  max_gfilesec = ini.value<uint16_t>(get_key_str(INI_STR_MAX_GFILESEC), max_gfilesec);

  // pull out strings
  ini_init_str(ini, INI_STR_UPLOAD_CMD, upload_cmd);
  ini_init_str(ini, INI_STR_BEGINDAY_CMD, beginday_cmd);
  ini_init_str(ini, INI_STR_NEWUSER_CMD, newuser_cmd);
  ini_init_str(ini, INI_STR_LOGON_CMD, logon_cmd);
  ini_init_str(ini, INI_STR_TERMINAL_CMD, terminal_command);

  forced_read_subnum_ = ini.value<uint16_t>(get_key_str(INI_STR_FORCE_SCAN_SUBNUM), forced_read_subnum_);
  internal_zmodem_ = ini.value<bool>(get_key_str(INI_STR_INTERNALZMODEM), true);
  newscan_at_login_ = ini.value<bool>(get_key_str(INI_STR_NEW_SCAN_AT_LOGIN), true);
  exec_log_syncfoss_ = ini.value<bool>(get_key_str(INI_STR_EXEC_LOG_SYNCFOSS), false);
  exec_child_process_wait_time_ = ini.value<int>(get_key_str(INI_STR_EXEC_CHILD_WAIT_TIME), 500);
  beginday_node_number_ = ini.value<int>(get_key_str(INI_STR_BEGINDAYNODENUMBER), 1);

  // pull out sysinfo_flags
  SetConfigFlags(GetFlagsFromIniFile(ini, sysinfo_flags, NEL(sysinfo_flags), GetConfigFlags()));

  // allow override of Application::message_color_
  message_color_ = ini.value<int>(get_key_str(INI_STR_MSG_COLOR), GetMessageColor());

  // get asv values
  if (HasConfigFlag(OP_FLAGS_SIMPLE_ASV)) {
    INI_GET_ASV("SL", sl, to_number<uint8_t>, a()->asv.sl);
    INI_GET_ASV("DSL", dsl, to_number<uint8_t>, a()->asv.dsl);
    INI_GET_ASV("EXEMPT", exempt, to_number<uint8_t>, a()->asv.exempt);
    INI_GET_ASV("AR", ar, str_to_arword, a()->asv.ar);
    INI_GET_ASV("DAR", dar, str_to_arword, a()->asv.dar);
    INI_GET_ASV("RESTRICT", restrict, str2restrict, a()->asv.restrict);
  }

  // sysconfig flags
  a()->config()->set_sysconfig(static_cast<uint16_t>(GetFlagsFromIniFile(ini, sysconfig_flags,
      NEL(sysconfig_flags), a()->config()->sysconfig_flags())));

  // misc stuff
  auto num = ini.value<uint16_t>(get_key_str(INI_STR_MAIL_WHO_LEN));
  if (num > 0) { mail_who_field_len = num; }

  const auto ratio_str = ini.value<string>(get_key_str(INI_STR_RATIO));
  if (!ratio_str.empty()) {
    a()->config()->set_req_ratio(to_number<float>(ratio_str));
  }

  const auto attach_dir = ini.value<string>(get_key_str(INI_STR_ATTACH_DIR));
  attach_dir_ = (!attach_dir.empty()) ? attach_dir : FilePath(GetHomeDir(), ATTACH_DIR);
  File::EnsureTrailingSlash(&attach_dir_);

  screen_saver_time = ini.value<uint16_t>("SCREEN_SAVER_TIME", screen_saver_time);

  max_extend_lines = std::min<uint16_t>(max_extend_lines, 99);
  max_batch  = std::min<uint16_t>(max_batch , 999);
  max_chains = std::min<uint16_t>(max_chains, 999);
  max_gfilesec = std::min<uint16_t>(max_gfilesec, 999);

  full_screen_read_prompt_ = ini.value<bool>("FULL_SCREEN_READER", true);
  bout.set_logon_key_timeout(seconds(std::max<int>(10, ini.value<int>("LOGON_KEY_TIMEOUT", 30))));
  bout.set_default_key_timeout(seconds(std::max<int>(30, ini.value<int>("USER_KEY_TIMEOUT", 180))));
  bout.set_sysop_key_timeout(seconds(std::max<int>(30, ini.value<int>("SYSOP_KEY_TIMEOUT", 600))));
}

bool Application::ReadInstanceSettings(int instance_number, IniFile& ini) {
  string temp_directory = ini.value<string>("TEMP_DIRECTORY");
  if (temp_directory.empty()) {
    LOG(ERROR) << "TEMP_DIRECTORY must be set in WWIV.INI.";
    return false;
  }

  File::FixPathSeparators(&temp_directory);
  // TEMP_DIRECTORY is defined in wwiv.ini, also default the batch_directory to 
  // TEMP_DIRECTORY if BATCH_DIRECTORY does not exist.
  string batch_directory(ini.value<string>("BATCH_DIRECTORY", temp_directory));
  File::FixPathSeparators(&batch_directory);

  // Replace %n with instance number value.
  string instance_num_string = std::to_string(instance_number);
  StringReplace(&temp_directory, "%n", instance_num_string);
  StringReplace(&batch_directory, "%n", instance_num_string);

  const string base_dir = GetHomeDir();
  File::absolute(base_dir, &batch_directory);
  File::absolute(base_dir, &temp_directory);
  File::EnsureTrailingSlash(&temp_directory);
  File::EnsureTrailingSlash(&batch_directory);

  temp_directory_ = temp_directory;
  batch_directory_ = batch_directory;

  int max_num_instances = ini.value<int>("NUM_INSTANCES", 4);
  if (instance_number > max_num_instances) {
    LOG(ERROR) << "Not enough instances configured (" << max_num_instances << ").";
    return false;
  }
  return true;
}

bool Application::ReadConfig() {
  config_.reset(new Config(a()->GetHomeDir()));
  if (!config_->IsInitialized()) {
    LOG(ERROR) << CONFIG_DAT << " NOT FOUND.";
    return false;
  }

  if (!config_->versioned_config_dat()) {
    std::cerr << "Please run INIT to upgrade " << CONFIG_DAT << " to the most recent version.\r\n";
    LOG(ERROR) << "Please run INIT to upgrade " << CONFIG_DAT << " to the most recent version.";
    wwiv::os::sleep_for(seconds(2));
    return false;
  }

  // initialize the user manager
  user_manager_.reset(new UserManager(*config_));
  statusMgr.reset(new StatusMgr(config_->datadir(), StatusManagerCallback));
  
  IniFile ini(FilePath(GetHomeDir(), WWIV_INI), {StrCat("WWIV-", instance_number()), INI_TAG});
  if (!ini.IsOpen()) {
    LOG(ERROR) << "Unable to read WWIV.INI.";
    AbortBBS();
  }
  ReadINIFile(ini);
  bool config_ovr_read = ReadInstanceSettings(instance_number(), ini);
  if (!config_ovr_read) {
    return false;
  }

  temp_directory_ = File::absolute(GetHomeDir(), temp_directory());
  batch_directory_ = File::absolute(GetHomeDir(), batch_directory());

  return true;
}


void Application::read_nextern() {
  externs.clear();
  DataFile<newexternalrec> externalFile(config()->datadir(), NEXTERN_DAT);
  if (externalFile) {
    externalFile.ReadVector(externs, 15);
  }
}

void Application::read_arcs() {
  arcs.clear();
  DataFile<arcrec> file(config()->datadir(), ARCHIVER_DAT);
  if (file) {
    file.ReadVector(arcs, MAX_ARCS);
  }
}

void Application::read_editors() {
  editors.clear();
  DataFile<editorrec> file(config()->datadir(), EDITORS_DAT);
  if (!file) {
    return;
  }
  file.ReadVector(editors, 10);
}

void Application::read_nintern() {
  over_intern.clear();
  DataFile<newexternalrec> file(config()->datadir(), NINTERN_DAT);
  if (file) {
    file.ReadVector(over_intern, 3);
  }
}

bool Application::read_subs() {
  subs_.reset(new wwiv::sdk::Subs(config_->datadir(), net_networks));
  return subs_->Load();
}

class BBSLastReadImpl : public wwiv::sdk::msgapi::WWIVLastReadImpl {
  uint32_t last_read(int area) const {
    return qsc_p[area];
  }

  void set_last_read(int area, uint32_t last_read) {
    if (area >= 0) {
      qsc_p[area] = last_read;
    }
  }

  void Load() {
    // Handled by the BBS in read_qscn(a()->usernum, qsc, false);
  }

  void Save() {
    // Handled by the BBS in write_qscn(a()->usernum, qsc, false);
  }

};

bool Application::create_message_api() {
  // TODO(rushfan): Create the right API type for the right message area.

  wwiv::sdk::msgapi::MessageApiOptions options;
  // Delete ONE matches classic WWIV behavior.
  options.overflow_strategy = wwiv::sdk::msgapi::OverflowStrategy::delete_one;

  // We only support type-2
  msgapis_[2] = std::make_unique<wwiv::sdk::msgapi::WWIVMessageApi>(
    options, *config_.get(), net_networks, new BBSLastReadImpl());

  return true;
}

void Application::SetLogonTime() {
  steady_logon_time_ = std::chrono::steady_clock::now();
  system_logon_time_ = std::chrono::system_clock::now();
}

std::chrono::system_clock::duration Application::duration_used_this_session() const {
  return std::chrono::system_clock::now() - system_logon_time_; 
}

void Application::read_networks() {
  internetEmailName = "";
  internetEmailDomain = "";
  internetPopDomain = "";
  SetInternetUseRealNames(false);

  // TODO(rushfan): Remove these and put them somewhere else.
  // Like on a per-network config when we add proper internet
  // support.
  IniFile ini("net.ini", {"NETWORK"});
  if (ini.IsOpen()) {
    // Note FWDNAME isn't listed here.
    internetEmailName = ini.value<string>("POPNAME");
    // Note FWDDOM isn't listed here.
    internetEmailDomain = ini.value<string>("DOMAIN");
    internetPopDomain = ini.value<string>("POPDOMAIN");
    SetInternetUseRealNames(ini.value<bool>("REALNAME"));
  }

  wwiv::sdk::Networks networks(*config());
  if (networks.IsInitialized()) {
    net_networks = networks.networks();
  }

  // Add a default entry for us.
  if (net_networks.empty()) {
    net_networks_rec n{};
    strcpy(n.name, "WWIVnet");
    n.dir = config()->datadir();
    n.sysnum = a()->config()->config()->systemnumber;
  }
}

bool Application::read_names() {
  // Load the SDK Names class too.
  names_.reset(new Names(*config_.get()));
  return true;
}

bool Application::read_dirs() {
  directories.clear();
  DataFile<directoryrec> file(config()->datadir(), DIRS_DAT);
  if (!file) {
    LOG(ERROR) << file.file().GetName() << " NOT FOUND.";
    return false;
  }
  file.ReadVector(directories, a()->config()->config()->max_dirs);
  return true;
}

void Application::read_chains() {
  chains.clear();
  DataFile<chainfilerec> file(config()->datadir(), CHAINS_DAT);
  if (!file) {
    return;
  }
  file.ReadVector(chains, max_chains);

  if (HasConfigFlag(OP_FLAGS_CHAIN_REG)) {
    chains_reg.clear();

    DataFile<chainregrec> regFile(config()->datadir(), CHAINS_REG);
    if (regFile) {
      regFile.ReadVector(chains_reg, max_chains);
    } else {
      regFile.Close();
      for (size_t nTempChainNum = 0; nTempChainNum < chains.size(); nTempChainNum++) {
        chainregrec reg;
        memset(&reg, 0, sizeof(chainregrec));
        reg.maxage = 255;
        chains_reg.push_back(reg);
      }
      DataFile<chainregrec> writeFile(config()->datadir(), CHAINS_REG, 
          File::modeReadWrite | File::modeBinary | File::modeCreateFile);
      writeFile.WriteVector(chains_reg);
    }
  }
}

bool Application::read_language() {
  {
    DataFile<languagerec> file(config()->datadir(), LANGUAGE_DAT);
    if (file) {
      file.ReadVector(languages);
    }
  }
  if (languages.empty()) {
    // Add a default language to the list.
    languagerec lang{};
    to_char_array(lang.name, "English");
    to_char_array(lang.dir, a()->config()->gfilesdir());
    to_char_array(lang.mdir, a()->config()->menudir());
    
    languages.emplace_back(lang);
  }

  set_language_number(-1);
  if (!set_language(0)) {
    LOG(ERROR) << "You need the default language installed to run the BBS.";
    return false;
  }
  return true;
}

void Application::read_gfile() {
  DataFile<gfiledirrec> file(config()->datadir(), GFILE_DAT);
  if (file) {
    file.ReadVector(gfilesec, max_gfilesec);
  }
}

void Application::InitializeBBS() {
  localIO()->Cls();
#if !defined( __unix__ )
  std::clog << std::endl << wwiv_version << beta_version << ", Copyright (c) 1998-2017, WWIV Software Services."
            << std::endl << std::endl
            << "\r\nInitializing BBS..." << std::endl;
#endif // __unix__
  SetCurrentReadMessageArea(-1);
  use_workspace = false;
  chat_file = false;
  clearnsp();
  a()->bquote_ = 0;
  a()->equote_ = 0;

  VLOG(1) << "Processing configuration file: WWIV.INI.";
  if (!File::Exists(temp_directory())) {
    if (!File::mkdirs(temp_directory())) {
      LOG(ERROR) << "Your temp dir isn't valid.";
      LOG(ERROR) << "It is now set to: '" << temp_directory() << "'";
      AbortBBS();
    }
  }

  if (!File::Exists(batch_directory())) {
    if (!File::mkdirs(batch_directory())) {
      LOG(ERROR) << "Your batch dir isn't valid.";
      LOG(ERROR) << "It is now set to: '" << batch_directory() << "'";
      AbortBBS();
    }
  }

  write_inst(INST_LOC_INIT, 0, INST_FLAGS_NONE);

  // make sure it is the new USERREC structure
  VLOG(1) << "Reading user scan pointers.";
  File fileQScan(config()->datadir(), USER_QSC);
  if (!fileQScan.Exists()) {
    LOG(ERROR) << "Could not open file '" << fileQScan.full_pathname() << "'";
    LOG(ERROR) << "You must go into INIT and convert your userlist before running the BBS.";
    AbortBBS();
  }

  if (!read_language()) {
    AbortBBS();
  }

  set_net_num(0);
  read_networks();
  set_net_num(0);
  if (!create_message_api()) {
    AbortBBS();
  }

  VLOG(1) << "Reading status information.";
  WStatus* pStatus = statusMgr->BeginTransaction();
  if (!pStatus) {
    LOG(ERROR) << "Unable to return statusrec.dat.";
    AbortBBS();
  }

  pStatus->SetWWIVVersion(wwiv_num_version);
  pStatus->EnsureCallerNumberIsValid();
  statusMgr->CommitTransaction(pStatus);

  VLOG(1) << "Reading Gfiles.";
  read_gfile();

  VLOG(1) << "Reading user names.";
  if (!read_names()) {
    AbortBBS();
  }

  VLOG(1) << "Reading Message Areas.";
  if (!read_subs()) {
    AbortBBS();
  }

  VLOG(1) << "Reading File Areas.";
  if (!read_dirs()) {
    AbortBBS();
  }

  VLOG(1) << "Reading Chains.";
  read_chains();

  VLOG(1) << "Reading File Transfer Protocols.";
  read_nextern();
  read_nintern();

  VLOG(1) << "Reading File Archivers.";
  read_arcs();

  VLOG(1) << "Reading Full Screen Message Editors.";
  read_editors();

  if (!File::mkdirs(attach_dir_)) {
    LOG(ERROR) << "Your file attachment directory is invalid.";
    LOG(ERROR) << "It is now set to: " << attach_dir_ << "'";
    AbortBBS();
  }
  CdHome();

  check_phonenum(); // dupphone addition
  batch().clear();

  VLOG(1) << "Reading User Information.";
  ReadCurrentUser(1);
  statusMgr->RefreshStatusCache();
  topdata = LocalIO::topdataUser;

  // Set DSZLOG
  dsz_logfile_name_ = StrCat(a()->temp_directory(), "dsz.log");
  if (environment_variable("DSZLOG").empty()) {
    set_environment_variable("DSZLOG", dsz_logfile_name_);
  }
  // SET BBS environment variable.
  set_environment_variable("BBS", wwiv_version);

  VLOG(1) << "Reading External Events.";
  init_events();

  VLOG(1) << "Allocating Memory for Message/File Areas.";
  a()->do_event_ = 0;
  usub.resize(config()->config()->max_subs);
  udir.resize(config()->config()->max_dirs);
  uconfsub.resize(MAX_CONFERENCES);
  uconfdir.resize(MAX_CONFERENCES);
  qsc = new uint32_t[(config()->config()->qscn_len / sizeof(uint32_t))];
  qsc_n = qsc + 1;
  qsc_q = qsc_n + (config()->config()->max_dirs + 31) / 32;
  qsc_p = qsc_q + (config()->config()->max_subs + 31) / 32;

  network_extension_ = ".net";
  const string wwiv_instance(environment_variable("WWIV_INSTANCE"));
  if (!wwiv_instance.empty()) {
    int inst_num = to_number<int>(wwiv_instance);
    if (inst_num > 0) {
      network_extension_ = StringPrintf(".%3.3d", inst_num);
      // Fix... Set the global instance variable to match this.  When you run WWIV with the -n<instance> parameter
      // it sets the WWIV_INSTANCE environment variable, however it wasn't doing the reverse.
      instance_number_ = inst_num;
    }
  }

  frequent_init();
  if (!user_already_on_) {
    TempDisablePause disable_pause;
    remove_from_temp("*.*", temp_directory(), true);
    remove_from_temp("*.*", batch_directory(), true);
    cleanup_net();
  }
  subconfnum = dirconfnum = 0;

  VLOG(1) << "Reading Conferences.";
  read_all_conferences();

  if (!user_already_on_) {
    sysoplog(false);
    sysoplog(false) << "WWIV " << wwiv_version << beta_version << ", inst " << instance_number()
      << ", brought up at " << times() << " on " << fulldate() << ".";
  }
  if (instance_number() > 1) {
    File::Remove(StringPrintf("%s.%3.3u", WWIV_NET_NOEXT, instance_number()));
  } else {
    File::Remove(WWIV_NET_DAT);
  }

  srand(static_cast<unsigned int>(time(nullptr)));
  catsl();

  VLOG(1) << "Saving Instance information.";
  write_inst(INST_LOC_WFC, 0, INST_FLAGS_NONE);
}


// begin dupphone additions

void Application::check_phonenum() {
  File phoneFile(config()->datadir(), PHONENUM_DAT);
  if (!phoneFile.Exists()) {
    create_phone_file();
  }
}

// TODO(rushfan): maybe move this to SDK, but pass in a vector of numbers.
void Application::create_phone_file() {
  phonerec p;

  File file(config()->datadir(), USER_LST);
  if (!file.Open(File::modeReadOnly | File::modeBinary)) {
    return;
  }
  auto lFileSize = file.length();
  file.Close();
  int numOfRecords = static_cast<int>(lFileSize / sizeof(userrec));

  File phoneNumFile(config()->datadir(), PHONENUM_DAT);
  if (!phoneNumFile.Open(File::modeReadWrite | File::modeAppend | File::modeBinary | File::modeCreateFile)) {
    return;
  }

  for (int16_t nTempUserNumber = 1; nTempUserNumber <= numOfRecords; nTempUserNumber++) {
    User user;
    users()->readuser(&user, nTempUserNumber);
    if (!user.IsUserDeleted()) {
      p.usernum = nTempUserNumber;
      char szTempVoiceNumber[255], szTempDataNumber[255];
      strcpy(szTempVoiceNumber, user.GetVoicePhoneNumber());
      strcpy(szTempDataNumber, user.GetDataPhoneNumber());
      if (szTempVoiceNumber[0] && !strstr(szTempVoiceNumber, "000-")) {
        strcpy(reinterpret_cast<char*>(p.phone), szTempVoiceNumber);
        phoneNumFile.Write(&p, sizeof(phonerec));
      }
      if (szTempDataNumber[0] &&
          !IsEquals(szTempVoiceNumber, szTempDataNumber) &&
          !strstr(szTempVoiceNumber, "000-")) {
        strcpy(reinterpret_cast<char*>(p.phone), szTempDataNumber);
        phoneNumFile.Write(&p, sizeof(phonerec));
      }
    }
  }
  phoneNumFile.Close();
}

uint32_t GetFlagsFromIniFile(IniFile& ini, ini_flags_type * fs, int nFlagNumber, uint32_t flags) {
  for (int i = 0; i < nFlagNumber; i++) {
    const char* key = INI_OPTIONS_ARRAY[ fs[i].strnum ];
    if (!key) {
      continue;
    }
    string val = ini.value<string>(key);
    if (val.empty()) {
      continue;
    }
    if (ini.value<bool>(key)) {
      if (fs[i].sense) {
        flags &= ~fs[i].value;
      } else {
        flags |= fs[i].value;
      }
    } else {
      if (fs[i].sense) {
        flags |= fs[i].value;
      } else {
        flags &= ~fs[i].value;
      }
    }
  }

  return flags;
}

// end dupphone additions
