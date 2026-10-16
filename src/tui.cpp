#include "tui.hpp"
#include "admin.hpp"
#include <algorithm>
#include <array>
#include <clocale>
#include <curses.h>
#include <ctime>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace agent {
namespace {
enum Style { Plain=1, Accent, Muted, Selected, Good, Warning, Bad, Input, Focus };
constexpr int min_width=80, min_height=24;
std::string printable(const std::string& value) {
    std::string out;
    for(unsigned char c : value) {
        if(c == '\n' || c == '\t' || (c >= 32 && c < 127)) out += static_cast<char>(c);
        else { const char* hex="0123456789abcdef"; out += "\\x"; out += hex[c>>4]; out += hex[c&15]; }
    }
    return out;
}
void text(int y, int x, const std::string& value, int style=Plain, int limit=-1, bool bold=false) {
    if(y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    auto width = limit < 0 ? COLS-x-1 : std::min(limit, COLS-x-1);
    if(width <= 0) return;
    attrset(COLOR_PAIR(style) | (bold ? A_BOLD : A_NORMAL));
    mvaddnstr(y,x,value.c_str(),width);
    attrset(A_NORMAL);
}
void fill(int y, int x, int width, int style) {
    if(y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    attrset(COLOR_PAIR(style));
    mvhline(y,x,' ',std::max(0,std::min(width,COLS-x)));
    attrset(A_NORMAL);
}
void panel(int y, int x, int h, int w, const std::string& title) {
    if(h < 2 || w < 2 || y+h > LINES || x+w > COLS) return;
    attrset(COLOR_PAIR(Muted));
    mvhline(y,x+1,ACS_HLINE,w-2); mvhline(y+h-1,x+1,ACS_HLINE,w-2);
    mvvline(y+1,x,ACS_VLINE,h-2); mvvline(y+1,x+w-1,ACS_VLINE,h-2);
    mvaddch(y,x,ACS_ULCORNER); mvaddch(y,x+w-1,ACS_URCORNER);
    mvaddch(y+h-1,x,ACS_LLCORNER); mvaddch(y+h-1,x+w-1,ACS_LRCORNER);
    text(y,x+2," "+title+" ",Accent,w-4,true);
}
bool size_ok() {
    if(COLS >= min_width && LINES >= min_height) return true;
    erase();
    text(1,2,"4agent",Accent,-1,true);
    text(3,2,"Resize terminal to at least 80 columns x 24 rows.");
    text(5,2,"Q / Esc to quit",Muted);
    refresh();
    return false;
}
void header(const std::string& title, const std::string& subtitle) {
    erase();
    fill(0,0,COLS,Selected);
    text(0,2,"4agent",Selected,10,true);
    text(0,13,"TEMPORARY AGENT ACCESS",Selected);
    text(2,3,title,Plain,-1,true);
    text(3,3,subtitle,Muted);
}
std::vector<std::string> wrap(const std::string& value, int width) {
    std::vector<std::string> lines;
    std::istringstream input(printable(value));
    std::string line;
    width = std::max(1,width);
    while(std::getline(input,line)) {
        for(char& c : line) if(c == '\t') c = ' ';
        if(line.empty()) { lines.emplace_back(); continue; }
        for(std::size_t pos=0; pos<line.size(); pos+=static_cast<std::size_t>(width))
            lines.push_back(line.substr(pos,static_cast<std::size_t>(width)));
    }
    if(lines.empty()) lines.emplace_back();
    return lines;
}
void viewer(const std::string& title, const std::string& content, int style=Plain) {
    int offset=0;
    curs_set(0);
    for(;;) {
        if(!size_ok()) { auto key=getch(); if(key=='q' || key==27) return; continue; }
        header(title,"Read-only view  /  use arrow keys or Page Up / Page Down to scroll");
        panel(5,2,LINES-8,COLS-4," DETAILS ");
        auto lines = wrap(content,COLS-10);
        int height=LINES-10;
        offset=std::clamp(offset,0,std::max(0,static_cast<int>(lines.size())-height));
        for(int i=0; i<height && offset+i<static_cast<int>(lines.size()); ++i)
            text(6+i,5,lines[static_cast<std::size_t>(offset+i)],style,COLS-10);
        text(LINES-2,3,"Esc / Enter  Back",Accent);
        text(LINES-2,COLS-24,std::to_string(offset+1)+" / "+std::to_string(lines.size()),Muted,21);
        refresh();
        auto key=getch();
        if(key==27 || key==3 || key=='q' || key=='\n' || key==KEY_ENTER) return;
        if(key==KEY_DOWN || key=='j') ++offset;
        if(key==KEY_UP || key=='k') --offset;
        if(key==KEY_NPAGE) offset+=height;
        if(key==KEY_PPAGE) offset-=height;
        if(key==KEY_HOME) offset=0;
        if(key==KEY_END) offset=static_cast<int>(lines.size());
    }
}
bool confirm(const std::string& name) {
    bool remove=false;
    for(;;) {
        if(!size_ok()) { auto key=getch(); if(key==27) return false; continue; }
        header("Revoke account", "This removes the account and its home, and terminates its user processes.");
        panel(6,4,11,COLS-8," CONFIRM REMOVAL ");
        text(8,7,name,Warning,-1,true);
        text(10,7,"Command history will be archived if available.",Muted);
        fill(13,7,16,!remove ? Selected : Plain);
        text(13,8,"Cancel",!remove ? Selected : Plain,14,true);
        fill(13,27,20,remove ? Bad : Plain);
        text(13,28,"Revoke account",remove ? Bad : Plain,18,true);
        text(LINES-2,3,"Left / Right  Select     Enter  Confirm     Esc  Cancel",Muted);
        refresh();
        auto key=getch();
        if(key==27 || key==3) return false;
        if(key=='\t' || key==KEY_LEFT || key==KEY_RIGHT) remove=!remove;
        if(key=='\n' || key==KEY_ENTER) return remove;
    }
}
struct Capture {
    std::ostringstream output;
    std::streambuf* out=std::cout.rdbuf(output.rdbuf());
    std::streambuf* err=std::cerr.rdbuf(output.rdbuf());
    ~Capture() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
};
bool perform(const std::string& title, const std::function<void()>& action) {
    header(title,"Working... Please wait.");
    text(7,4,"Account changes are serialized with the expiration service.",Muted);
    refresh();
    std::string error;
    {
        Capture capture;
        try { action(); }
        catch(const std::exception& e) { error=e.what(); }
    }
    if(!error.empty()) { viewer("Action failed",error,Bad); return false; }
    return true;
}
void create_form() {
    std::array<std::string,6> values{
        "agent_"+std::to_string(std::time(nullptr)),"60","Standard user","","22",default_export_directory};
    const std::array<std::string,6> labels{"Username","Lifetime (minutes)","Permissions","Server public IP","SSH port","Export directory"};
    const std::array<std::string,6> hints{
        "agent_ + lowercase letters, digits or underscores",
        "Account lifetime: 1 to 525600 minutes",
        "Left / Right or Space to change permissions",
        "Leave blank to detect automatically via HTTPS",
        "Externally reachable port; SSH configuration stays as configured",
        "Private keys require a Linux directory with owner-only access"};
    int focus=0;
    std::size_t caret=values[0].size();
    bool root=false;
    for(;;) {
        if(!size_ok()) { curs_set(0); auto key=getch(); if(key==27) return; continue; }
        header("Create agent account","Provision a temporary Linux login and export its SSH credentials.");
        int form_width=std::min(COLS-8,110);
        int left=(COLS-form_width)/2;
        panel(5,left,14,form_width," ACCOUNT CONFIGURATION ");
        int field_left=left+24, field_width=form_width-28;
        for(int i=0; i<6; ++i) {
            int y=6+i*2;
            text(y,left+3,labels[static_cast<std::size_t>(i)],focus==i ? Accent : Muted,20,focus==i);
            int style=focus==i ? Focus : Input;
            fill(y,field_left,field_width,style);
            auto value=values[static_cast<std::size_t>(i)];
            if(i==3 && value.empty() && focus!=i) value="Detect automatically";
            if(i==2) value=root ? "< Root via sudo >" : "< Standard user >";
            std::size_t start=0;
            if(focus==i && i!=2 && caret>=static_cast<std::size_t>(field_width-2)) start=caret-static_cast<std::size_t>(field_width-3);
            text(y,field_left+1,value.substr(start),style,field_width-2);
        }
        std::string hint=focus<6 ? hints[static_cast<std::size_t>(focus)] : "Enter to activate the selected action";
        text(19,left+3,hint,Muted,form_width-6);
        text(20,left+2,root ? "Root can disable expiry and persist access. History is user-editable." : "Standard access  /  SSH key expiry  /  Automatic account cleanup",root ? Warning : Good,form_width-4);
        int button_y=LINES-3;
        fill(button_y,left+2,22,focus==6 ? Selected : Input);
        text(button_y,left+3,"Create account",focus==6 ? Selected : Input,20,true);
        fill(button_y,left+27,13,focus==7 ? Selected : Input);
        text(button_y,left+28,"Cancel",focus==7 ? Selected : Input,11,true);
        text(LINES-1,3,"Tab / Arrows Navigate   Enter Continue   Ctrl+U Clear   Esc Cancel",Muted);
        if(focus<6 && focus!=2) {
            auto start=caret>=static_cast<std::size_t>(field_width-2) ? caret-static_cast<std::size_t>(field_width-3) : 0;
            move(6+focus*2,field_left+1+static_cast<int>(caret-start));
            curs_set(1);
        } else curs_set(0);
        refresh();
        auto key=getch();
        if(key==27 || key==3) { curs_set(0); return; }
        auto change_focus=[&](int next) { focus=(next+8)%8; if(focus<6) caret=values[static_cast<std::size_t>(focus)].size(); };
        if(key==KEY_RESIZE) continue;
        if(key=='\t' || key==KEY_DOWN) { change_focus(focus+1); continue; }
        if(key==KEY_BTAB || key==KEY_UP) { change_focus(focus-1); continue; }
        if(key=='\n' || key==KEY_ENTER) {
            if(focus==7) { curs_set(0); return; }
            if(focus!=6) { change_focus(focus+1); continue; }
            curs_set(0);
            std::filesystem::path report;
            auto success=perform("Creating account",[&]{
                Options o;
                o.name=values[0]; o.minutes=number(values[1],1,525600); o.root=root;
                o.ip=values[3]; o.port=number(values[4],1,65535); o.output=values[5];
                Admin admin; report=admin.create(o);
            });
            if(success) {
                viewer("Account created", "Username: "+values[0]+"\nPermissions: "+(root ? "Root via passwordless sudo" : "Standard user")+
                    "\nLifetime: "+values[1]+" minutes\n\nCredentials saved to:\n"+report.string()+
                    "\n\nThe file contains the username, public and private SSH keys, public IP, SSH port, and connection instructions.\n\nRead it as root. Keep the private key secret.",Good);
                return;
            }
            continue;
        }
        if(focus==2) {
            if(key==' ' || key==KEY_LEFT || key==KEY_RIGHT) root=!root;
            continue;
        }
        if(focus>=6) { if(key==KEY_LEFT || key==KEY_RIGHT) change_focus(focus==6 ? 7 : 6); continue; }
        auto& value=values[static_cast<std::size_t>(focus)];
        if(key==KEY_LEFT && caret>0) --caret;
        else if(key==KEY_RIGHT && caret<value.size()) ++caret;
        else if(key==KEY_HOME || key==1) caret=0;
        else if(key==KEY_END || key==5) caret=value.size();
        else if((key==KEY_BACKSPACE || key==127 || key==8) && caret>0) { value.erase(--caret,1); }
        else if(key==KEY_DC && caret<value.size()) value.erase(caret,1);
        else if(key==21) { value.clear(); caret=0; }
        else if(key>=32 && key<127 && value.size()<1024) { value.insert(caret,1,static_cast<char>(key)); ++caret; }
    }
}
std::string history_name() {
    std::string name="agent_";
    for(;;) {
        if(!size_ok()) { auto key=getch(); if(key==27) return {}; continue; }
        header("Open command history","Enter the username of an active or expired account.");
        panel(6,4,7,COLS-8," ACCOUNT ");
        text(8,7,"Username",Muted);
        fill(10,7,COLS-15,Focus);
        text(10,8,name,Focus,COLS-18);
        text(LINES-2,3,"Enter  Open history    Backspace  Edit    Esc  Cancel",Muted);
        move(10,8+std::min(static_cast<int>(name.size()),COLS-19)); curs_set(1); refresh();
        auto key=getch();
        if(key==27 || key==3) { curs_set(0); return {}; }
        if(key=='\n' || key==KEY_ENTER) { curs_set(0); return name; }
        if((key==KEY_BACKSPACE || key==127 || key==8) && !name.empty()) name.pop_back();
        else if(key>=32 && key<127 && name.size()<30) name+=static_cast<char>(key);
    }
}
std::string remaining(const Account& account) {
    auto seconds=account.expires-std::time(nullptr);
    if(seconds<=0) return "Expiry pending";
    if(seconds<60) return std::to_string(seconds)+" sec";
    if(seconds<3600) return std::to_string((seconds+59)/60)+" min";
    if(seconds<86400) return std::to_string(seconds/3600)+"h "+std::to_string((seconds%3600)/60)+"m";
    return std::to_string(seconds/86400)+" days";
}
class Screen {
public:
    Screen() {
        if(!initscr()) throw std::runtime_error("Cannot initialize terminal");
        raw(); noecho(); keypad(stdscr,TRUE); curs_set(0); set_escdelay(25);
        if(has_colors()) {
            start_color(); use_default_colors();
            init_pair(Plain,COLOR_WHITE,-1); init_pair(Accent,COLOR_CYAN,-1);
            init_pair(Muted,COLOR_BLUE,-1); init_pair(Selected,COLOR_BLACK,COLOR_CYAN);
            init_pair(Good,COLOR_GREEN,-1); init_pair(Warning,COLOR_YELLOW,-1);
            init_pair(Bad,COLOR_RED,-1); init_pair(Input,COLOR_WHITE,COLOR_BLACK);
            init_pair(Focus,COLOR_BLACK,COLOR_WHITE);
        }
    }
    ~Screen() { endwin(); }
};
}
void tui() {
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) throw std::runtime_error("TUI requires a terminal. Use --help for CLI commands");
    if(geteuid()!=0) throw std::runtime_error("Run with sudo: sudo 4agent");
    std::setlocale(LC_ALL,"");
    Screen screen;
    std::vector<Account> accounts;
    int selected=0, menu=0;
    bool sidebar=false;
    std::string status="Ready. Press N to create your first account.";
    auto reload=[&] {
        try { Admin admin; accounts=admin.accounts(); }
        catch(const std::exception& e) { status="Error: "+printable(e.what()); }
        selected=std::clamp(selected,0,std::max(0,static_cast<int>(accounts.size())-1));
    };
    reload();
    auto next_refresh=std::time(nullptr)+3;
    for(;;) {
        timeout(1000);
        if(!size_ok()) { auto key=getch(); if(key=='q' || key==27) return; continue; }
        if(std::time(nullptr)>=next_refresh) { reload(); next_refresh=std::time(nullptr)+3; }
        header("Agent access manager",hostname()+"  /  Linux accounts  /  Expiration checked every 15 seconds");
        int root_count=static_cast<int>(std::count_if(accounts.begin(),accounts.end(),[](const auto& a){ return a.root; }));
        panel(5,2,LINES-9,21," WORKSPACE ");
        std::array<std::string,5> items{"Accounts","Create account","Open history","Expire now","Quit"};
        for(int i=0; i<5; ++i) {
            int style=sidebar && menu==i ? Selected : (i==0 ? Accent : Plain);
            fill(7+i*2,4,17,style); text(7+i*2,5,items[static_cast<std::size_t>(i)],style,15,i==0);
        }
        text(LINES-7,5,"KEY EXPORTS",Muted,15,true);
        text(LINES-6,5,"Linux storage",Good,15);
        int x=25, width=COLS-27;
        text(5,x,std::to_string(accounts.size())+" ACCOUNTS",Accent,20,true);
        text(5,x+22,std::to_string(root_count)+" ROOT",Warning,16,true);
        panel(7,x,LINES-15,width," ACTIVE ACCOUNTS ");
        text(9,x+2,"USERNAME",Muted,30,true);
        text(9,x+width-30,"ACCESS",Muted,12,true);
        text(9,x+width-16,"TIME LEFT",Muted,14,true);
        int visible=LINES-20;
        int start=selected>=visible ? selected-visible+1 : 0;
        if(accounts.empty()) {
            text(11,x+3,"No active accounts",Plain,width-6,true);
            text(13,x+3,"Press N to create one.",Muted,width-6);
        }
        for(int i=0; i<visible && start+i<static_cast<int>(accounts.size()); ++i) {
            const auto& a=accounts[static_cast<std::size_t>(start+i)];
            bool active=selected==start+i;
            int style=active && !sidebar ? Selected : Plain;
            fill(11+i,x+1,width-2,style);
            text(11+i,x+2,(active ? "> " : "  ")+a.name,style,width-34,active);
            text(11+i,x+width-30,a.root ? "sudo root" : "standard",style,12);
            text(11+i,x+width-16,remaining(a),style,14);
        }
        if(!accounts.empty()) {
            const auto& a=accounts[static_cast<std::size_t>(selected)];
            text(LINES-7,x,"Expires: "+utc(a.expires),Muted,width);
            text(LINES-6,x,"UID "+std::to_string(a.uid)+"  /  H history  /  D revoke",Plain,width);
        }
        text(LINES-4,3,status,status.starts_with("Error:") ? Bad : Good,COLS-6);
        fill(LINES-2,0,COLS,Input);
        text(LINES-2,2,"N New  H History  D Revoke  R Refresh  Tab Panels  Arrows Select  Q Quit",Input);
        refresh();
        auto key=getch();
        timeout(-1);
        if(key==ERR || key==KEY_RESIZE) continue;
        if(key=='q' || key=='Q' || key==27 || key==3) return;
        if(key=='\t' || key==KEY_BTAB || key==KEY_LEFT || key==KEY_RIGHT) { sidebar=!sidebar; continue; }
        if(key==KEY_UP || key=='k') { if(sidebar) menu=(menu+4)%5; else selected=std::max(0,selected-1); continue; }
        if(key==KEY_DOWN || key=='j') { if(sidebar) menu=(menu+1)%5; else selected=std::min(std::max(0,static_cast<int>(accounts.size())-1),selected+1); continue; }
        int action=-1;
        if(key=='n' || key=='N') action=1;
        if(key=='h' || key=='H') action=2;
        if(key=='d' || key=='D' || key==KEY_DC) action=5;
        if(key=='r' || key=='R') { reload(); status="Accounts refreshed."; continue; }
        if(key=='\n' || key==KEY_ENTER) action=sidebar ? menu : 2;
        if(action==0) sidebar=false;
        if(action==1) { create_form(); reload(); status="Account list refreshed."; }
        if(action==2) {
            auto name=accounts.empty() || sidebar ? history_name() : accounts[static_cast<std::size_t>(selected)].name;
            if(!name.empty()) {
                std::string history;
                if(perform("Loading command history",[&]{ Admin admin; history=admin.history(name); }))
                    viewer("History / "+name,history.empty() ? "No commands recorded yet.\n\nHistory is user-editable, not an audit log." : history);
            }
        }
        if(action==3) {
            if(perform("Removing expired accounts",[]{ Admin admin; admin.reap(); })) status="Expired accounts processed.";
            reload();
        }
        if(action==4) return;
        if(action==5 && !accounts.empty()) {
            auto name=accounts[static_cast<std::size_t>(selected)].name;
            if(confirm(name) && perform("Revoking "+name,[&]{ Admin admin; admin.revoke(name); })) status="Revoked "+name+". History archived if available.";
            reload();
        }
    }
}
}
