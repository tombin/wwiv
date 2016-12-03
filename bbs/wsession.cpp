/**************************************************************************/
/*                                                                        */
/*                              WWIV Version 5.x                          */
/*             Copyright (C)1998-2016, WWIV Software Services             */
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
#ifdef _WIN32
// include this here so it won't get includes by local_io_win32.h
#include "bbs/wwiv_windows.h"
#include <io.h>
#endif  // WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <cstdarg>
#include "bbs/asv.h"
#include "bbs/bbsovl1.h"
#include "bbs/bbsovl2.h"
#include "bbs/bbsutl2.h"
#include "bbs/chnedit.h"
#include "bbs/bgetch.h"
#include "bbs/bbs.h"
#include "bbs/bbsutl1.h"
#include "bbs/com.h"
#include "bbs/confutil.h"
#include "bbs/connect1.h"
#include "bbs/datetime.h"
#include "bbs/diredit.h"
#include "bbs/events.h"
#include "bbs/exceptions.h"
#include "bbs/external_edit.h"
#include "bbs/fcns.h"
#include "bbs/gfileedit.h"
#include "bbs/gfiles.h"
#include "bbs/input.h"
#include "bbs/inetmsg.h"
#include "bbs/instmsg.h"
#include "bbs/lilo.h"
#include "bbs/local_io.h"
#include "bbs/local_io_curses.h"
#include "bbs/null_local_io.h"
#include "bbs/null_remote_io.h"
#include "bbs/netsup.h"
#include "bbs/menu.h"
#include "bbs/pause.h"
#include "bbs/printfile.h"
#include "bbs/ssh.h"
#include "bbs/sysoplog.h"
#include "bbs/uedit.h"
#include "bbs/utility.h"
#include "bbs/voteedit.h"
#include "bbs/remote_io.h"
#include "bbs/subedit.h"
#include "bbs/wconstants.h"
#include "bbs/wfc.h"
#include "bbs/wsession.h"
#include "bbs/workspace.h"
#include "bbs/platform/platformfcns.h"
#include "core/strings.h"
#include "core/os.h"
#include "core/wwivassert.h"
#include "core/wwivport.h"
#include "sdk/filenames.h"
#include "sdk/status.h"

#if defined( _WIN32 )
#include "bbs/remote_socket_io.h"
#include "bbs/local_io_win32.h"
#else
#include <unistd.h>
#endif // _WIN32

using std::chrono::milliseconds;
using std::chrono::seconds;
using std::clog;
using std::cout;
using std::endl;
using std::exception;
using std::stoi;
using std::stol;
using std::string;
using std::min;
using std::max;
using std::string;
using std::unique_ptr;
using wwiv::bbs::InputMode;
using namespace wwiv::os;
using namespace wwiv::sdk;
using namespace wwiv::strings;

extern time_t last_time_c;
Output bout;

WSession::WSession(WApplication* app, LocalIO* localIO)
    : application_(app), local_io_(localIO), oklevel_(exitLevelOK), errorlevel_(exitLevelNotOK), batch_() {
  ::bout.SetLocalIO(localIO);

  memset(&newuser_colors, 0, sizeof(newuser_colors));
  memset(&newuser_bwcolors, 0, sizeof(newuser_bwcolors));
  memset(&asv, 0, sizeof(asv_rec));

  // Set the home directory
  current_dir_ = File::current_directory();
}

WSession::~WSession() {
  if (comm_ && ok_modem_stuff) {
    comm_->close(false);
    comm_.reset(nullptr);
  }
  if (local_io_) {
    local_io_->SetCursor(LocalIO::cursorNormal);
  }
  // CursesIO.
  if (out != nullptr) {
    delete out;
    out = nullptr;
  }
}

bool WSession::reset_local_io(LocalIO* wlocal_io) {
  local_io_.reset(wlocal_io);

  const int screen_bottom = localIO()->GetDefaultScreenBottom();
  localIO()->SetScreenBottom(screen_bottom);
  defscreenbottom = screen_bottom;
  screenlinest = screen_bottom + 1;

  ClearTopScreenProtection();
  ::bout.SetLocalIO(wlocal_io);
  return true;
}

void WSession::CreateComm(unsigned int nHandle, CommunicationType type) {
  switch (type) {
  case CommunicationType::SSH: {
    const File key_file(config_->datadir(), "wwiv.key");
    const string system_password = config()->config()->systempw;
    wwiv::bbs::Key key(key_file.full_pathname(), system_password);
    if (!key_file.Exists()) {
      LOG(ERROR)<< "Key file doesn't exist. Will try to create it.";
      if (!key.Create()) {
        LOG(ERROR) << "Unable to create or open key file!.  SSH will be disabled!" << endl;
        type = CommunicationType::TELNET;
      }
    }
    if (!key.Open()) {
      LOG(ERROR)<< "Unable to open key file!. Did you change your sytem pw?" << endl;
      LOG(ERROR) << "If so, delete " << key_file.full_pathname();
      LOG(ERROR) << "SSH will be disabled!";
      type = CommunicationType::TELNET;
    }
    comm_.reset(new wwiv::bbs::IOSSH(nHandle, key));
  } break;
  case CommunicationType::TELNET: {
    comm_.reset(new RemoteSocketIO(nHandle, true));
  } break;
  case CommunicationType::NONE: {
    comm_.reset(new NullRemoteIO());
  } break;
  }
  bout.SetComm(comm_.get());
}

bool WSession::ReadCurrentUser(int user_number) {
  bool result = users()->ReadUser(&thisuser_, user_number);

  // Update all other session variables that are dependent.
  screenlinest = (using_modem) ? user()->GetScreenLines() : defscreenbottom + 1;
  return result;
}

bool WSession::WriteCurrentUser(int user_number) {
  return users()->WriteUser(&thisuser_, user_number);
}

void WSession::tleft(bool check_for_timeout) {
  long nsln = nsl();
  bool temp_sysop = session()->user()->GetSl() != 255 && session()->GetEffectiveSl() == 255;
  bool sysop_available = sysop1();

  int cx = localIO()->WhereX();
  int cy = localIO()->WhereY();
  int ctl = localIO()->GetTopLine();
  int cc = curatr;
  curatr = localIO()->GetTopScreenColor();
  localIO()->SetTopLine(0);
  int line_number = (chatcall && (topdata == LocalIO::topdataUser)) ? 5 : 4;

  if (topdata) {
    localIO()->PutsXY(1, line_number, GetCurrentSpeed());
    for (int i = localIO()->WhereX(); i < 23; i++) {
      localIO()->Putch(static_cast<unsigned char>('\xCD'));
    }

    if (temp_sysop) {
      localIO()->PutsXY(23, line_number, "Temp Sysop");
    }

    if (sysop_available) {
      localIO()->PutsXY(64, line_number, "Available");
    }
  }

  auto min_left = nsln / SECONDS_PER_MINUTE;
  auto secs_left = nsln % SECONDS_PER_MINUTE;
  string tleft_display = wwiv::strings::StringPrintf("T-%4ldm %2lds", min_left, secs_left);
  switch (topdata) {
  case LocalIO::topdataSystem:
    if (IsUserOnline()) {
      localIO()->PutsXY(18, 3, tleft_display);
    }
    break;
  case LocalIO::topdataUser: {
    if (IsUserOnline()) {
      localIO()->PutsXY(18, 3, tleft_display);
    } else {
      localIO()->PrintfXY(18, 3, user()->GetPassword());
    }
  }
    break;
  }
  localIO()->SetTopLine(ctl);
  curatr = cc;
  localIO()->GotoXY(cx, cy);

  if (check_for_timeout && IsUserOnline()) {
    if (nsln == 0.0) {
      bout << "\r\nTime expired.\r\n\n";
      Hangup();
    }
  }
}

void WSession::handle_sysop_key(uint8_t key) {
  int i, i1;

  if (okskey) {
    if (key >= AF1 && key <= AF10) {
      set_autoval(key - 104);
    } else {
      switch (key) {
      case F1: /* F1 */
        OnlineUserEditor();
        break;
      case SF1:
        /* Shift-F1 */
        // Nothing.
        UpdateTopScreen();
        break;
      case CF1: /* Ctrl-F1 */
        // Used to be shutdown bbs in 3 minutes.
        break;
      case F2: /* F2 */
        topdata++;
        if (topdata > LocalIO::topdataUser) {
          topdata = LocalIO::topdataNone;
        }
        UpdateTopScreen();
        break;
      case F3: /* F3 */
        if (using_modem) {
          incom = !incom;
          bout.dump();
          tleft(false);
        }
        break;
      case F4: /* F4 */
        chatcall = false;
        UpdateTopScreen();
        break;
      case F5: /* F5 */
        remoteIO()->disconnect();
        Hangup();
        break;
      case SF5: /* Shift-F5 */
        i1 = (rand() % 20) + 10;
        for (i = 0; i < i1; i++) {
          bout.bputch(static_cast<unsigned char>(rand() % 256));
        }
        remoteIO()->disconnect();
        Hangup();
        break;
      case CF5: /* Ctrl-F5 */
        bout << "\r\nCall back later when you are there.\r\n\n";
        remoteIO()->disconnect();
        Hangup();
        break;
      case F6: /* F6 - was Toggle Sysop Alert*/
        tleft(false);
        break;
      case F7: /* F7 */
        user()->SetExtraTime(user()->GetExtraTime() - static_cast<float>(5 * SECONDS_PER_MINUTE));
        tleft(false);
        break;
      case F8: /* F8 */
        user()->SetExtraTime(user()->GetExtraTime() + static_cast<float>(5 * SECONDS_PER_MINUTE));
        tleft(false);
        break;
      case F9: /* F9 */
        if (user()->GetSl() != 255) {
          if (GetEffectiveSl() != 255) {
            SetEffectiveSl(255);
          } else {
            ResetEffectiveSl();
          }
          changedsl();
          tleft(false);
        }
        break;
      case F10: /* F10 */
        if (chatting == 0) {
          if (syscfg.sysconfig & sysconfig_2_way) {
            chat1("", true);
          } else {
            chat1("", false);
          }
        } else {
          chatting = 0;
        }
        break;
      case CF10: /* Ctrl-F10 */
        if (chatting == 0) {
          chat1("", false);
        } else {
          chatting = 0;
        }
        break;
      case HOME: /* HOME */
        if (chatting == 1) {
          chat_file = !chat_file;
        }
        break;
      }
    }
  }
}

void WSession::DisplaySysopWorkingIndicator(bool displayWait) {
  const string waitString = "-=[WAIT]=-";
  auto nNumPrintableChars = waitString.length();
  for (std::string::const_iterator iter = waitString.begin(); iter != waitString.end(); ++iter) {
    if (*iter == 3 && nNumPrintableChars > 1) {
      nNumPrintableChars -= 2;
    }
  }

  if (displayWait) {
    if (okansi()) {
      int nSavedAttribute = curatr;
      bout.SystemColor(user()->HasColor() ? user()->GetColor(3) : user()->GetBWColor(3));
      bout << waitString << "\x1b[" << nNumPrintableChars << "D";
      bout.SystemColor(static_cast<unsigned char>(nSavedAttribute));
    } else {
      bout << waitString;
    }
  } else {
    if (okansi()) {
      for (unsigned int j = 0; j < nNumPrintableChars; j++) {
        bout.bputch(' ');
      }
      bout << "\x1b[" << nNumPrintableChars << "D";
    } else {
      for (unsigned int j = 0; j < nNumPrintableChars; j++) {
        bout.bs();
      }
    }
  }
}

void WSession::UpdateTopScreen() {
  if (GetWfcStatus()) {
    return;
  }

  unique_ptr<WStatus> pStatus(status_manager()->GetStatus());
  char i;
  char sl[82], ar[17], dar[17], restrict[17], rst[17], lo[90];

  int lll = bout.lines_listed();

  if (so() && !incom) {
    topdata = LocalIO::topdataNone;
  }

#ifdef _WIN32
  if (syscfg.sysconfig & sysconfig_titlebar) {
    // Only set the titlebar if the user wanted it that way.
    const string username_num = names()->UserName(usernum);
    string title = StringPrintf("WWIV Node %d (User: %s)", instance_number(),
        username_num.c_str());
    ::SetConsoleTitle(title.c_str());
  }
#endif // _WIN32

  switch (topdata) {
  case LocalIO::topdataNone:
    localIO()->set_protect(this, 0);
    break;
  case LocalIO::topdataSystem:
    localIO()->set_protect(this, 5);
    break;
  case LocalIO::topdataUser:
    if (chatcall) {
      localIO()->set_protect(this, 6);
    } else {
      if (localIO()->GetTopLine() == 6) {
        localIO()->set_protect(this, 0);
      }
      localIO()->set_protect(this, 5);
    }
    break;
  }
  int cx = localIO()->WhereX();
  int cy = localIO()->WhereY();
  int nOldTopLine = localIO()->GetTopLine();
  int cc = curatr;
  curatr = localIO()->GetTopScreenColor();
  localIO()->SetTopLine(0);
  for (i = 0; i < 80; i++) {
    sl[i] = '\xCD';
  }
  sl[80] = '\0';

  switch (topdata) {
  case LocalIO::topdataNone:
    break;
  case LocalIO::topdataSystem: {
    localIO()->PrintfXY(0, 0, "%-50s  Activity for %8s:      ", syscfg.systemname, pStatus->GetLastDate());

    localIO()->PrintfXY(0, 1, "Users: %4u       Total Calls: %5lu      Calls Today: %4u    Posted      :%3u ",
        pStatus->GetNumUsers(), pStatus->GetCallerNumber(), pStatus->GetNumCallsToday(), pStatus->GetNumLocalPosts());

    const string username_num = names()->UserName(usernum);
    localIO()->PrintfXY(0, 2, "%-36s      %-4u min   /  %2u%%    E-mail sent :%3u ", username_num.c_str(),
        pStatus->GetMinutesActiveToday(), static_cast<int>(10 * pStatus->GetMinutesActiveToday() / 144),
        pStatus->GetNumEmailSentToday());

    User sysop { };
    int feedback_waiting = 0;
    if (session()->users()->ReadUserNoCache(&sysop, 1)) {
      feedback_waiting = sysop.GetNumMailWaiting();
    }
    localIO()->PrintfXY(0, 3, "SL=%3u   DL=%3u               FW=%3u      Uploaded:%2u files    Feedback    :%3u ",
        user()->GetSl(), user()->GetDsl(), feedback_waiting, pStatus->GetNumUploadsToday(),
        pStatus->GetNumFeedbackSentToday());
  }
    break;
  case LocalIO::topdataUser: {
    strcpy(rst, restrict_string);
    for (i = 0; i <= 15; i++) {
      if (user()->HasArFlag(1 << i)) {
        ar[i] = static_cast<char>('A' + i);
      } else {
        ar[i] = SPACE;
      }
      if (user()->HasDarFlag(1 << i)) {
        dar[i] = static_cast<char>('A' + i);
      } else {
        dar[i] = SPACE;
      }
      if (user()->HasRestrictionFlag(1 << i)) {
        restrict[i] = rst[i];
      } else {
        restrict[i] = SPACE;
      }
    }
    dar[16] = '\0';
    ar[16] = '\0';
    restrict[16] = '\0';
    if (!wwiv::strings::IsEquals(user()->GetLastOn(), date())) {
      strcpy(lo, user()->GetLastOn());
    } else {
      snprintf(lo, sizeof(lo), "Today:%2d", user()->GetTimesOnToday());
    }

    const string username_num = names()->UserName(usernum);
    localIO()->PrintfXYA(0, 0, curatr, "%-35s W=%3u UL=%4u/%6lu SL=%3u LO=%5u PO=%4u", username_num.c_str(),
        user()->GetNumMailWaiting(), user()->GetFilesUploaded(), user()->GetUploadK(), user()->GetSl(),
        user()->GetNumLogons(), user()->GetNumMessagesPosted());

    char szCallSignOrRegNum[41];
    if (user()->GetWWIVRegNumber()) {
      snprintf(szCallSignOrRegNum, sizeof(szCallSignOrRegNum), "%lu", user()->GetWWIVRegNumber());
    } else {
      strcpy(szCallSignOrRegNum, user()->GetCallsign());
    }
    localIO()->PrintfXY(0, 1, "%-20s %12s  %-6s DL=%4u/%6lu DL=%3u TO=%5.0lu ES=%4u", user()->GetRealName(),
        user()->GetVoicePhoneNumber(), szCallSignOrRegNum, user()->GetFilesDownloaded(), user()->GetDownloadK(),
        user()->GetDsl(), static_cast<long>((user()->GetTimeOn() + timer() - timeon) / SECONDS_PER_MINUTE),
        user()->GetNumEmailSent() + user()->GetNumNetEmailSent());

    localIO()->PrintfXY(0, 2, "ARs=%-16s/%-16s R=%-16s EX=%3u %-8s FS=%4u", ar, dar, restrict, user()->GetExempt(), lo,
        user()->GetNumFeedbackSent());

    User sysop { };
    int feedback_waiting = 0;
    if (session()->users()->ReadUserNoCache(&sysop, 1)) {
      feedback_waiting = sysop.GetNumMailWaiting();
    }
    localIO()->PrintfXY(0, 3, "%-40.40s %c %2u %-16.16s           FW= %3u", user()->GetNote(), user()->GetGender(),
        user()->GetAge(), ctypes(user()->GetComputerType()).c_str(), feedback_waiting);

    if (chatcall) {
      localIO()->PutsXY(0, 4, chat_reason_);
    }
  }
    break;
  }
  if (nOldTopLine != 0) {
    localIO()->PutsXY(0, nOldTopLine - 1, sl);
  }
  localIO()->SetTopLine(nOldTopLine);
  localIO()->GotoXY(cx, cy);
  curatr = cc;
  tleft(false);

  bout.lines_listed_ = lll;
}

void WSession::ClearTopScreenProtection() {
  localIO()->set_protect(this, 0);
}

const char* WSession::network_name() const {
  if (net_networks.empty()) {
    return "";
  }
  return net_networks[network_num_].name;
}

const std::string WSession::network_directory() const {
  if (net_networks.empty()) {
    return "";
  }
  return std::string(net_networks[network_num_].dir);
}

void WSession::GetCaller() {
  wfc_init();
  remoteIO()->remote_info().clear();
  frequent_init();
  if (wfc_status == 0) {
    localIO()->Cls();
  }
  usernum = 0;
  // Since hang_it_up sets hangup = true, let's ensure we're always
  // not in this state when we enter the WFC.
  hangup = false;
  SetWfcStatus(0);
  write_inst(INST_LOC_WFC, 0, INST_FLAGS_NONE);
  ReadCurrentUser(1);
  read_qscn(1, qsc, false);
  usernum = 1;
  ResetEffectiveSl();
  if (user()->IsUserDeleted()) {
    user()->SetScreenChars(80);
    user()->SetScreenLines(25);
  }
  screenlinest = defscreenbottom + 1;

  int lokb = doWFCEvents();

  if (lokb) {
    modem_speed = 38400;
  }

  using_modem = incom;
  if (lokb == 2) {
    using_modem = -1;
  }

  okskey = true;
  localIO()->Cls();
  localIO()->Printf("Logging on at %s ...\r\n", GetCurrentSpeed().c_str());
  SetWfcStatus(0);
}

int WSession::doWFCEvents() {
  unsigned char ch;
  int lokb;
  LocalIO* io = localIO();

  unique_ptr<WStatus> last_date_status(status_manager()->GetStatus());
  do {
    write_inst(INST_LOC_WFC, 0, INST_FLAGS_NONE);
    set_net_num(0);
    bool any = false;
    SetWfcStatus(1);

    // If the date has changed since we last checked, then then run the beginday event.
    if (!IsEquals(date(), last_date_status->GetLastDate())) {
      if ((GetBeginDayNodeNumber() == 0) || (instance_number_ == GetBeginDayNodeNumber())) {
        cleanup_events();
        beginday(true);
        wfc_cls();
      }
    }

    if (!do_event) {
      check_event();
    }

    while (do_event) {
      run_event(do_event - 1);
      check_event();
      any = true;
    }

    lokb = 0;
    SetCurrentSpeed("KB");
    time_t current_time = time(nullptr);
    bool node_supports_callout = HasConfigFlag(OP_FLAGS_NET_CALLOUT);
    // try to check for packets to send every minute.
    time_t diff_time = current_time - last_time_c;
    bool time_to_call = diff_time > 60;  // was 1200
    if (!any && time_to_call && current_net().sysnum && node_supports_callout) {
      // also try this.
      wfc_cls();
      attempt_callout();
      any = true;
    }
    wfc_screen();
    okskey = false;
    if (io->KeyPressed()) {
      SetWfcStatus(0);
      ReadCurrentUser(1);
      read_qscn(1, qsc, false);
      SetWfcStatus(1);
      ch = wwiv::UpperCase<char>(io->GetChar());
      if (ch == 0) {
        ch = io->GetChar();
        handle_sysop_key(ch);
        ch = 0;
      }
    } else {
      ch = 0;
      giveup_timeslice();
    }
    if (ch) {
      SetWfcStatus(2);
      any = true;
      okskey = true;
      resetnsp();
      io->SetCursor(LocalIO::cursorNormal);
      switch (ch) {
      // Local Logon
      case SPACE:
        lokb = this->LocalLogon();
        break;
        // Show WFC Menu
      case '?': {
        string helpFileName = SWFC_NOEXT;
        char chHelp = ESC;
        do {
          io->Cls();
          bout.nl();
          printfile(helpFileName);
          chHelp = bout.getkey();
          helpFileName = (helpFileName == SWFC_NOEXT) ? SONLINE_NOEXT : SWFC_NOEXT;
        } while (chHelp != SPACE && chHelp != ESC);
      }
        break;
        // Force Network Callout
      case '/':
        if (current_net().sysnum) {
          force_callout(0);
        }
        break;
      // War Dial Connect
      case '.':
        if (current_net().sysnum) {
          force_callout(1);
        } break;
      // Fast Net Callout from WFC
      case '*': {
        io->Cls();
        do_callout(32767);
      } break;
      // Run MenuEditor
      case '!':
        EditMenus();
        break;
        // Print NetLogs
      case ',':
        if (current_net().sysnum > 0 || !net_networks.empty()) {
          io->GotoXY(2, 23);
          bout << "|#7(|#2Q|#7=|#2Quit|#7) Display Which NETDAT Log File (|#10|#7-|#12|#7): ";
          ch = onek("Q012");
          switch (ch) {
          case '0':
          case '1':
          case '2': {
            print_local_file(StringPrintf("netdat%c.log", ch));
          }
            break;
          }
        }
        break;
        // Net List
      case '`':
        if (current_net().sysnum) {
          print_net_listing(true);
        }
        break;
        // [ESC] Quit the BBS
      case ESC:
        io->GotoXY(2, 23);
        bout << "|#7Exit the BBS? ";
        if (yesno()) {
          QuitBBS();
        }
        io->Cls();
        break;
        // BoardEdit
      case 'B':
        write_inst(INST_LOC_BOARDEDIT, 0, INST_FLAGS_NONE);
        boardedit();
        cleanup_net();
        break;
        // ChainEdit
      case 'C':
        write_inst(INST_LOC_CHAINEDIT, 0, INST_FLAGS_NONE);
        chainedit();
        break;
        // DirEdit
      case 'D':
        write_inst(INST_LOC_DIREDIT, 0, INST_FLAGS_NONE);
        dlboardedit();
        break;
        // Send Email
      case 'E':
        wfc_cls();
        usernum = 1;
        bout.bputs("|#1Send Email:");
        send_email();
        WriteCurrentUser(1);
        cleanup_net();
        break;
        // GfileEdit
      case 'G':
        write_inst(INST_LOC_GFILEEDIT, 0, INST_FLAGS_NONE);
        gfileedit();
        break;
        // EventEdit
      case 'H':
        write_inst(INST_LOC_EVENTEDIT, 0, INST_FLAGS_NONE);
        eventedit();
        break;
        // Send Internet Mail
      case 'I': {
        wfc_cls();
        usernum = 1;
        SetUserOnline(true);
        get_user_ppp_addr();
        send_inet_email();
        SetUserOnline(false);
        WriteCurrentUser(1);
        cleanup_net();
      }
        break;
        // ConfEdit
      case 'J':
        wfc_cls();
        edit_confs();
        break;
        // SendMailFile
      case 'K': {
        wfc_cls();
        usernum = 1;
        bout << "|#1Send any Text File in Email:\r\n\n|#2Filename: ";
        string buffer = input(50);
        LoadFileIntoWorkspace(buffer, false);
        send_email();
        WriteCurrentUser(1);
        cleanup_net();
      }
        break;
        // Print Log Daily logs
      case 'L': {
        wfc_cls();
        unique_ptr<WStatus> pStatus(status_manager()->GetStatus());
        print_local_file(pStatus->GetLogFileName(0));
      }
        break;
        // Read User Mail
      case 'M': {
        wfc_cls();
        usernum = 1;
        readmail(0);
        WriteCurrentUser(1);
        cleanup_net();
      }
        break;
        // Print Net Log
      case 'N': {
        wfc_cls();
        print_local_file("net.log");
      }
        break;
        // EditTextFile
      case 'O': {
        wfc_cls();
        write_inst(INST_LOC_TEDIT, 0, INST_FLAGS_NONE);
        bout << "\r\n|#1Edit any Text File: \r\n\n|#2Filename: ";
        const string current_dir_slash = File::current_directory() + File::pathSeparatorString;
        string newFileName = Input1(current_dir_slash, 50, true, InputMode::FULL_PATH_NAME);
        if (!newFileName.empty()) {
          external_text_edit(newFileName, "", 500, ".", MSGED_FLAG_NO_TAGLINE);
        }
      }
        break;
        // Print Network Pending list
      case 'P': {
        wfc_cls();
        print_pending_list();
      }
        break;
        // Quit BBS
      case 'Q':
        io->GotoXY(2, 23);
        QuitBBS();
        break;
        // Read All Mail
      case 'R':
        wfc_cls();
        write_inst(INST_LOC_MAILR, 0, INST_FLAGS_NONE);
        mailr();
        break;
        // Print Current Status
      case 'S':
        prstatus();
        bout.getkey();
        break;
        // UserEdit
      case 'T':
        if (syscfg.terminal_command.empty()) {
          bout << "Terminal Command not specified. " << wwiv::endl << " Please set TERMINAL_CMD in WWIV.INI"
              << wwiv::endl;
          bout.getkey();
          break;
        }
        ExecExternalProgram(syscfg.terminal_command, INST_FLAGS_NONE);
        break;
      case 'U':
        write_inst(INST_LOC_UEDIT, 0, INST_FLAGS_NONE);
        uedit(1, UEDIT_NONE);
        break;
        // InitVotes
      case 'V': {
        wfc_cls();
        write_inst(INST_LOC_VOTEEDIT, 0, INST_FLAGS_NONE);
        ivotes();
      }
        break;
        // Edit Gfile
      case 'W': {
        wfc_cls();
        write_inst(INST_LOC_TEDIT, 0, INST_FLAGS_NONE);
        bout << "|#1Edit " << session()->config()->gfilesdir() << "<filename>: \r\n";
        text_edit();
      }
        break;
        // Print Environment
      case 'X':
        break;
        // Print Yesterday's Log
      case 'Y': {
        wfc_cls();
        unique_ptr<WStatus> pStatus(status_manager()->GetStatus());
        print_local_file(pStatus->GetLogFileName(1));
      }
        break;
        // Print Activity (Z) Log
      case 'Z': {
        zlog();
        bout.nl();
        bout.getkey();
      }
        break;
      }
      wfc_cls();  // moved from after getch
      if (!incom && !lokb) {
        frequent_init();
        ReadCurrentUser(1);
        read_qscn(1, qsc, false);
        ResetEffectiveSl();
        usernum = 1;
      }
      catsl();
      write_inst(INST_LOC_WFC, 0, INST_FLAGS_NONE);
    }

    if (!any) {
      static int mult_time = 0;
      if (this->IsCleanNetNeeded() || std::abs(timer1() - mult_time) > 1000L) {
        // let's try this.
        wfc_cls();
        cleanup_net();
        mult_time = timer1();
      }
      giveup_timeslice();
    }
  } while (!incom && !lokb);
  return lokb;
}

int WSession::LocalLogon() {
  localIO()->GotoXY(2, 23);
  bout << "|#9Log on to the BBS?";
  auto d = timer();
  int lokb = 0;
  while (!localIO()->KeyPressed() && (std::abs(timer() - d) < SECONDS_PER_MINUTE))
    ;

  if (localIO()->KeyPressed()) {
    char ch = wwiv::UpperCase<char>(localIO()->GetChar());
    if (ch == 'Y') {
      localIO()->Puts(YesNoString(true));
      bout << wwiv::endl;
      lokb = 1;
    } else if (ch == 0 || static_cast<unsigned char>(ch) == 224) {
      // The ch == 224 is a Win32'ism
      localIO()->GetChar();
    } else {
      bool fast = false;

      if (ch == 'F') {   // 'F' for Fast
        unx_ = 1;
        fast = true;
      } else {
        switch (ch) {
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          fast = true;
          unx_ = ch - '0';
          break;
        }
      }
      if (!fast || unx_ > status_manager()->GetUserCount()) {
        return lokb;
      }

      User tu;
      users()->ReadUserNoCache(&tu, unx_);
      if (tu.GetSl() != 255 || tu.IsUserDeleted()) {
        return lokb;
      }

      usernum = unx_;
      int nSavedWFCStatus = GetWfcStatus();
      SetWfcStatus(0);
      ReadCurrentUser();
      read_qscn(usernum, qsc, false);
      SetWfcStatus(nSavedWFCStatus);
      bout.bputch(ch);
      localIO()->Puts("\r\n\r\n\r\n\r\n\r\n\r\n");
      lokb = 2;
      ResetEffectiveSl();
      changedsl();
      if (!set_language(user()->GetLanguage())) {
        user()->SetLanguage(0);
        set_language(0);
      }
      return lokb;
    }
    if (ch == 0 || static_cast<unsigned char>(ch) == 224) {
      // The 224 is a Win32'ism
      localIO()->GetChar();
    }
  }
  if (lokb == 0) {
    localIO()->Cls();
  }
  return lokb;
}

void WSession::GotCaller(unsigned int ms, unsigned long cs) {
  frequent_init();
  if (wfc_status == 0) {
    localIO()->Cls();
  }
  com_speed = cs;
  modem_speed = ms;
  ReadCurrentUser(1);
  read_qscn(1, qsc, false);
  ResetEffectiveSl();
  usernum = 1;
  if (user()->IsUserDeleted()) {
    user()->SetScreenChars(80);
    user()->SetScreenLines(25);
  }
  localIO()->Cls();
  localIO()->Printf("Logging on at %s...\r\n", GetCurrentSpeed().c_str());
  if (ms) {
    incom = true;
    outcom = true;
    using_modem = 1;
  } else {
    using_modem = 0;
    incom = false;
    outcom = false;
  }
}

void WSession::CdHome() {
  File::set_current_directory(current_dir_);
}

const string WSession::GetHomeDir() {
  string dir = current_dir_;
  File::EnsureTrailingSlash(&dir);
  return dir;
}

void WSession::AbortBBS(bool bSkipShutdown) {
  clog.flush();
  ExitBBSImpl(errorlevel_, !bSkipShutdown);
}

void WSession::QuitBBS() {
  ExitBBSImpl(WSession::exitLevelQuit, true);
}

void WSession::ExitBBSImpl(int exit_level, bool perform_shutdown) {
  if (perform_shutdown) {
    if (exit_level != WSession::exitLevelOK && exit_level != WSession::exitLevelQuit) {
      // Only log the exiting at abnormal error levels, since we see lots of exiting statements
      // in the logs that don't correspond to sessions every being created (network probers, etc).
      sysoplog(false);
      sysoplog(false) << "WWIV " << wwiv_version << ", inst " << instance_number() << ", taken down at " << times()
          << " on " << fulldate() << " with exit code " << exit_level << ".";
      sysoplog(false);
    }
    catsl();
    write_inst(INST_LOC_DOWN, 0, INST_FLAGS_NONE);
    clog << "\r\n";
    clog << "WWIV Bulletin Board System " << wwiv_version << beta_version << " exiting at error level " << exit_level
        << endl << endl;
  }

  // We just delete the session class, not the application class
  // since one day it'd be ideal to have 1 application contain
  // N sessions for N>1.
  delete this;
  exit(exit_level);
}

void WSession::ShowUsage() {
  cout << "WWIV Bulletin Board System [" << wwiv_version << beta_version << "]\r\n\n" << "Usage:\r\n\n"
      << "bbs -N<inst> [options] \r\n\n" << "Options:\r\n\n"
      << "  -?         - Display command line options (This screen)\r\n\n"
      << "  -A<level>  - Specify the Error Exit Level\r\n"
      << "  -B<rate>   - Someone already logged on at rate (modem speed)\r\n"
      << "  -E         - Load for beginday event only\r\n"
      << "  -H<handle> - Socket handle\r\n"
      << "  -K [# # #] - Pack Message Areas, optionally list the area(s) to pack\r\n"
      << "  -M         - Don't access modem at all\r\n"
      << "  -N<inst>   - Designate instance number <inst>\r\n"
      << "  -Q<level>  - Normal exit level\r\n"
      << "  -R<min>    - Specify max # minutes until event\r\n"
      << "  -S<rate>   - Used only with -B, indicates com port speed\r\n"
      << "  -U<user#>  - Pass usernumber <user#> online\r\n"
      << "  -V         - Display WWIV Version\r\n"
      << "  -XT        - Someone already logged on via telnet (socket handle)\r\n"
#if defined (_WIN32)
      << "  -XS        - Someone already logged on via SSH (socket handle)\r\n"
#endif // _WIN32
      << "  -Z         - Do not hang up on user when at log off\r\n" << endl;
}

int WSession::Run(int argc, char *argv[]) {
  int num_min = 0;
  unsigned int ui = 0;
  unsigned long us = 0;
  unsigned short this_usernum = 0;
  bool ooneuser = false;
  bool event_only = false;
  CommunicationType type = CommunicationType::NONE;
  unsigned int hSockOrComm = 0;

  curatr = 0x07;
  // Set the instance, this may be changed by a command line argument
  instance_number_ = 1;
  no_hangup = false;
  ok_modem_stuff = true;

  const std::string bbs_env = environment_variable("BBS");
  if (!bbs_env.empty()) {
    if (bbs_env.find("WWIV") != string::npos) {
      LOG(ERROR)<< "You are already in the BBS, type 'EXIT' instead.\n\n";
      session()->ExitBBSImpl(255, false);
    }
  }
  const string wwiv_dir = environment_variable("WWIV_DIR");
  if (!wwiv_dir.empty()) {
    File::set_current_directory(wwiv_dir);
  }

  for (int i = 1; i < argc; i++) {
    string argumentRaw = argv[i];
    if (argumentRaw.length() > 1 && (argumentRaw[0] == '-' || argumentRaw[0] == '/')) {
      string argument = argumentRaw.substr(2);
      char ch = wwiv::UpperCase<char>(argumentRaw[1]);
      switch (ch) {
      case 'B': {
        // I think this roundtrip here is just to ensure argument is really a number.
        ui = static_cast<unsigned int>(atol(argument.c_str()));
        const string current_speed_string = std::to_string(ui);
        SetCurrentSpeed(current_speed_string.c_str());
        if (!us) {
          us = ui;
        }
        user_already_on_ = true;
      }
        break;
      case 'C':
        break;
      case 'E':
        event_only = true;
        break;
      case 'S':
        us = static_cast<unsigned int>(stol(argument));
        if ((us % 300) && us != 115200) {
          us = ui;
        }
        break;
      case 'Q':
        oklevel_ = stoi(argument);
        break;
      case 'A':
        errorlevel_ = stoi(argument);
        break;
      case 'H':
        hSockOrComm = stoi(argument);
        break;
      case 'M':
        ok_modem_stuff = false;
        break;
      case 'N': {
        instance_number_ = stoi(argument);
        if (instance_number_ <= 0 || instance_number_ > 999) {
          clog << "Your Instance can only be 1..999, you tried instance #" << instance_number_ << endl;
          session()->ExitBBSImpl(errorlevel_, false);
        }
      }
        break;
      case 'P':
        localIO()->Cls();
        localIO()->Printf("Waiting for keypress...");
        (void) getchar();
        break;
      case 'R':
        num_min = stoi(argument);
        break;
      case 'U':
        this_usernum = StringToUnsignedShort(argument);
        if (!user_already_on_) {
          SetCurrentSpeed("KB");
        }
        user_already_on_ = true;
        break;
      case 'V':
        cout << "WWIV Bulletin Board System [" << wwiv_version << beta_version << "]" << endl;
        ExitBBSImpl(0, false);
        break;
      case 'X': {
        char argument2Char = wwiv::UpperCase<char>(argument.at(0));
        if (argument2Char == 'T' || argument2Char == 'S' || argument2Char == 'U') {
          // This more of a hack to make sure the WWIV
          // Server's -Bxxx parameter doesn't hose us.
          SetCurrentSpeed("115200");

          // These are needed for both Telnet or SSH
          SetUserOnline(false);
          us = 115200;
          ui = us;
          user_already_on_ = true;
          ooneuser = true;
          using_modem = 0;
          hangup = false;
          incom = true;
          outcom = false;
          if (argument2Char == 'T') {
            type = CommunicationType::TELNET;
          } else if (argument2Char == 'S') {
            type = CommunicationType::SSH;
          }
        } else {
          clog << "Invalid Command line argument given '" << argumentRaw << "'" << std::endl;
          ExitBBSImpl(errorlevel_, false);
        }
      }
        break;
      case 'Z':
        no_hangup = true;
        break;
        //
        // TODO Add handling for Socket and Comm handle here
        //
        //
      case 'K': {
        if (!ReadConfig()) {
          std::clog << "Unable to load CONFIG.DAT";
          AbortBBS(true);
        }
        this->InitializeBBS();
        localIO()->Cls();
        if ((i + 1) < argc) {
          i++;
          bout << "\r\n|#7\xFE |#5Packing specified subs: \r\n";
          while (i < argc) {
            int nSubNumToPack = atoi(argv[i]);
            pack_sub(nSubNumToPack);
            sysoplog() << "* Packed Message Subboard:" << nSubNumToPack;
            i++;
          }
        } else {
          bout << "\r\n|#7\xFE |#5Packing all subboards: \r\n";
          sysoplog() << "* Packing All Message Subboards";
          wwiv::bbs::TempDisablePause disable_pause;
          if (!pack_all_subs()) {
            bout << "|#6Aborted.\r\n";
          }
        }
        ExitBBSImpl(oklevel_, true);
      }
        break;
      case '?':
        ShowUsage();
        ExitBBSImpl(0, false);
        break;
      case '-': {
        if (argumentRaw == "--help") {
          ShowUsage();
          ExitBBSImpl(0, false);
        }
      } break;
      default: {
        LOG(ERROR) << "Invalid Command line argument given '" << argument << "'";
        ExitBBSImpl(errorlevel_, false);
      }
        break;
      }
    } else {
      // Command line argument did not start with a '-' or a '/'
      LOG(ERROR) << "Invalid Command line argument given '" << argumentRaw << "'";
      ExitBBSImpl(errorlevel_, false);
    }
  }

  // Setup the full-featured localIO if we have a TTY (or console)
  if (isatty(fileno(stdin))) {
#if defined ( _WIN32 ) && !defined (WWIV_WIN32_CURSES_IO)
    reset_local_io(new Win32ConsoleIO());
#else
    if (type == CommunicationType::NONE) {
      // We only want the localIO if we ran this locally at a terminal
      // and also not passed in from the telnet handler, etc.  On Windows
      // We always have a local console, so this is *NIX specific.
      CursesIO::Init(StringPrintf("WWIV BBS %s%s", wwiv_version, beta_version));
      reset_local_io(new CursesLocalIO(out->GetMaxY()));
    }
    else if (type == CommunicationType::TELNET || type == CommunicationType::SSH) {
      reset_local_io(new NullLocalIO());
    }
#endif
  }
  else {
#ifdef __unix__
    reset_local_io(new NullLocalIO());
#endif  // __unix__
  }

  // Add the environment variable or overwrite the existing one
  const string env_str = std::to_string(instance_number());
  set_environment_variable("WWIV_INSTANCE", env_str);
  if (!ReadConfig()) {
    // Gotta read the config before we can create the socket handles.
    // Since we may need the SSH key.
    AbortBBS(true);
  }

  CreateComm(hSockOrComm, type);
  InitializeBBS();
  localIO()->UpdateNativeTitleBar(this);

  bool remote_opened = true;
  // If we are telnet...
  if (type == CommunicationType::TELNET || type == CommunicationType::SSH) {
    ok_modem_stuff = true;
    remote_opened = remoteIO()->open();
  }

  if (!remote_opened) {
    // Remote side disconnected.
    clog << "Remote side disconnected." << std::endl;
    ExitBBSImpl(oklevel_, false);
  }

  if (num_min > 0) {
    syscfg.executetime = static_cast<uint16_t>((timer() + num_min * 60) / 60);
    if (syscfg.executetime > 1440) {
      syscfg.executetime -= 1440;
    }
    time_event = static_cast<long>(syscfg.executetime) * MINUTES_PER_HOUR;
    last_time = time_event - timer();
    if (last_time < 0.0) {
      last_time += SECONDS_PER_DAY;
    }
  }

  if (event_only) {
    unique_ptr<WStatus> pStatus(status_manager()->GetStatus());
    cleanup_events();
    if (!IsEquals(date(), pStatus->GetLastDate())) {
      // This may be another node, but the user explicitly wanted to run the beginday
      // event from the commandline, so we'll just check the date.
      beginday(true);
    }
    else {
      sysoplog(false) << "!!! Wanted to run the beginday event when it's not required!!!";
      clog << "! WARNING: Tried to run beginday event again\r\n\n";
      sleep_for(seconds(2));
    }
    ExitBBSImpl(oklevel_, true);
  }

  do {
    if (this_usernum) {
      usernum = this_usernum;
      ReadCurrentUser();
      if (!user()->IsUserDeleted()) {
        GotCaller(ui, us);
        usernum = this_usernum;
        ReadCurrentUser();
        read_qscn(usernum, qsc, false);
        ResetEffectiveSl();
        changedsl();
        okmacro = true;
      }
      else {
        this_usernum = 0;
      }
    }
    try {
      if (!this_usernum) {
        if (user_already_on_) {
          GotCaller(ui, us);
        }
        else {
          GetCaller();
        }
      }

      if (using_modem > -1) {
        if (!this_usernum) {
          getuser();
        }
      }
      else {
        using_modem = 0;
        okmacro = true;
        usernum = unx_;
        ResetEffectiveSl();
        changedsl();
      }
      this_usernum = 0;
      CheckForHangup();
      logon();
      setiia(90);
      set_net_num(0);
      while (true) {
        CheckForHangup();
        filelist.clear();
        zap_ed_info();
        write_inst(INST_LOC_MAIN, current_user_sub().subnum, INST_FLAGS_NONE);
        wwiv::menus::mainmenu();
      }
    }
    catch (wwiv::bbs::hangup_error& h) {
      std::cerr << h.what() << "\r\n";
      sysoplog() << "hangup_error:" << h.what();
    }
    logoff();
    if (!no_hangup && using_modem && ok_modem_stuff) {
      hang_it_up();
    }
    catsl();
    frequent_init();
    if (wfc_status == 0) {
      localIO()->Cls();
    }
    cleanup_net();

    if (!no_hangup && ok_modem_stuff) {
      remoteIO()->disconnect();
    }
    user_already_on_ = false;
  } while (!ooneuser);

  return oklevel_;
}

