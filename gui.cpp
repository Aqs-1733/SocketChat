#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace std;
namespace fs = std::filesystem;
enum { OPEN_FILE=101, SAVE_FILE, START, STOP, CLEAR, SAVE_LOG };
HWND mainWnd, input, output, winBox, lossBox, delayBox, timeoutBox, portBox, corruptBox;
HWND startBtn, stopBtn, logBox, progress, statusBox, speedBox, timeBox, retryBox, rttBox, windowBox, recvWindowBox;
HFONT normalFont, titleFont, valueFont, logFont;
HBRUSH background, headerBrush, whiteBrush;
const COLORREF navy=RGB(24,42,63), blue=RGB(25,112,189);
struct Child {
    HANDLE process=nullptr, pipe=nullptr;
    wstring name;
    string rest;
    DWORD code=STILL_ACTIVE, pid=0;
};
Child children[3];
bool running=false, senderOk=false, receiverOk=false, tempCreated=false;
fs::path logPath, outputPath;
ofstream fullLog;
unsigned long long dropped=0, corrupted=0;
unsigned long long total=0, base=0, retries=0;
int windowSize=16;

wstring text(HWND h) {
    int n=GetWindowTextLengthW(h);
    wstring s(n+1,L'\0');
    GetWindowTextW(h,s.data(),n+1);
    s.resize(n);
    return s;
}
void set(HWND h,const wstring& s) { SetWindowTextW(h,s.c_str()); }
HWND control(const wchar_t* cls,const wchar_t* title,DWORD style,int x,int y,int w,int h,int id=0,DWORD ex=0) {
    HWND c=CreateWindowExW(ex,cls,title,WS_CHILD|WS_VISIBLE|style,x,y,w,h,mainWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,(WPARAM)normalFont,TRUE);
    return c;
}
HWND label(const wchar_t* title,int x,int y,int w,int h=22) {
    return control(L"STATIC",title,SS_LEFT,x,y,w,h);
}
HWND edit(const wchar_t* value,int x,int y,int w,bool numeric=false) {
    return control(L"EDIT",value,WS_TABSTOP|ES_AUTOHSCROLL|(numeric?ES_NUMBER:0),x,y,w,28,0,WS_EX_CLIENTEDGE);
}
HWND button(const wchar_t* value,int x,int y,int w,int id) {
    return control(L"BUTTON",value,WS_TABSTOP|BS_PUSHBUTTON,x,y,w,30,id);
}
HFONT font(int size,int weight,const wchar_t* face=L"Microsoft YaHei UI") {
    return CreateFontW(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,face);
}
void append(const wstring& s) {
    int n=GetWindowTextLengthW(logBox);
    //保留最新日志，避免长文件传输占用过多界面内存
    if(n>180000) {
        SendMessageW(logBox,EM_SETSEL,0,60000);
        SendMessageW(logBox,EM_REPLACESEL,FALSE,(LPARAM)L"");
        n=GetWindowTextLengthW(logBox);
    }
    SendMessageW(logBox,EM_SETSEL,n,n);
    SendMessageW(logBox,EM_REPLACESEL,FALSE,(LPARAM)s.c_str());
    SendMessageW(logBox,EM_SCROLLCARET,0,0);
}
wstring widen(const string& s) { return wstring(s.begin(),s.end()); }
string field(const string& s,const string& key) {
    size_t p=s.find(key);
    if(p==string::npos) return {};
    p+=key.size();
    return s.substr(p,s.find(' ',p)-p);
}
//根据真实协议日志更新统计量
void onLine(int i,const string& s) {
    if(i==2) {
        if(s.find("[START]")==0) {
            try { total=stoull(field(s,"packets=")); } catch(...) {}
        }
        if(s.find("[WINDOW]")==0) {
            auto p=s.find(" -> ");
            if(p!=string::npos) {
                try { base=stoull(s.substr(p+4)); } catch(...) {}
                SendMessageW(progress,PBM_SETPOS,total?int(base*1000/total):0,0);
                set(windowBox,L"发送窗口  ["+to_wstring(base)+L", "+to_wstring(min(total,base+windowSize))+L")   已确认连续包 "+to_wstring(base)+L" / "+to_wstring(total));
            }
        }
        if(s.find("[RETX]")==0) set(retryBox,to_wstring(++retries));
        if(s.find("[ACK]")==0 && s.find("RTT=")!=string::npos)
            set(rttBox,widen(field(s,"RTT="))+L" (当前)");
        if(s.find("[CONNECTED]")==0) set(statusBox,L"已建立连接 · 正在传输");
        if(s.find("[FIN]")==0) set(statusBox,L"等待接收端校验与关闭");
        if(s.find("[SUCCESS]")==0) {
            senderOk=true;
            set(retryBox,widen(field(s,"retransmissions=")));
            set(rttBox,widen(field(s,"avg_RTT=")));
        }
        if(s.find("[RESULT]")==0) {
            set(speedBox,widen(field(s,"throughput=")));
            set(timeBox,widen(field(s,"time=")));
        }
    }
    if(i==0) {
        if(s.find("[TEMP]")==0) tempCreated=true;
        if(s.find("[SUCCESS]")==0) receiverOk=true;
        if(s.find("[RECV_WINDOW]")==0)
            set(recvWindowBox,L"接收窗口  ["+widen(field(s,"base="))+L", "+widen(field(s,"end="))+L")   乱序缓存 "+widen(field(s,"buffered=")));
    }
    if(i==1) {
        if(s.find("[DROP]")==0) ++dropped;
        if(s.find("[CORRUPT]")==0) ++corrupted;
    }
}
//通过匿名管道读取三个进程的日志
void drain(int i) {
    auto& c=children[i];
    if(!c.pipe) return;
    DWORD available=0,n=0;
    char buf[4096];
    wstring batch;
    for(int k=0;k<32 && PeekNamedPipe(c.pipe,nullptr,0,nullptr,&available,nullptr) && available;k++) {
        if(!ReadFile(c.pipe,buf,min<DWORD>(available,sizeof(buf)),&n,nullptr)||!n) break;
        c.rest.append(buf,n);
        size_t end;
        while((end=c.rest.find('\n'))!=string::npos) {
            string line=c.rest.substr(0,end);
            c.rest.erase(0,end+1);
            if(!line.empty() && line.back()=='\r') line.pop_back();
            if(fullLog.is_open()) {
                const char* role[]={"RECV","ROUTE","SEND"};
                fullLog << '[' << role[i] << "] " << line << '\n';
            }
            onLine(i,line);
            batch+=L"["+c.name+L"] "+widen(line)+L"\r\n";
        }
    }
    if(fullLog.is_open()) fullLog.flush();
    if(!batch.empty()) append(batch);
}
void closeChild(Child& c) {
    if(c.process) {
        if(WaitForSingleObject(c.process,0)==WAIT_TIMEOUT) {
            TerminateProcess(c.process,2);
            WaitForSingleObject(c.process,2000);
        }
        CloseHandle(c.process);
    }
    if(c.pipe) CloseHandle(c.pipe);
    c=Child{};
}
void lockParams(bool lock) {
    HWND fields[]={input,output,winBox,lossBox,delayBox,timeoutBox,portBox,corruptBox,
                   GetDlgItem(mainWnd,OPEN_FILE),GetDlgItem(mainWnd,SAVE_FILE)};
    for(HWND h:fields) EnableWindow(h,!lock);
}
void finish(const wchar_t* message) {
    DWORD receiverPid=children[0].pid;
    //先停止进程，再读取剩余日志
    for(int i=0;i<3;++i) {
        auto& c=children[i];
        if(c.process && WaitForSingleObject(c.process,0)==WAIT_TIMEOUT) {
            TerminateProcess(c.process,2);
            WaitForSingleObject(c.process,2000);
        }
        drain(i);
        while(c.pipe) {
            DWORD available=0;
            if(!PeekNamedPipe(c.pipe,nullptr,0,nullptr,&available,nullptr)||!available) break;
            drain(i);
        }
        closeChild(c);
    }
    if(tempCreated && receiverPid) {
        auto part=outputPath;
        part+=L".part."+to_wstring(receiverPid);
        error_code ec;
        fs::remove(part,ec);
        if(ec) append(L"[系统] 临时文件清理失败，请检查接收目录。\r\n");
    }
    running=false;
    set(statusBox,message);
    append(wstring(L"[系统] ")+message+L"\r\n");
    if(fullLog.is_open()) {
        fullLog << "[GUI] sender_verified=" << senderOk << " receiver_verified=" << receiverOk
                << " dropped=" << dropped << " corrupted=" << corrupted << '\n';
        fullLog.close();
        if(fullLog.fail()) append(L"[系统] 日志写入失败。\r\n");
        else append(L"[系统] 完整日志："+logPath.wstring()+L"\r\n");
    }
    EnableWindow(startBtn,TRUE);
    EnableWindow(stopBtn,FALSE);
    lockParams(false);
}
wstring quote(const wstring& s) {
    //文件路径以引号包围，末尾反斜杠需转义
    size_t n=0;
    while(n<s.size() && s[s.size()-1-n]==L'\\') ++n;
    return L"\""+s+wstring(n,L'\\')+L"\"";
}
void launch(int i,const wstring& name,const wstring& args) {
    wchar_t path[32768];
    DWORD len=GetModuleFileNameW(nullptr,path,32768);
    if(!len || len>=32768) throw runtime_error("cannot locate executable");
    wstring exe=(fs::path(path).parent_path()/L"reliable_udp.exe").wstring();
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE read=nullptr,write=nullptr;
    if(!CreatePipe(&read,&write,&sa,65536)) throw runtime_error("CreatePipe failed");
    if(!SetHandleInformation(read,HANDLE_FLAG_INHERIT,0)) {
        CloseHandle(read); CloseHandle(write); throw runtime_error("pipe setup failed");
    }
    HANDLE nullIn=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    if(nullIn==INVALID_HANDLE_VALUE) {
        CloseHandle(read); CloseHandle(write); throw runtime_error("cannot open NUL");
    }
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=si.hStdError=write; si.hStdInput=nullIn;
    PROCESS_INFORMATION pi{};
    wstring cmd=quote(exe)+L" "+args;
    BOOL ok=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    CloseHandle(write); CloseHandle(nullIn);
    if(!ok) { CloseHandle(read); throw runtime_error("cannot start reliable_udp.exe; run build.bat first"); }
    CloseHandle(pi.hThread);
    children[i].process=pi.hProcess; children[i].pipe=read; children[i].name=name; children[i].pid=pi.dwProcessId;
}
int number(HWND h,int lo,int hi) {
    wstring s=text(h);
    if(s.empty()||s.find_first_not_of(L"0123456789")!=wstring::npos) throw runtime_error("invalid number");
    int n=stoi(s);
    if(n<lo||n>hi) throw runtime_error("parameter out of range");
    return n;
}
void start() {
    if(running) return;
    try {
        fs::path src(text(input)),dst(text(output));
        if(src.empty()||!fs::is_regular_file(src)||dst.empty()||fs::is_directory(dst)) throw runtime_error("choose valid source and output files");
        if(fs::exists(dst)&&fs::equivalent(src,dst)) throw runtime_error("source and output must differ");
        int w=number(winBox,1,4096),loss=number(lossBox,0,99),delay=number(delayBox,0,10000),corrupt=number(corruptBox,0,99);
        int timeout=number(timeoutBox,100,60000),port=number(portBox,1024,65534);
        if(timeout<=2*delay+20) {
            timeout=2*delay+100;
            set(timeoutBox,to_wstring(timeout));
            MessageBoxW(mainWnd,L"重传超时已调整为往返时延加100ms，避免无效重传。",L"参数提醒",MB_OK|MB_ICONINFORMATION);
        }
        if(fs::exists(dst)&&MessageBoxW(mainWnd,L"接收文件已存在，是否覆盖？",L"确认",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        set(logBox,L""); set(speedBox,L"--"); set(timeBox,L"--"); set(retryBox,L"--"); set(rttBox,L"--");
        total=base=retries=dropped=corrupted=0; windowSize=w;
        senderOk=receiverOk=tempCreated=false;
        outputPath=fs::absolute(dst);
        logPath=outputPath.parent_path()/(L"transfer_"+to_wstring(GetCurrentProcessId())+L"_"+to_wstring(GetTickCount64())+L".log");
        fullLog.clear();
        fullLog.open(logPath,ios::binary|ios::out|ios::trunc);
        if(!fullLog) throw runtime_error("cannot create experiment log");
        fullLog << "[PARAM] window=" << w << " loss=" << loss << "% delay=" << delay
                << "ms corrupt=" << corrupt << "% timeout=" << timeout << "ms\n";
        SendMessageW(progress,PBM_SETPOS,0,0);
        set(windowBox,L"发送窗口：等待连接"); set(recvWindowBox,L"接收窗口：等待连接"); set(statusBox,L"正在建立连接...");
        wstring p=to_wstring(port),r=to_wstring(port+1),sw=to_wstring(w),t=to_wstring(timeout);
        //接收端、路由器、发送端依次启动，双方窗口使用相同大小
        launch(0,L"接收",L"recv "+quote(dst.wstring())+L" "+r+L" "+sw+L" "+t);
        launch(1,L"路由",L"route "+p+L" 127.0.0.1 "+r+L" "+to_wstring(loss)+L" "+to_wstring(delay)+L" "+to_wstring(corrupt));
        launch(2,L"发送",L"send "+quote(src.wstring())+L" 127.0.0.1 "+p+L" "+sw+L" "+t);
        running=true;
        lockParams(true);
        EnableWindow(startBtn,FALSE); EnableWindow(stopBtn,TRUE);
    } catch(const exception& e) {
        finish(L"启动失败");
        MessageBoxW(mainWnd,widen(e.what()).c_str(),L"请检查文件与参数",MB_OK|MB_ICONERROR);
    }
}
void poll() {
    if(!running) return;
    for(int i=0;i<3;++i) {
        drain(i);
        auto& c=children[i];
        if(WaitForSingleObject(c.process,0)==WAIT_OBJECT_0) {
            GetExitCodeProcess(c.process,&c.code);
            if(c.code!=0 || i==1) { finish(L"传输失败 · 请检查日志"); return; }
            drain(i);
        }
    }
    if(senderOk && receiverOk && children[2].code==0 && children[0].code==0) {
        SendMessageW(progress,PBM_SETPOS,1000,0);
        finish((L"传输完成 · 校验通过 · 丢包 "+to_wstring(dropped)+L" · 误码 "+to_wstring(corrupted)).c_str());
    } else if(senderOk && receiverOk) set(statusBox,L"文件校验通过 · 等待连接关闭");
    else if(children[2].code==0 && !senderOk) finish(L"发送端异常结束");
}
void choose(bool save) {
    wchar_t path[32768]{};
    OPENFILENAMEW of{}; of.lStructSize=sizeof(of); of.hwndOwner=mainWnd;
    of.lpstrFile=path; of.nMaxFile=32768; of.lpstrFilter=L"所有文件\0*.*\0\0";
    of.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|(save?0:OFN_FILEMUSTEXIST);
    if(save?GetSaveFileNameW(&of):GetOpenFileNameW(&of)) {
        set(save?output:input,path);
        if(!save) {
            fs::path p(path);
            set(output,(p.parent_path()/(L"received_"+p.filename().wstring())).wstring());
        }
    }
}
void saveLog() {
    if(running || logPath.empty()) {
        MessageBoxW(mainWnd,L"请完成本次传输后保存日志。",L"日志",MB_OK|MB_ICONINFORMATION);
        return;
    }
    wchar_t path[32768]=L"transfer.log";
    OPENFILENAMEW of{}; of.lStructSize=sizeof(of); of.hwndOwner=mainWnd;
    of.lpstrFile=path; of.nMaxFile=32768; of.lpstrFilter=L"日志文件\0*.log\0所有文件\0*.*\0\0";
    of.lpstrDefExt=L"log"; of.Flags=OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;
    if(GetSaveFileNameW(&of)) {
        try {
            if(!fs::exists(path)||!fs::equivalent(logPath,path)) fs::copy_file(logPath,path,fs::copy_options::overwrite_existing);
            append(L"[系统] 日志已保存。\r\n");
        } catch(const exception&) { MessageBoxW(mainWnd,L"无法保存日志。",L"错误",MB_OK|MB_ICONERROR); }
    }
}
void setup() {
    normalFont=font(15,FW_NORMAL); titleFont=font(26,FW_BOLD); valueFont=font(23,FW_SEMIBOLD); logFont=font(14,FW_NORMAL,L"Consolas");
    HWND h=label(L"可靠传输协议  /  Reliable UDP",26,18,800,36);
    SendMessageW(h,WM_SETFONT,(WPARAM)titleFont,TRUE);
    label(L"计算机网络实验三    ·    Selective Repeat / UDP",28,59,800);
    label(L"01   文件传输",26,104,180);
    label(L"发送文件",26,142,84); input=edit(L"",116,137,650); button(L"浏览...",780,136,100,OPEN_FILE);
    label(L"接收文件",26,181,84); output=edit(L"",116,176,650); button(L"另存为...",780,175,100,SAVE_FILE);
    label(L"02   网络参数",26,220,180);
    label(L"窗口大小 / 包",26,252,130); winBox=edit(L"16",26,276,128,true);
    label(L"丢包率 / %",170,252,130); lossBox=edit(L"10",170,276,128,true);
    label(L"单向时延 / ms",314,252,130); delayBox=edit(L"40",314,276,128,true);
    label(L"报文误码 / %",458,252,130); corruptBox=edit(L"0",458,276,128,true);
    label(L"重传超时 / ms",602,252,130); timeoutBox=edit(L"800",602,276,128,true);
    label(L"路由端口",746,252,130); portBox=edit(L"9000",746,276,134,true);
    startBtn=button(L"开始传输",26,320,160,START); stopBtn=button(L"停止",198,320,90,STOP); EnableWindow(stopBtn,FALSE);
    statusBox=label(L"等待开始",310,325,570,26);
    progress=control(PROGRESS_CLASSW,L"",PBS_SMOOTH,26,365,854,13); SendMessageW(progress,PBM_SETRANGE32,0,1000);
    windowBox=label(L"发送窗口  [0, 0)",26,385,854);
    recvWindowBox=label(L"接收窗口  [0, 0)",26,407,854);
    const wchar_t* titles[]={L"平均吞吐率",L"传输耗时",L"数据重传次数",L"RTT（结束后取平均）"};
    HWND* values[]={&speedBox,&timeBox,&retryBox,&rttBox};
    for(int i=0;i<4;++i) {
        int x=26+i*215;
        label(titles[i],x+12,448,192); *values[i]=label(L"--",x+12,471,192,36);
        SendMessageW(*values[i],WM_SETFONT,(WPARAM)valueFont,TRUE);
    }
    label(L"03   实时协议日志",26,533,300); button(L"保存日志",666,525,100,SAVE_LOG); button(L"清空日志",780,525,100,CLEAR);
    logBox=control(L"EDIT",L"[系统] 请选择文件并设置网络参数。\r\n",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,26,565,854,162,0,WS_EX_CLIENTEDGE);
    SendMessageW(logBox,WM_SETFONT,(WPARAM)logFont,TRUE);
    SendMessageW(logBox,EM_SETLIMITTEXT,500000,0);
    SetTimer(mainWnd,1,25,nullptr);
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: mainWnd=h; setup(); return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case OPEN_FILE: if(!running) choose(false); break;
        case SAVE_FILE: if(!running) choose(true); break;
        case START: start(); break;
        case STOP: if(running) finish(L"已停止 · 未完成文件不会替换目标文件"); break;
        case CLEAR: set(logBox,L""); break;
        case SAVE_LOG: saveLog(); break;
        }
        return 0;
    case WM_TIMER: poll(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT top{0,0,906,88}; FillRect(dc,&top,headerBrush);
        for(int i=0;i<4;++i) { RECT card{26+i*215,436,230+i*215,515}; FillRect(dc,&card,whiteBrush); }
        EndPaint(h,&ps); return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp; HWND c=(HWND)lp; RECT r{}; GetWindowRect(c,&r); MapWindowPoints(nullptr,h,(POINT*)&r,2);
        if(r.top<88) { SetTextColor(dc,RGB(255,255,255)); SetBkColor(dc,navy); return (LRESULT)headerBrush; }
        if(c==logBox) { SetTextColor(dc,navy); SetBkColor(dc,RGB(255,255,255)); return (LRESULT)whiteBrush; }
        SetTextColor(dc,(c==statusBox||c==speedBox||c==timeBox||c==retryBox||c==rttBox)?blue:navy);
        if(r.top>=436&&r.top<515) { SetBkColor(dc,RGB(255,255,255)); return (LRESULT)whiteBrush; }
        SetBkColor(dc,RGB(239,244,249)); return (LRESULT)background;
    }
    case WM_CLOSE: if(running) finish(L"已停止"); DestroyWindow(h); return 0;
    case WM_DESTROY:
        KillTimer(h,1); for(auto& c:children) closeChild(c);
        DeleteObject(normalFont); DeleteObject(titleFont); DeleteObject(valueFont); DeleteObject(logFont);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,LPWSTR,int show) {
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_PROGRESS_CLASS}; InitCommonControlsEx(&ic);
    background=CreateSolidBrush(RGB(239,244,249)); headerBrush=CreateSolidBrush(navy); whiteBrush=CreateSolidBrush(RGB(255,255,255));
    WNDCLASSW wc{}; wc.lpfnWndProc=proc; wc.hInstance=inst; wc.lpszClassName=L"ReliableUDP"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=background;
    RegisterClassW(&wc);
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT r{0,0,906,749}; AdjustWindowRect(&r,style,FALSE);
    HWND h=CreateWindowW(wc.lpszClassName,L"计算机网络实验三 · 可靠文件传输",style,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,inst,nullptr);
    if(!h) return 1;
    ShowWindow(h,show); UpdateWindow(h);
    MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0) { if(!IsDialogMessageW(h,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    UnregisterClassW(wc.lpszClassName,inst); DeleteObject(background); DeleteObject(headerBrush); DeleteObject(whiteBrush);
    return 0;
}
