#include "Renderer.h"
#include <WebView2.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <dwmapi.h>
#include <fstream>
#include <sstream>
#include <memory>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using namespace winrt::Windows::Data::Json;
namespace fs = std::filesystem;
constexpr wchar_t LocalPage[]=L"https://tasks.local/index.html";
constexpr UINT TrayMessage=WM_APP+1;
constexpr UINT QuitMessage=WM_APP+2;
constexpr UINT FrameMessage=WM_APP+3;
static std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if (!count) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(count,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),result.data(),count); return result;
}
static std::string utf8(std::wstring_view s) { return winrt::to_string(winrt::hstring(s)); }
static std::string readFile(const fs::path& path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + utf8(path.wstring()));
    std::string content((std::istreambuf_iterator<char>(stream)),{});
    if (stream.bad()) throw std::runtime_error("Read failed " + utf8(path.wstring()));
    if (content.starts_with("\xEF\xBB\xBF")) content.erase(0,3);
    return content;
}
static void writeFile(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    fs::path temp=path; temp+=L".tmp";
    { std::ofstream stream(temp,std::ios::binary|std::ios::trunc); stream.write(content.data(),content.size()); if(!stream) throw std::runtime_error("Cannot write report"); }
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) Check(HRESULT_FROM_WIN32(GetLastError()),"Save report");
}
static JsonObject readJson(const fs::path& path) { return JsonObject::Parse(wide(readFile(path))); }
static void put(JsonObject& o, const wchar_t* key, std::wstring_view value) { o.Insert(key,JsonValue::CreateStringValue(value)); }
static void number(JsonObject& o, const wchar_t* key, double value) { o.Insert(key,JsonValue::CreateNumberValue(value)); }
static void putBool(JsonObject& o, const wchar_t* key, bool value) { o.Insert(key,JsonValue::CreateBooleanValue(value)); }
static std::wstring quote(const std::wstring& s) {
    std::wstring out=L"\""; size_t slash=0;
    for(wchar_t c:s) { if(c==L'\\') {slash++;continue;} if(c==L'\"') out.append(slash*2+1,L'\\'); else out.append(slash,L'\\'); slash=0; out+=c; }
    out.append(slash*2,L'\\'); return out+L'\"';
}
static std::wstring hex(std::string_view value) {
    constexpr wchar_t digits[]=L"0123456789abcdef"; std::wstring result; result.reserve(value.size()*2);
    for(unsigned char c:value) { result+=digits[c>>4];result+=digits[c&15]; } return result;
}
static fs::path executableDirectory() {
    std::wstring buffer(32768,L'\0'); DWORD length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!length || length>=buffer.size()) throw std::runtime_error("Executable path unavailable");
    buffer.resize(length); return fs::path(buffer).parent_path();
}
static fs::path localData() {
    PWSTR path=nullptr; Check(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path),"Local app data");
    fs::path result=fs::path(path)/L"ObsidianGlassTasks"; CoTaskMemFree(path); fs::create_directories(result); return result;
}
struct App {
    HWND hwnd{};
    HANDLE mutex{}, actionProcess{}, stopEvent{}, frameTimer{}, frameWait{};
    std::unique_ptr<Renderer> renderer;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2CompositionController> compositionController;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    fs::path appDir=executableDirectory(), resources=appDir/L"resources", stateDir=localData(), reportPath=stateDir/L"diagnostics.json";
    JsonObject config, uiReport;
    fs::path sourcePath, powershell;
    std::wstring lastSource, lastSourceError, captureError, fatal, actionId;
    bool webReady=false, graphicsReady=false, fixtureTest=false, selfTest=false, sourceKnown=false;
    bool dirty=true, tracking=false, topmost=false, screenshotMode=false, reduceMotion=false;
    bool exclusion=false, closeRequested=false;
    int exitCode=0, captureAttempts=0;
    float pressure=0, velocity=0, pressureTarget=0;
    ULONGLONG lastSourceRead=0,lastReport=0,nextCaptureAttempt=0,started=GetTickCount64();
    double nextFrameDueMs=0,lastAnimationMs=0,lastFrameWakeMs=0;
    TimingSamples frameWakeIntervalMs;
    uint64_t frameWakeCount=0,moveRefreshCount=0;
    HMONITOR currentMonitor{};
    NOTIFYICONDATAW tray{};
    HICON icon{};
    ~App() {
        stopFrameClock();
        if(controller) controller->Close();
        if(tray.hWnd) Shell_NotifyIconW(NIM_DELETE,&tray);
        if(icon) DestroyIcon(icon);
        if(actionProcess) CloseHandle(actionProcess);
        if(mutex) CloseHandle(mutex);
        if(stopEvent) CloseHandle(stopEvent);
    }
    void stopFrameClock() {
        if(frameTimer)CancelWaitableTimer(frameTimer);
        if(frameWait){UnregisterWaitEx(frameWait,INVALID_HANDLE_VALUE);frameWait=nullptr;}
        if(frameTimer){CloseHandle(frameTimer);frameTimer=nullptr;}
    }
    void armFrameClock() {
        double now=ClockMilliseconds(),period=1000.0/renderer->refreshRateHz;
        nextFrameDueMs+=period;
        if(nextFrameDueMs<=now)nextFrameDueMs+=std::floor((now-nextFrameDueMs)/period+1)*period;
        LARGE_INTEGER due{};due.QuadPart=-std::max<int64_t>(1,static_cast<int64_t>((nextFrameDueMs-now)*10000));
        if(!SetWaitableTimerEx(frameTimer,&due,0,nullptr,nullptr,nullptr,0))Check(HRESULT_FROM_WIN32(GetLastError()),"Schedule glass frame");
    }
    void startFrameClock() {
        // A high-resolution clock posts at most one frame message. Unlike WM_TIMER,
        // this also reaches Windows' native move/menu loop at high refresh rates.
        frameTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(!frameTimer)Check(HRESULT_FROM_WIN32(GetLastError()),"Create glass frame clock");
        if(!RegisterWaitForSingleObject(&frameWait,frameTimer,[](PVOID context,BOOLEAN){
            auto app=static_cast<App*>(context);PostMessageW(app->hwnd,FrameMessage,0,0);
        },this,INFINITE,WT_EXECUTEINWAITTHREAD))Check(HRESULT_FROM_WIN32(GetLastError()),"Register glass frame clock");
        nextFrameDueMs=lastAnimationMs=ClockMilliseconds();armFrameClock();
    }
    void send(JsonObject value) { if(web && webReady) Check(web->PostWebMessageAsJson(value.Stringify().c_str()),"Send page message"); }
    void theme() { JsonObject m; put(m,L"type",L"material"); putBool(m,L"paused",screenshotMode); put(m,L"error",captureError); send(m); }
    void readSource(bool force=false) {
        if(!webReady) return;
        try {
            std::wstring text=wide(readFile(sourcePath));
            if(force || !sourceKnown || text!=lastSource || !lastSourceError.empty()) {
                JsonObject m; put(m,L"type",L"source"); put(m,L"text",text); send(m);
                lastSource=std::move(text); lastSourceError.clear();sourceKnown=true;
            }
        } catch(const std::exception& error) {
            std::wstring detail=wide(error.what());
            if(force || detail!=lastSourceError) { JsonObject m;put(m,L"type",L"source-error");put(m,L"message",L"无法读取收件箱，请检查笔记位置或稍后刷新");send(m); }
            lastSourceError=std::move(detail);
        }
    }
    void actionResult(JsonObject result) { JsonObject m; put(m,L"type",L"action-result");m.Insert(L"result",result);send(m); }
    void actionFailure(std::wstring_view id,std::wstring_view message) { JsonObject r;put(r,L"id",id);putBool(r,L"ok",false);put(r,L"message",message);actionResult(r); }
    void action(JsonObject request) {
        auto id=request.GetNamedString(L"id",L"");
        if(id.size()!=36 || std::any_of(id.begin(),id.end(),[](wchar_t c){return !(c==L'-'||(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f'));})) return;
        if(actionProcess) {actionFailure(id,L"上一项操作仍在处理中，请稍候");return;}
        auto kind=request.GetNamedString(L"kind",L"");
        if(kind!=L"toggle" && kind!=L"open" && kind!=L"probe") {actionFailure(id,L"不支持的操作");return;}
        auto payload=hex(winrt::to_string(request.Stringify()));
        if(payload.size()>28000) {actionFailure(id,L"这项任务过长，请在 Obsidian 中操作");return;}
        std::wstring command=quote(powershell.wstring())+L" -NoProfile -NonInteractive -File "+quote((resources/L"Action.ps1").wstring())+L" -RequestHex "+payload;
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,resources.c_str(),&si,&pi)) {
            actionFailure(id,L"无法启动笔记保存程序，请检查 PowerShell 路径");return;
        }
        actionId=id; actionProcess=pi.hProcess;CloseHandle(pi.hThread);
    }
    void pollAction() {
        if(!actionProcess || WaitForSingleObject(actionProcess,0)!=WAIT_OBJECT_0) return;
        DWORD code=0;GetExitCodeProcess(actionProcess,&code);CloseHandle(actionProcess);actionProcess=nullptr;
        try {
            if(code) throw std::runtime_error("Action process failed");
            auto result=readJson(resources/L"ui"/L"action-result.json");
            if(result.GetNamedString(L"id",L"")!=actionId) throw std::runtime_error("Action result id mismatch");
            actionResult(result);
        } catch(const std::exception&) {actionFailure(actionId,L"未收到有效保存结果，请核对笔记后再操作");}
        actionId.clear();readSource(true);
    }
    void savePosition() {
        if(fixtureTest||selfTest) return;
        RECT r{};GetWindowRect(hwnd,&r);JsonObject state;number(state,L"x",r.left);number(state,L"y",r.top);putBool(state,L"topmost",topmost);
        writeFile(stateDir/L"window.json",winrt::to_string(state.Stringify()));
    }
    void resizeContent(double cssHeight) {
        if(!std::isfinite(cssHeight)) return;
        auto dpi=GetDpiForWindow(hwnd)/96.f; MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);
        RECT r{};GetClientRect(hwnd,&r);
        int h=std::clamp(static_cast<int>(std::round(cssHeight*dpi)),static_cast<int>(280*dpi),static_cast<int>((mi.rcWork.bottom-mi.rcWork.top)*.87));
        if(abs(r.bottom-h)>2) SetWindowPos(hwnd,nullptr,0,0,r.right,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void report() {
        JsonObject r;putBool(r,L"ok",fatal.empty()&&webReady&&sourceKnown&&lastSourceError.empty());number(r,L"pid",GetCurrentProcessId());
        number(r,L"uptimeSeconds",(GetTickCount64()-started)/1000.0);put(r,L"version",L"0.1.5");
        put(r,L"adapter",renderer?renderer->adapterName:L""); putBool(r,L"desktopCaptureReady",renderer&&renderer->ready());
        putBool(r,L"excludedFromCapture",exclusion);putBool(r,L"screenshotMode",screenshotMode);putBool(r,L"topmost",topmost);
        putBool(r,L"saving",actionProcess!=nullptr);
        number(r,L"capturedFrames",renderer?static_cast<double>(renderer->captured):0);number(r,L"renderedFrames",renderer?static_cast<double>(renderer->rendered):0);
        number(r,L"unchangedFramesSkipped",renderer?static_cast<double>(renderer->unchanged):0);
        JsonObject timing;
        number(timing,L"targetHz",renderer?renderer->refreshRateHz:0);
        number(timing,L"frameWakeCount",static_cast<double>(frameWakeCount));
        number(timing,L"moveRefreshCount",static_cast<double>(moveRefreshCount));
        auto metric=[&](const wchar_t* name,const TimingSamples& values){JsonObject m;number(m,L"meanMs",values.mean());number(m,L"p95Ms",values.p95());number(m,L"samples",static_cast<double>(values.values.size()));timing.Insert(name,m);};
        metric(L"frameWakeInterval",frameWakeIntervalMs);
        if(renderer){
            metric(L"captureWork",renderer->captureWorkMs);metric(L"gpuCompareWait",renderer->compareWaitMs);
            metric(L"presentCall",renderer->presentCallMs);metric(L"desktopFrameToSubmit",renderer->sourceToSubmitMs);
            number(timing,L"presentBackpressure",static_cast<double>(renderer->presentBackpressure));
            number(timing,L"accumulatedDesktopFrames",static_cast<double>(renderer->accumulatedDesktopFrames));
        }
        r.Insert(L"timing",timing);
        putBool(r,L"darkMaterial",true);
        put(r,L"captureError",captureError);put(r,L"sourceError",lastSourceError);put(r,L"fatal",fatal);r.Insert(L"ui",uiReport);
        writeFile(reportPath,winrt::to_string(r.Stringify()));
    }
    void fail(std::wstring message) {
        fatal=std::move(message);exitCode=1;
        try {report();} catch(...) {}
        if(!fixtureTest && !selfTest) MessageBoxW(hwnd,fatal.c_str(),L"桌面待办未能启动",MB_OK|MB_ICONERROR);
        PostMessageW(hwnd,QuitMessage,1,0);
    }
    void requestCaptureRestart() { if(!renderer||screenshotMode||selfTest||fixtureTest)return;renderer->resetCapture();captureAttempts=0;nextCaptureAttempt=GetTickCount64()+150;dirty=true; }
    void updateFrame() {
        auto now=GetTickCount64();
        if(graphicsReady && !fixtureTest && !selfTest && !IsIconic(hwnd)) {
            if(!screenshotMode && exclusion) {
                if(nextCaptureAttempt && now>=nextCaptureAttempt) {
                    captureAttempts++;nextCaptureAttempt=0;
                    try {renderer->initializeCapture();captureError.clear();theme();}
                    catch(const std::exception& e) {captureError=wide(e.what());if(captureAttempts<3)nextCaptureAttempt=now+1000;theme();}
                    dirty=true;
                }
                if(!nextCaptureAttempt && captureError.empty()) {
                    try {dirty=renderer->capture()||dirty;}
                    catch(const std::exception& e) {renderer->resetCapture();captureError=wide(e.what());nextCaptureAttempt=captureAttempts<3?now+500:0;dirty=true;theme();}
                }
            }
            double animationNow=ClockMilliseconds();
            float dt=static_cast<float>(std::min((animationNow-lastAnimationMs)/1000.0,.05));lastAnimationMs=animationNow;
            if(reduceMotion) {pressure=pressureTarget=velocity=0;}
            else if(abs(pressure-pressureTarget)>.001f || abs(velocity)>.001f) {
                velocity+=(pressureTarget-pressure)*140*dt;velocity*=std::exp(-18*dt);pressure+=velocity*dt;dirty=true;
                if(abs(pressure-pressureTarget)<.001f&&abs(velocity)<.001f)pressure=pressureTarget;
            }
            if(dirty)dirty=!renderer->render(pressure);
        }
    }
    void tick() {
        auto now=GetTickCount64();pollAction();
        if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)closeRequested=true;
        if(closeRequested&&!actionProcess){PostMessageW(hwnd,WM_CLOSE,0,0);closeRequested=false;}
        if(webReady && now-lastSourceRead>=2000){lastSourceRead=now;readSource();}
        if(now-lastReport>=3000) {
            lastReport=now;
            if(webReady){JsonObject request;put(request,L"type",L"diagnose");send(request);}
            report();
        }
        if(fixtureTest && now-started>45000) fail(L"Fixture bridge test timed out");
    }
    void menu() {
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING|(topmost?MF_CHECKED:0),1,L"保持置顶");
        AppendMenuW(menu,MF_STRING|(screenshotMode?MF_CHECKED:0),2,L"暂停动态玻璃（便于截图）");
        AppendMenuW(menu,MF_STRING,3,L"刷新待办");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(actionProcess?MF_GRAYED:0),4,L"切回 Rainmeter 版");
        AppendMenuW(menu,MF_STRING|(actionProcess?MF_GRAYED:0),5,L"退出");
        POINT p{};GetCursorPos(&p);SetForegroundWindow(hwnd);
        UINT command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,hwnd,nullptr);DestroyMenu(menu);PostMessageW(hwnd,WM_NULL,0,0);
        if(command==1) {topmost=!topmost;SetWindowPos(hwnd,topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);savePosition();}
        if(command==2) {
            screenshotMode=!screenshotMode;
            if(!SetWindowDisplayAffinity(hwnd,screenshotMode?WDA_NONE:WDA_EXCLUDEFROMCAPTURE)) {screenshotMode=true;captureError=L"无法排除自身画面，动态玻璃已暂停";}
            exclusion=!screenshotMode;DwmFlush();
            if(!screenshotMode)requestCaptureRestart();theme();dirty=true;
        }
        if(command==3){readSource(true);if(!screenshotMode)requestCaptureRestart();}
        if(command==4){
            SHELLEXECUTEINFOW execute{sizeof(execute)};execute.fMask=SEE_MASK_NOCLOSEPROCESS;execute.hwnd=hwnd;execute.lpFile=powershell.c_str();
            std::wstring args=L"-NoProfile -NonInteractive -File "+quote((appDir/L"Restore-Rainmeter.ps1").wstring())+L" -SkipStop";
            execute.lpParameters=args.c_str();execute.nShow=SW_HIDE;
            if(ShellExecuteExW(&execute)){if(execute.hProcess)CloseHandle(execute.hProcess);PostMessageW(hwnd,WM_CLOSE,0,0);}
        }
        if(command==5)PostMessageW(hwnd,WM_CLOSE,0,0);
    }
    void onMessage(JsonObject message) {
        auto type=message.GetNamedString(L"type",L"");
        if(type==L"ready") {
            webReady=true;
            JsonObject m;put(m,L"type",L"init");put(m,L"vault",config.GetNamedString(L"vault"));put(m,L"noteFile",config.GetNamedString(L"noteFile"));
            putBool(m,L"fixtureTest",fixtureTest);send(m);theme();readSource(true);
        } else if(type==L"action") action(message.GetNamedObject(L"request"));
        else if(type==L"refresh")readSource(true);
        else if(type==L"resize")resizeContent(message.GetNamedNumber(L"height",640));
        else if(type==L"drag") {if(!fixtureTest){ReleaseCapture();pressureTarget=0;SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);}}
        else if(type==L"menu") {if(!fixtureTest)menu();}
        else if(type==L"preferences")reduceMotion=message.GetNamedBoolean(L"reduceMotion",false);
        else if(type==L"diagnostics") {
            uiReport=message.GetNamedObject(L"report");report();
            if(fixtureTest && uiReport.HasKey(L"fixtureResult")) {
                exitCode=uiReport.GetNamedObject(L"fixtureResult").GetNamedBoolean(L"ok",false)?0:1;
                PostMessageW(hwnd,QuitMessage,exitCode,0);
            }
        }
    }
    void createWebView() {
        fs::path profile=stateDir/(fixtureTest?L"FixtureWebView2":L"WebView2");
        Check(CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT hr,ICoreWebView2Environment* env)->HRESULT {
                try {
                    Check(hr,"WebView2 environment");environment=env;ComPtr<ICoreWebView2Environment3> env3;Check(environment.As(&env3),"Composition environment");
                    Check(env3->CreateCoreWebView2CompositionController(hwnd,Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [this](HRESULT result,ICoreWebView2CompositionController* cc)->HRESULT {
                            try {Check(result,"WebView2 composition controller");compositionController=cc;setupWebView();}
                            catch(const std::exception& e){fail(wide(e.what()));}catch(const winrt::hresult_error& e){fail(e.message().c_str());}
                            return S_OK;
                        }).Get()),"Create WebView2 controller");
                }catch(const std::exception& e){fail(wide(e.what()));}catch(const winrt::hresult_error& e){fail(e.message().c_str());}
                return S_OK;
            }).Get()),"Create WebView2 environment");
    }
    void setupWebView() {
        Check(compositionController.As(&controller),"WebView controller");Check(controller->get_CoreWebView2(&web),"WebView content");
        Check(compositionController->put_RootVisualTarget(renderer->webRoot()),"WebView visual target");
        ComPtr<ICoreWebView2Controller2> controller2;Check(controller.As(&controller2),"Transparent controller");
        Check(controller2->put_DefaultBackgroundColor({0,0,0,0}),"Transparent web background");
        RECT bounds{};GetClientRect(hwnd,&bounds);Check(controller->put_Bounds(bounds),"WebView bounds");Check(controller->put_ZoomFactor(1),"WebView scale");
        Check(controller->put_IsVisible(TRUE),"WebView visible");
        ComPtr<ICoreWebView2Settings> settings;Check(web->get_Settings(&settings),"WebView settings");
        settings->put_AreDevToolsEnabled(FALSE);settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);settings->put_AreDefaultScriptDialogsEnabled(FALSE);settings->put_IsWebMessageEnabled(TRUE);
        ComPtr<ICoreWebView2_3> web3;Check(web.As(&web3),"Local virtual host");
        Check(web3->SetVirtualHostNameToFolderMapping(L"tasks.local",(resources/L"ui").c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS),"Map local page");
        EventRegistrationToken token{};
        Check(web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT {
            LPWSTR uri=nullptr;args->get_Uri(&uri);args->put_Cancel(!uri || wcscmp(uri,LocalPage)!=0);CoTaskMemFree(uri);return S_OK;
        }).Get(),&token),"Navigation policy");
        Check(web->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT {args->put_Cancel(TRUE);return S_OK;}).Get(),&token),"Frame policy");
        Check(web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT{args->put_Handled(TRUE);return S_OK;}).Get(),&token),"New window policy");
        Check(web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* args)->HRESULT{args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);return S_OK;}).Get(),&token),"Permission policy");
        Check(web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
            LPWSTR origin=nullptr,json=nullptr;args->get_Source(&origin);
            bool trusted=origin && wcscmp(origin,LocalPage)==0;CoTaskMemFree(origin);if(!trusted)return S_OK;
            if(SUCCEEDED(args->get_WebMessageAsJson(&json))&&json) {
                try{if(wcslen(json)<=65536)onMessage(JsonObject::Parse(json));}
                catch(const std::exception& e){fatal=wide(e.what());report();}
                catch(const winrt::hresult_error& e){fatal=e.message().c_str();report();}
                CoTaskMemFree(json);
            }
            return S_OK;
        }).Get(),&token),"Native bridge");
        Check(web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([this](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT {
            BOOL ok=FALSE;args->get_IsSuccess(&ok);if(!ok)fail(L"本地待办页面加载失败");return S_OK;
        }).Get(),&token),"Page load status");
        Check(web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([this](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT {fail(L"待办页面进程已停止，请重新打开桌面待办");return S_OK;}).Get(),&token),"WebView process status");
        Check(compositionController->add_CursorChanged(Callback<ICoreWebView2CursorChangedEventHandler>([this](ICoreWebView2CompositionController*,IUnknown*)->HRESULT {
            HCURSOR cursor=nullptr;if(SUCCEEDED(compositionController->get_Cursor(&cursor)))SetCursor(cursor);return S_OK;
        }).Get(),&token),"Web cursor");
        Check(renderer->compositionDevice()->Commit(),"Commit WebView");Check(web->Navigate(LocalPage),"Load local page");
    }
    void mouse(UINT message,WPARAM wParam,LPARAM lParam) {
        if(!compositionController)return;
        if(message==WM_MOUSEMOVE&&!tracking){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);tracking=true;}
        if(message==WM_MOUSELEAVE){tracking=false;renderer->pointer(-1000,-1000);dirty=true;}
        POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};UINT data=0;
        if(message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL){ScreenToClient(hwnd,&p);data=static_cast<UINT>(GET_WHEEL_DELTA_WPARAM(wParam));}
        if(message==WM_XBUTTONDOWN||message==WM_XBUTTONUP||message==WM_XBUTTONDBLCLK)data=GET_XBUTTON_WPARAM(wParam);
        if(message==WM_MOUSEMOVE){renderer->pointer(static_cast<float>(p.x),static_cast<float>(p.y));dirty=true;}
        if(message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN){SetCapture(hwnd);controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);pressureTarget=1;}
        if(message==WM_LBUTTONUP||message==WM_RBUTTONUP||message==WM_MBUTTONUP){ReleaseCapture();pressureTarget=0;}
        auto keys=static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GET_KEYSTATE_WPARAM(wParam));
        if(message==WM_MOUSELEAVE){p={};keys=COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;}
        Check(compositionController->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(message),keys,data,p),"Forward pointer");
    }
    void createTray() {
        // A small application-owned check icon. No external artwork.
        constexpr int size=32;std::vector<unsigned char> pixels(size*size*4);
        for(int y=0;y<size;y++)for(int x=0;x<size;x++) {
            float dx=x-15.5f,dy=y-15.5f;if(dx*dx+dy*dy>14*14)continue;
            auto i=(y*size+x)*4;pixels[i]=171;pixels[i+1]=116;pixels[i+2]=35;pixels[i+3]=255;
            auto lineDistance=[&](float ax,float ay,float bx,float by){float vx=bx-ax,vy=by-ay;float t=std::clamp(((x-ax)*vx+(y-ay)*vy)/(vx*vx+vy*vy),0.f,1.f);return std::hypot(x-ax-vx*t,y-ay-vy*t);};
            if(std::min(lineDistance(8,16,13,21),lineDistance(13,21,24,10))<1.4f)pixels[i]=pixels[i+1]=pixels[i+2]=255;
        }
        BITMAPV5HEADER header{};header.bV5Size=sizeof(header);header.bV5Width=size;header.bV5Height=-size;header.bV5Planes=1;header.bV5BitCount=32;header.bV5Compression=BI_BITFIELDS;
        header.bV5RedMask=0x00ff0000;header.bV5GreenMask=0x0000ff00;header.bV5BlueMask=0x000000ff;header.bV5AlphaMask=0xff000000;
        void* bits=nullptr;HDC dc=GetDC(nullptr);HBITMAP color=CreateDIBSection(dc,reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS,&bits,nullptr,0);ReleaseDC(nullptr,dc);
        if(!color)throw std::runtime_error("Tray bitmap failed");memcpy(bits,pixels.data(),pixels.size());HBITMAP mask=CreateBitmap(size,size,1,1,nullptr);
        ICONINFO info{};info.fIcon=TRUE;info.hbmColor=color;info.hbmMask=mask;icon=CreateIconIndirect(&info);DeleteObject(color);DeleteObject(mask);
        tray.cbSize=sizeof(tray);tray.hWnd=hwnd;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=TrayMessage;tray.hIcon=icon;
        wcscpy_s(tray.szTip,L"Obsidian 桌面待办");Shell_NotifyIconW(NIM_ADD,&tray);tray.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray);
    }
    LRESULT handle(UINT message,WPARAM wParam,LPARAM lParam) {
        switch(message) {
        case FrameMessage:{
            double now=ClockMilliseconds();if(lastFrameWakeMs>0)frameWakeIntervalMs.add(now-lastFrameWakeMs);lastFrameWakeMs=now;frameWakeCount++;
            updateFrame();armFrameClock();return 0;
        }
        case WM_TIMER: tick();return 0;
        case WM_SIZE:
            if(graphicsReady && wParam!=SIZE_MINIMIZED){RECT r{};GetClientRect(hwnd,&r);renderer->resize(r.right,r.bottom,GetDpiForWindow(hwnd)/96.f);if(controller)controller->put_Bounds(r);dirty=true;}
            return 0;
        case WM_MOVE:
            if(graphicsReady){renderer->move();dirty=true;auto monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);if(monitor!=currentMonitor){currentMonitor=monitor;requestCaptureRestart();}moveRefreshCount++;updateFrame();}
            if(controller)controller->NotifyParentWindowPositionChanged();return 0;
        case WM_DPICHANGED: {auto r=reinterpret_cast<RECT*>(lParam);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
        case WM_DISPLAYCHANGE:requestCaptureRestart();return 0;
        case WM_POWERBROADCAST:if(wParam==PBT_APMRESUMEAUTOMATIC||wParam==PBT_APMRESUMESUSPEND)requestCaptureRestart();return TRUE;
        case WM_WTSSESSION_CHANGE:if(wParam==WTS_SESSION_UNLOCK)requestCaptureRestart();return 0;
        case WM_EXITSIZEMOVE:savePosition();pressureTarget=0;return 0;
        case WM_SETFOCUS:if(controller)controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);return 0;
        case WM_CAPTURECHANGED:pressureTarget=0;return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);return 0;}
        case WM_SETCURSOR:if(compositionController&&LOWORD(lParam)==HTCLIENT){HCURSOR cursor=nullptr;compositionController->get_Cursor(&cursor);SetCursor(cursor);return TRUE;}break;
        case WM_MOUSEMOVE:case WM_MOUSELEAVE:case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_RBUTTONDBLCLK:case WM_MBUTTONDOWN:case WM_MBUTTONUP:case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:case WM_XBUTTONUP:case WM_XBUTTONDBLCLK:case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:mouse(message,wParam,lParam);return 0;
        case WM_CONTEXTMENU:if(lParam==-1)menu();return 0;
        case TrayMessage:if(LOWORD(lParam)==WM_CONTEXTMENU)menu();else if(LOWORD(lParam)==NIN_SELECT){ShowWindow(hwnd,SW_SHOWNORMAL);SetForegroundWindow(hwnd);}return 0;
        case WM_QUERYENDSESSION:savePosition();return TRUE;
        case WM_CLOSE:
            if(actionProcess){JsonObject m;put(m,L"type",L"notice");put(m,L"message",L"正在保存，请保存完成后再退出");send(m);return 0;}
            savePosition();DestroyWindow(hwnd);return 0;
        case QuitMessage:exitCode=static_cast<int>(wParam);DestroyWindow(hwnd);return 0;
        case WM_DESTROY:stopFrameClock();KillTimer(hwnd,1);WTSUnRegisterSessionNotification(hwnd);PostQuitMessage(exitCode);return 0;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }
};
static LRESULT CALLBACK WindowProcedure(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    App* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);app->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(app)try{return app->handle(message,wParam,lParam);}catch(const std::exception& e){app->fail(wide(e.what()));return 0;}catch(const winrt::hresult_error& e){app->fail(e.message().c_str());return 0;}
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::unique_ptr<App> app;
    try {
        app=std::make_unique<App>();int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        for(int i=1;i<argc;i++) {
            std::wstring arg=argv[i];
            if(arg==L"--stop") {
                HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,L"Local\\ObsidianGlassTasks.Stop.v1");
                if(event){SetEvent(event);CloseHandle(event);}LocalFree(argv);return 0;
            }
            else if(arg==L"--self-test")app->selfTest=true;
            else if(arg==L"--fixture-test")app->fixtureTest=true;
            else if(arg==L"--data-dir" && i+1<argc)app->resources=fs::absolute(argv[++i]);
            else if(arg==L"--diagnostics" && i+1<argc)app->reportPath=fs::absolute(argv[++i]);
            else throw std::runtime_error("Unknown command-line argument");
        }
        LocalFree(argv);
        if(!app->selfTest) {
            app->config=readJson(app->resources/L"config.json");
            app->sourcePath=fs::path(app->config.GetNamedString(L"sourcePath").c_str());
            app->powershell=fs::path(app->config.GetNamedString(L"powershellPath").c_str());
            if(!app->sourcePath.is_absolute()||!app->powershell.is_absolute()||!fs::is_regular_file(app->powershell))throw std::runtime_error("Configuration requires absolute source and PowerShell paths");
            if(app->fixtureTest) {
                if(!app->config.GetNamedBoolean(L"testFixture",false))throw std::runtime_error("Fixture flag is required");
                auto relative=fs::canonical(app->sourcePath).lexically_relative(fs::canonical(app->resources));
                if(relative.empty()||*relative.begin()==L".."||relative.is_absolute())throw std::runtime_error("Fixture source must be inside its isolated data directory");
            }
        }
        if(!app->fixtureTest&&!app->selfTest) {
            app->mutex=CreateMutexW(nullptr,FALSE,L"Local\\ObsidianGlassTasks.Native.v1");
            if(!app->mutex)Check(HRESULT_FROM_WIN32(GetLastError()),"Single instance mutex");
            if(GetLastError()==ERROR_ALREADY_EXISTS)return 0;
            app->stopEvent=CreateEventW(nullptr,FALSE,FALSE,L"Local\\ObsidianGlassTasks.Stop.v1");
            if(!app->stopEvent)Check(HRESULT_FROM_WIN32(GetLastError()),"Stop event");
        }
        WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_DBLCLKS;wc.hInstance=instance;wc.lpfnWndProc=WindowProcedure;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"ObsidianGlassTasks.Native";
        if(!RegisterClassExW(&wc))Check(HRESULT_FROM_WIN32(GetLastError()),"Window class");
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);
        int width=MulDiv(360,GetDpiForSystem(),96),height=MulDiv(640,GetDpiForSystem(),96);
        int x=mi.rcWork.right-width,y=mi.rcWork.top;
        auto positionFile=app->stateDir/L"window.json";
        if(!app->fixtureTest&&!app->selfTest&&fs::exists(positionFile)) {
            auto state=readJson(positionFile);x=static_cast<int>(state.GetNamedNumber(L"x",x));y=static_cast<int>(state.GetNamedNumber(L"y",y));app->topmost=state.GetNamedBoolean(L"topmost",false);
            RECT wanted{x,y,x+width,y+height};GetMonitorInfoW(MonitorFromRect(&wanted,MONITOR_DEFAULTTONEAREST),&mi);
            x=std::clamp(x,int(mi.rcWork.left),int(mi.rcWork.right-width));y=std::clamp(y,int(mi.rcWork.top),int(mi.rcWork.bottom-180));
        }
        HWND hwnd=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW,wc.lpszClassName,L"Obsidian 桌面待办",WS_POPUP,x,y,width,height,nullptr,nullptr,instance,app.get());
        if(!hwnd)Check(HRESULT_FROM_WIN32(GetLastError()),"Create task window");
        app->renderer=std::make_unique<Renderer>();app->renderer->initialize(hwnd,app->appDir/L"resources"/L"Glass.hlsl");app->graphicsReady=true;
        if(app->selfTest){writeFile(app->reportPath,app->renderer->selfTest());DestroyWindow(hwnd);return 0;}
        if(!app->fixtureTest) {
            if(!SetWindowDisplayAffinity(hwnd,WDA_EXCLUDEFROMCAPTURE))throw std::runtime_error("Windows cannot exclude this window from capture; refusing recursive capture");
            DWORD affinity=0;app->exclusion=GetWindowDisplayAffinity(hwnd,&affinity)&&affinity==WDA_EXCLUDEFROMCAPTURE;
            if(!app->exclusion)throw std::runtime_error("Capture exclusion verification failed");
            app->currentMonitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);app->requestCaptureRestart();app->createTray();
            WTSRegisterSessionNotification(hwnd,NOTIFY_FOR_THIS_SESSION);
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);SetWindowPos(hwnd,app->topmost?HWND_TOPMOST:HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
        app->renderer->render();app->createWebView();app->startFrameClock();SetTimer(hwnd,1,50,nullptr);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}app->report();return app->exitCode;
    } catch(const std::exception& e) {
        if(app){app->fatal=wide(e.what());try{app->report();}catch(...){}if(!app->fixtureTest&&!app->selfTest)MessageBoxW(nullptr,app->fatal.c_str(),L"桌面待办未能启动",MB_OK|MB_ICONERROR);}
        return 1;
    } catch(const winrt::hresult_error& e) {
        if(app){app->fatal=e.message().c_str();try{app->report();}catch(...){}if(!app->fixtureTest&&!app->selfTest)MessageBoxW(nullptr,app->fatal.c_str(),L"桌面待办未能启动",MB_OK|MB_ICONERROR);}
        return 1;
    }
}
