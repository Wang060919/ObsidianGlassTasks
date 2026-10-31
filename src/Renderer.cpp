#include "Renderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <sstream>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
double CounterMilliseconds(int64_t ticks) {
    static const double frequency=[] {LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return static_cast<double>(value.QuadPart);}();
    return ticks*1000.0/frequency;
}
double ClockMilliseconds() {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return CounterMilliseconds(value.QuadPart);}
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char code[16]; sprintf_s(code, "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(operation) + ": " + code);
    }
}
static ComPtr<ID3DBlob> compile(const fs::path& path, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors) throw std::runtime_error(static_cast<char*>(errors->GetBufferPointer()));
    Check(hr, entry); return code;
}
void Renderer::readRefreshRate(const DXGI_OUTPUT_DESC& output) {
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsExW(output.DeviceName,ENUM_CURRENT_SETTINGS,&mode,0)||mode.dmDisplayFrequency<=1)
        throw std::runtime_error("Cannot determine the current monitor refresh rate");
    refreshRateHz=mode.dmDisplayFrequency;
}
void Renderer::initialize(HWND hwnd, const fs::path& shaderFile) {
    window=hwnd; monitor=MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory2> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    for (UINT a=0; !adapter; a++) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(a, &candidate)==DXGI_ERROR_NOT_FOUND) break;
        for (UINT i=0;; i++) {
            ComPtr<IDXGIOutput> output;
            if (candidate->EnumOutputs(i, &output)==DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{}; Check(output->GetDesc(&desc), "Output description");
            if (desc.AttachedToDesktop && desc.Monitor==monitor) { adapter=candidate;readRefreshRate(desc);break; }
        }
    }
    if (!adapter) throw std::runtime_error("No DXGI adapter owns the widget monitor");
    DXGI_ADAPTER_DESC1 adapterDesc{}; Check(adapter->GetDesc1(&adapterDesc), "Adapter description");
    adapterName=adapterDesc.Description;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11 device");
    ComPtr<IDXGIDevice> dxgiDevice; Check(device.As(&dxgiDevice), "DXGI device");
    Check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&composition)), "Composition device");
    Check(composition->CreateTargetForHwnd(hwnd, TRUE, &target), "Composition target");
    Check(composition->CreateVisual(&root), "Composition root");
    Check(composition->CreateVisual(&glassVisual), "Glass visual");
    Check(composition->CreateVisual(&webVisual), "Web visual");
    Check(root->AddVisual(glassVisual.Get(), FALSE, nullptr), "Add glass");
    Check(root->AddVisual(webVisual.Get(), TRUE, glassVisual.Get()), "Add web content");
    Check(target->SetRoot(root.Get()), "Set composition root");
    DXGI_SWAP_CHAIN_DESC1 swap{};
    swap.Width=width; swap.Height=height; swap.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    swap.SampleDesc.Count=1; swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount=2; swap.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
    swap.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Check(factory->CreateSwapChainForComposition(device.Get(), &swap, nullptr, &swapchain), "Composition swapchain");
    Check(swapchain.As(&lowLatencySwapchain),"Low latency swapchain");
    Check(lowLatencySwapchain->SetMaximumFrameLatency(1),"Limit queued glass frames");
    frameAvailable=lowLatencySwapchain->GetFrameLatencyWaitableObject();
    if(!frameAvailable)throw std::runtime_error("Frame latency waitable object unavailable");
    Check(glassVisual->SetContent(swapchain.Get()), "Glass surface");
    auto vs=compile(shaderFile,"Vertex","vs_5_0"), ps=compile(shaderFile,"Glass","ps_5_0"), ds=compile(shaderFile,"Downsample","ps_5_0"), cs=compile(shaderFile,"Compare","cs_5_0");
    Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"Vertex shader");
    Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel),"Glass shader");
    Check(device->CreatePixelShader(ds->GetBufferPointer(),ds->GetBufferSize(),nullptr,&downsample),"Backdrop prefilter shader");
    auto bs=compile(shaderFile,"BlurHorizontal","ps_5_0");
    Check(device->CreatePixelShader(bs->GetBufferPointer(),bs->GetBufferSize(),nullptr,&blur),"Gaussian shader");
    Check(device->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,&compare),"Change shader");
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=sizeof(params); buffer.Usage=D3D11_USAGE_DEFAULT; buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    Check(device->CreateBuffer(&buffer,nullptr,&constants),"Material constants");
    buffer={}; buffer.ByteWidth=4; buffer.Usage=D3D11_USAGE_DEFAULT; buffer.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    buffer.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; buffer.StructureByteStride=4;
    Check(device->CreateBuffer(&buffer,nullptr,&changeBuffer),"Change result");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; uav.Buffer.NumElements=1;
    Check(device->CreateUnorderedAccessView(changeBuffer.Get(),&uav,&changeView),"Change view");
    buffer.BindFlags=0; buffer.MiscFlags=0; buffer.StructureByteStride=0; buffer.Usage=D3D11_USAGE_STAGING; buffer.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(device->CreateBuffer(&buffer,nullptr,&changeStaging),"Change staging");
    D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sample.MaxLOD=D3D11_FLOAT32_MAX;
    Check(device->CreateSamplerState(&sample,&sampler),"Sampler");
    RECT rect{}; GetClientRect(hwnd,&rect);
    resize(rect.right,rect.bottom,GetDpiForWindow(hwnd)/96.f); move();
    Check(composition->Commit(),"Commit composition");
}
void Renderer::resize(UINT w, UINT h, float scale) {
    if (!swapchain || !w || !h) return;
    width=w; height=h; params.viewport[0]=static_cast<float>(w); params.viewport[1]=static_cast<float>(h); params.dpi=scale;
    context->OMSetRenderTargets(0,nullptr,nullptr); renderTarget.Reset();
    Check(swapchain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Resize glass");
    ComPtr<ID3D11Texture2D> buffer; Check(swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Swapchain buffer");
    Check(device->CreateRenderTargetView(buffer.Get(),nullptr,&renderTarget),"Glass target");
    blurTarget.Reset();blurView.Reset();blurTexture.Reset();
    reducedTarget.Reset();reducedView.Reset();reducedTexture.Reset();
    blurWidth=static_cast<UINT>(std::ceil((w+96*scale)/(4*scale)));blurHeight=static_cast<UINT>(std::ceil((h+96*scale)/(4*scale)));
    D3D11_TEXTURE2D_DESC tex{};tex.Width=blurWidth;tex.Height=blurHeight;tex.MipLevels=tex.ArraySize=tex.SampleDesc.Count=1;
    tex.Format=DXGI_FORMAT_B8G8R8A8_UNORM;tex.Usage=D3D11_USAGE_DEFAULT;tex.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    Check(device->CreateTexture2D(&tex,nullptr,&blurTexture),"Gaussian texture");
    Check(device->CreateRenderTargetView(blurTexture.Get(),nullptr,&blurTarget),"Gaussian target");
    Check(device->CreateShaderResourceView(blurTexture.Get(),nullptr,&blurView),"Gaussian view");
    Check(device->CreateTexture2D(&tex,nullptr,&reducedTexture),"Prefiltered texture");
    Check(device->CreateRenderTargetView(reducedTexture.Get(),nullptr,&reducedTarget),"Prefilter target");
    Check(device->CreateShaderResourceView(reducedTexture.Get(),nullptr,&reducedView),"Prefilter view");
}
void Renderer::move() {
    POINT position{}; ClientToScreen(window,&position);
    params.origin[0]=static_cast<float>(position.x); params.origin[1]=static_cast<float>(position.y);
    if (incoming && background) context->CopyResource(background.Get(),incoming.Get());
}
void Renderer::resetCapture() {
    duplication.Reset(); incomingView.Reset(); backgroundView.Reset(); incoming.Reset(); background.Reset();
    params.hasBackdrop=0;
    pendingSourceTimeMs=0;
}
void Renderer::initializeCapture() {
    resetCapture(); monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    for (UINT i=0;;i++) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i,&output)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{}; Check(output->GetDesc(&desc),"Capture output");
        if (desc.Monitor!=monitor || !desc.AttachedToDesktop) continue;
        readRefreshRate(desc);
        ComPtr<IDXGIOutput1> output1; Check(output.As(&output1),"DXGI output 1");
        Check(output1->DuplicateOutput(device.Get(),&duplication),"Desktop duplication");
        params.desktopOrigin[0]=static_cast<float>(desc.DesktopCoordinates.left);
        params.desktopOrigin[1]=static_cast<float>(desc.DesktopCoordinates.top);
        params.desktopSize[0]=static_cast<float>(desc.DesktopCoordinates.right-desc.DesktopCoordinates.left);
        params.desktopSize[1]=static_cast<float>(desc.DesktopCoordinates.bottom-desc.DesktopCoordinates.top);
        params.rotation=desc.Rotation==DXGI_MODE_ROTATION_ROTATE90 ? 1.f : desc.Rotation==DXGI_MODE_ROTATION_ROTATE180 ? 2.f : desc.Rotation==DXGI_MODE_ROTATION_ROTATE270 ? 3.f : 0.f;
        return;
    }
    throw std::runtime_error("Monitor moved to another GPU; restart the widget on that monitor");
}
void Renderer::makeBackground(UINT w, UINT h) {
    incomingView.Reset(); backgroundView.Reset(); incoming.Reset(); background.Reset();
    D3D11_TEXTURE2D_DESC tex{}; tex.Width=w; tex.Height=h; tex.ArraySize=tex.MipLevels=tex.SampleDesc.Count=1;
    tex.Format=DXGI_FORMAT_B8G8R8A8_UNORM; tex.Usage=D3D11_USAGE_DEFAULT; tex.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    Check(device->CreateTexture2D(&tex,nullptr,&incoming),"Incoming desktop");
    Check(device->CreateTexture2D(&tex,nullptr,&background),"Retained desktop");
    Check(device->CreateShaderResourceView(incoming.Get(),nullptr,&incomingView),"Incoming view");
    Check(device->CreateShaderResourceView(background.Get(),nullptr,&backgroundView),"Desktop view");
    params.hasBackdrop=0;
}
void Renderer::upload() { context->UpdateSubresource(constants.Get(),0,nullptr,&params,0,0); }
bool Renderer::differs() {
    upload(); const UINT zero[4]{}; context->ClearUnorderedAccessViewUint(changeView.Get(),zero);
    ID3D11ShaderResourceView* views[]{incomingView.Get(),backgroundView.Get()};
    context->CSSetShader(compare.Get(),nullptr,0); context->CSSetConstantBuffers(0,1,constants.GetAddressOf());
    context->CSSetShaderResources(0,2,views); context->CSSetUnorderedAccessViews(0,1,changeView.GetAddressOf(),nullptr);
    UINT padding=static_cast<UINT>(std::ceil(96*params.dpi));
    context->Dispatch((width+padding+15)/16,(height+padding+15)/16,1);
    ID3D11ShaderResourceView* empty[2]{}; ID3D11UnorderedAccessView* noUav=nullptr;
    context->CSSetShaderResources(0,2,empty); context->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
    context->CopyResource(changeStaging.Get(),changeBuffer.Get());
    double waitStart=ClockMilliseconds();
    D3D11_MAPPED_SUBRESOURCE data{}; Check(context->Map(changeStaging.Get(),0,D3D11_MAP_READ,0,&data),"Read change flag");
    compareWaitMs.add(ClockMilliseconds()-waitStart);
    bool result=*static_cast<const UINT*>(data.pData)!=0; context->Unmap(changeStaging.Get(),0); return result;
}
bool Renderer::capture() {
    if (!duplication) return false;
    double captureStart=ClockMilliseconds();
    DXGI_OUTDUPL_FRAME_INFO frame{}; ComPtr<IDXGIResource> resource;
    HRESULT hr=duplication->AcquireNextFrame(0,&frame,&resource);
    if (hr==DXGI_ERROR_WAIT_TIMEOUT) return false;
    Check(hr,"Acquire desktop frame");
    struct Release { IDXGIOutputDuplication* value; ~Release(){value->ReleaseFrame();} } release{duplication.Get()};
    if (!frame.LastPresentTime.QuadPart && ready()) return false;
    captured++;
    accumulatedDesktopFrames+=frame.AccumulatedFrames>1?frame.AccumulatedFrames-1:0;
    ComPtr<ID3D11Texture2D> texture; Check(resource.As(&texture),"Desktop texture");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (!incoming) makeBackground(desc.Width,desc.Height);
    context->CopyResource(incoming.Get(),texture.Get());
    if (ready() && !differs()) { unchanged++;captureWorkMs.add(ClockMilliseconds()-captureStart);return false; }
    context->CopyResource(background.Get(),incoming.Get()); params.hasBackdrop=1;
    pendingSourceTimeMs=frame.LastPresentTime.QuadPart?CounterMilliseconds(frame.LastPresentTime.QuadPart):0;
    captureWorkMs.add(ClockMilliseconds()-captureStart);
    return true;
}
void Renderer::draw(ID3D11PixelShader* shader, ID3D11RenderTargetView* out, UINT w, UINT h) {
    upload(); D3D11_VIEWPORT vp{0,0,static_cast<float>(w),static_cast<float>(h),0,1};
    context->RSSetViewports(1,&vp); context->OMSetRenderTargets(1,&out,nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(),nullptr,0); context->PSSetShader(shader,nullptr,0);
    context->PSSetConstantBuffers(0,1,constants.GetAddressOf());
    ID3D11ShaderResourceView* views[]{backgroundView.Get(),nullptr,shader==pixel.Get()?blurView.Get():nullptr,shader==blur.Get()?reducedView.Get():nullptr};
    context->PSSetShaderResources(0,4,views); context->PSSetSamplers(0,1,sampler.GetAddressOf());
    context->Draw(3,0);
    ID3D11ShaderResourceView* empty[4]{}; context->PSSetShaderResources(0,4,empty);
    context->OMSetRenderTargets(0,nullptr,nullptr);
}
void Renderer::prepareBlur() {
    draw(downsample.Get(),reducedTarget.Get(),blurWidth,blurHeight);
    draw(blur.Get(),blurTarget.Get(),blurWidth,blurHeight);
}
bool Renderer::render(float pressure, float refract) {
    // Never queue old glass behind another frame. Keep the newest captured texture
    // and retry on the next clock/move event when DWM is ready for a presentation.
    if(WaitForSingleObject(frameAvailable,0)!=WAIT_OBJECT_0){presentBackpressure++;return false;}
    params.press=pressure; params.refraction=refract;
    prepareBlur();
    draw(pixel.Get(),renderTarget.Get(),width,height);
    double submit=ClockMilliseconds();
    HRESULT hr=swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
    presentCallMs.add(ClockMilliseconds()-submit);
    if(hr==DXGI_ERROR_WAS_STILL_DRAWING){presentBackpressure++;return false;}
    Check(hr,"Present glass");rendered++;
    if(pendingSourceTimeMs>0){sourceToSubmitMs.add(submit-pendingSourceTimeMs);pendingSourceTimeMs=0;}
    return true;
}
std::string Renderer::selfTest() {
    // Only generated stripes are read back. This path never starts desktop capture.
    UINT queuedFrames=0;Check(lowLatencySwapchain->GetMaximumFrameLatency(&queuedFrames),"Read presentation latency");
    if(queuedFrames!=1||!frameAvailable)throw std::runtime_error("Glass presentation queue is not limited to one frame");
    resize(360,300,1); params.origin[0]=120; params.origin[1]=80;
    params.desktopOrigin[0]=params.desktopOrigin[1]=0; params.desktopSize[0]=640; params.desktopSize[1]=480;
    makeBackground(640,480);
    std::vector<unsigned char> fixture(640*480*4);
    for (UINT y=0;y<480;y++) for(UINT x=0;x<640;x++) {
        auto i=(y*640+x)*4; fixture[i]=static_cast<unsigned char>((x/48)%2 ? 220:35);
        fixture[i+1]=static_cast<unsigned char>((y/36)%2 ? 185:55); fixture[i+2]=static_cast<unsigned char>((x/64)%2 ? 190:25); fixture[i+3]=255;
    }
    context->UpdateSubresource(background.Get(),0,nullptr,fixture.data(),640*4,0);
    context->CopyResource(incoming.Get(),background.Get()); params.hasBackdrop=1;
    if (differs()) throw std::runtime_error("Identical synthetic backgrounds reported a change");
    fixture[(100*640+140)*4]^=255;
    context->UpdateSubresource(incoming.Get(),0,nullptr,fixture.data(),640*4,0);
    if (!differs()) throw std::runtime_error("Synthetic background change was missed");
    fixture[(100*640+140)*4]^=255;
    fixture[(120*640+90)*4]^=255;
    context->UpdateSubresource(incoming.Get(),0,nullptr,fixture.data(),640*4,0);
    if (!differs()) throw std::runtime_error("Backdrop change inside blur padding was missed");
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=360;desc.Height=300;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc,nullptr,&staging),"Fixture readback");
    auto pixels=[&](float refraction) {
        params.refraction=refraction;prepareBlur();draw(pixel.Get(),renderTarget.Get(),360,300);
        ComPtr<ID3D11Texture2D> buffer; Check(swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Fixture backbuffer");
        context->CopyResource(staging.Get(),buffer.Get()); D3D11_MAPPED_SUBRESOURCE data{};
        Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&data),"Read synthetic result");
        std::vector<unsigned char> result(360*300*4);
        for(UINT y=0;y<300;y++) memcpy(result.data()+y*360*4,static_cast<unsigned char*>(data.pData)+y*data.RowPitch,360*4);
        context->Unmap(staging.Get(),0); return result;
    };
    auto flat=pixels(0), refracted=pixels(1);
    UINT changedPixels=0, centerDifference=0, straightEdgePixels=0;
    for(UINT y=0;y<300;y++) for(UINT x=0;x<360;x++) {
        auto i=(y*360+x)*4; UINT diff=0;
        for(UINT c=0;c<3;c++) diff+=static_cast<UINT>(abs(int(flat[i+c])-int(refracted[i+c])));
        if(diff>3) changedPixels++;
        if(diff>3 && x>60 && x<300 && y>14 && y<42)straightEdgePixels++;
        if(x>70 && x<290 && y>70 && y<230) centerDifference+=diff;
    }
    if (refracted[3]!=0 || refracted[(150*360+180)*4+3]!=255 || changedPixels<200 || centerDifference!=0)
        throw std::runtime_error("Synthetic alpha/refraction contract failed");
    if(straightEdgePixels<1000)throw std::runtime_error("Refraction did not reach the straight lens shoulder");
    // A smooth radial backdrop must not reverse direction inside a rounded
    // corner. Mixing unrelated straight-edge and corner maps produced folds here.
    for(UINT y=0;y<480;y++)for(UINT x=0;x<640;x++) {
        float localX=x+.5f-params.origin[0],localY=y+.5f-params.origin[1];
        float cx=localX<180?40.f:320.f,cy=localY<150?40.f:260.f;
        auto tone=static_cast<unsigned char>(std::clamp(40.f+6.f*std::hypot(localX-cx,localY-cy),0.f,255.f));
        auto i=(y*640+x)*4;fixture[i]=fixture[i+1]=fixture[i+2]=tone;
    }
    context->UpdateSubresource(background.Get(),0,nullptr,fixture.data(),640*4,0);
    auto corners=pixels(1);int cornerReversal=0;
    for(UINT corner=0;corner<4;corner++) {
        int last=255;
        for(UINT step=22;step<=31;step++) {
            UINT x=(corner&1)?359-step:step,y=(corner&2)?299-step:step;
            int value=corners[(y*360+x)*4+1];cornerReversal=std::max(cornerReversal,value-last);last=value;
        }
    }
    if(cornerReversal>2)throw std::runtime_error("Corner refraction reverses a smooth backdrop: jump="+std::to_string(cornerReversal));
    for(UINT y=0;y<480;y++)for(UINT x=0;x<640;x++) {
        auto i=(y*640+x)*4;fixture[i]=fixture[i+1]=fixture[i+2]=static_cast<unsigned char>((x+y)%2?255:0);fixture[i+3]=255;
    }
    context->UpdateSubresource(background.Get(),0,nullptr,fixture.data(),640*4,0);
    auto checker=pixels(0);UINT minimum[3]{255,255,255},maximum[3]{};
    for(UINT y=70;y<230;y++)for(UINT x=70;x<290;x++)for(UINT c=0;c<3;c++) {
        UINT value=checker[(y*360+x)*4+c];minimum[c]=std::min(minimum[c],value);maximum[c]=std::max(maximum[c],value);
    }
    UINT checkerResidual=0;for(UINT c=0;c<3;c++)checkerResidual=std::max(checkerResidual,maximum[c]-minimum[c]);
    if(checkerResidual>2)throw std::runtime_error("Fine backdrop texture aliases through the reading surface");
    // Clear glass must retain medium-size features in both axes while filtering
    // the single-pixel checker above. A wide frosted blur fails this distinction.
    UINT detailContrast=255;
    for(UINT axis=0;axis<2;axis++) {
        for(UINT y=0;y<480;y++)for(UINT x=0;x<640;x++) {
            auto i=(y*640+x)*4;
            fixture[i]=fixture[i+1]=fixture[i+2]=static_cast<unsigned char>(((axis?y:x)/16)%2?255:0);
        }
        context->UpdateSubresource(background.Get(),0,nullptr,fixture.data(),640*4,0);
        auto detail=pixels(0);UINT low=255,high=0;
        for(UINT y=70;y<230;y++)for(UINT x=70;x<290;x++) {
            UINT value=detail[(y*360+x)*4+1];low=std::min(low,value);high=std::max(high,value);
        }
        detailContrast=std::min(detailContrast,high-low);
    }
    if(detailContrast<100)throw std::runtime_error("Medium-size backdrop detail was lost: contrast="+std::to_string(detailContrast));
    std::fill(fixture.begin(),fixture.end(),static_cast<unsigned char>(255));
    context->UpdateSubresource(background.Get(),0,nullptr,fixture.data(),640*4,0);
    auto white=pixels(0);
    for(size_t i=0;i<fixture.size();i+=4)fixture[i]=fixture[i+1]=fixture[i+2]=0;
    context->UpdateSubresource(background.Get(),0,nullptr,fixture.data(),640*4,0);
    auto black=pixels(0);
    UINT center=(150*360+180)*4;
    UINT whitePeak=std::max({white[center],white[center+1],white[center+2]});
    UINT blackPeak=std::max({black[center],black[center+1],black[center+2]});
    if(whitePeak<200||whitePeak>236||blackPeak>16)throw std::runtime_error("Clear dark glass lost transmission or lifted the black backdrop");
    auto linear=[](unsigned char channel){double value=channel/255.0;return value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4);};
    double whiteReadingMaxLuminance=0;
    for(UINT y=32;y<268;y++)for(UINT x=32;x<328;x++) {
        UINT i=(y*360+x)*4;
        whiteReadingMaxLuminance=std::max(whiteReadingMaxLuminance,.2126*linear(white[i+2])+.7152*linear(white[i+1])+.0722*linear(white[i]));
    }
    std::ostringstream report;
    report << "{\"ok\":true,\"background\":\"generated stripes, checkerboard, black and white only\",\"shaderCompiled\":true,\"outsideAlpha\":" << int(refracted[3])
           << ",\"insideAlpha\":255,\"refractionChangedPixels\":" << changedPixels << ",\"centerDifference\":" << centerDifference
           << ",\"changeDetection\":true,\"darkMaterial\":true"
           << ",\"straightEdgeRefractionPixels\":" << straightEdgePixels << ",\"fineTextureResidual\":" << checkerResidual
           << ",\"cornerGradientReversal\":" << cornerReversal
           << ",\"mediumTextureContrast\":" << detailContrast
           << ",\"whiteBackdropCenterPeak\":" << whitePeak << ",\"blackBackdropCenterPeak\":" << blackPeak
           << ",\"whiteReadingMaxLuminance\":" << whiteReadingMaxLuminance << ",\"blurPaddingChangesDetected\":true"
           << ",\"maxQueuedFrames\":" << queuedFrames << ",\"refreshRateHz\":" << refreshRateHz << "}";
    return report.str();
}
