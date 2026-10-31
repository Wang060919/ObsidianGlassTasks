#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>

void Check(HRESULT hr, const char* operation);
double CounterMilliseconds(int64_t ticks);
double ClockMilliseconds();
struct TimingSamples {
    std::vector<double> values;
    size_t next=0;
    void add(double ms) {
        if(values.size()<512) values.push_back(ms);
        else {values[next]=ms;next=(next+1)%values.size();}
    }
    double mean() const {return values.empty()?0:std::accumulate(values.begin(),values.end(),0.0)/values.size();}
    double p95() const {
        if(values.empty())return 0;
        auto sorted=values;std::sort(sorted.begin(),sorted.end());return sorted[(sorted.size()-1)*95/100];
    }
};
class Renderer {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct alignas(16) Parameters {
        float viewport[2]{360, 640}, origin[2]{};
        float desktopOrigin[2]{}, desktopSize[2]{1920, 1080};
        float pointer[2]{-1000,-1000}, unused0[2]{};
        float dpi=1, press=0, unusedMaterial=0, refraction=1;
        float rotation=0, hasBackdrop=0, unused1[2]{};
    } params;
    HWND window{};
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<IDXGIAdapter1> adapter;
    Ptr<IDXGIOutputDuplication> duplication;
    Ptr<IDXGISwapChain1> swapchain;
    Ptr<IDXGISwapChain2> lowLatencySwapchain;
    HANDLE frameAvailable{};
    Ptr<IDCompositionDevice> composition;
    Ptr<IDCompositionTarget> target;
    Ptr<IDCompositionVisual> root, glassVisual, webVisual;
    Ptr<ID3D11Texture2D> incoming, background;
    Ptr<ID3D11ShaderResourceView> incomingView, backgroundView;
    Ptr<ID3D11RenderTargetView> renderTarget;
    Ptr<ID3D11Texture2D> reducedTexture;
    Ptr<ID3D11ShaderResourceView> reducedView;
    Ptr<ID3D11RenderTargetView> reducedTarget;
    Ptr<ID3D11Texture2D> blurTexture;
    Ptr<ID3D11ShaderResourceView> blurView;
    Ptr<ID3D11RenderTargetView> blurTarget;
    Ptr<ID3D11VertexShader> vertex;
    Ptr<ID3D11PixelShader> pixel, downsample, blur;
    Ptr<ID3D11ComputeShader> compare;
    Ptr<ID3D11Buffer> constants, changeBuffer, changeStaging;
    Ptr<ID3D11UnorderedAccessView> changeView;
    Ptr<ID3D11SamplerState> sampler;
    HMONITOR monitor{};
    UINT width=360, height=640;
    UINT blurWidth=114, blurHeight=184;
    double pendingSourceTimeMs=0;
    void readRefreshRate(const DXGI_OUTPUT_DESC& output);
    void upload();
    void makeBackground(UINT w, UINT h);
    void draw(ID3D11PixelShader* shader, ID3D11RenderTargetView* out, UINT w, UINT h);
    void prepareBlur();
    bool differs();
public:
    ~Renderer() {if(frameAvailable)CloseHandle(frameAvailable);}
    std::wstring adapterName;
    uint64_t captured=0, rendered=0, unchanged=0;
    uint64_t presentBackpressure=0, accumulatedDesktopFrames=0;
    double refreshRateHz=60;
    TimingSamples captureWorkMs, compareWaitMs, presentCallMs, sourceToSubmitMs;
    void initialize(HWND hwnd, const std::filesystem::path& shaderFile);
    void initializeCapture();
    void resetCapture();
    bool capture();
    bool ready() const { return params.hasBackdrop > 0; }
    void resize(UINT w, UINT h, float scale);
    void move();
    void pointer(float x, float y) { params.pointer[0]=x; params.pointer[1]=y; }
    bool render(float pressure=0, float refract=1);
    IDCompositionVisual* webRoot() const { return webVisual.Get(); }
    IDCompositionDevice* compositionDevice() const { return composition.Get(); }
    std::string selfTest();
};
