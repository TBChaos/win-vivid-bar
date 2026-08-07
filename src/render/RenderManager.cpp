// src/render/RenderManager.cpp
#include "RenderManager.h"
#include "../utils/DiagLog.h"
#include <stdarg.h>
#include <shellapi.h>   // SHGetFileInfo（系统默认文件/文件夹图标兜底）

// 诊断日志（真实 GUI 下 stderr 不可见）：写入 debug_output/openDock_render.log，
// 诊断日志已统一至 DiagLog("render", ...)（src/utils/DiagLog.h）。

// ═══════════════════════════════════════════════════════════
// 初始化
// ═══════════════════════════════════════════════════════════
HRESULT RenderManager::Initialize(Mode mode, HWND hwnd, const DockConfig& config) {
    m_mode   = mode;
    m_config = config;

    if (mode == Mode::Windowed && hwnd == nullptr) {
        return E_INVALIDARG;
    }

    // Dock 逻辑尺寸（静息态；竖直朝向自动交换宽高）。统一几何：据 config.position
    // 构造多态实例（编译期模板 + 运行时多态），消除旧 free function 的运行时 position 分支。
    m_geom = MakeGeometry(config.position);
    m_geom->computeBarSize(config.iconCount, config.baseIconSize, config.iconSpacing,
                           config.dockPadding, m_dockWidth, m_dockHeight);

    // ═══ 降级策略（详细设计 §6.1）═══
    // 测试钩子强制 GDI，或 DComp/D2D 创建失败 → GDI + Layered Window 回退
    if (m_forceGdi) {
        DOCK_HR_CHECK(InitializeGDI(hwnd), "InitializeGDI(forced)");
        m_renderMode  = RenderMode::GDI_Fallback;
        m_initialized = true;
        return S_OK;
    }

    HRESULT hr = CreateDeviceResources();
    if (SUCCEEDED(hr)) {
        if (mode == Mode::Windowed) {
            hr = CreateCompositionResources(hwnd);
        } else {
            hr = CreateHeadlessTarget();
        }
    }

    if (FAILED(hr)) {
        // 自动降级：DirectComposition 不可用 → GDI + UpdateLayeredWindow
        DOCK_HR_CHECK(InitializeGDI(hwnd), "InitializeGDI(fallback)");
        m_renderMode = RenderMode::GDI_Fallback;
    } else {
        m_renderMode = RenderMode::DirectComposition;
    }

    m_initialized = true;
    return S_OK;
}

HRESULT RenderManager::CreateDeviceResources() {
    // 1. D3D11 设备（BGRA 支持是 D2D 互操作必需）
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &m_d3dDevice, nullptr, &m_d3dContext);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &m_d3dDevice, nullptr, &m_d3dContext);
    }
    DOCK_HR_CHECK(hr, "D3D11CreateDevice");

    DOCK_HR_CHECK(m_d3dDevice.As(&m_dxgiDevice), "QI IDXGIDevice");

    // 2. D2D 工厂 / 设备 / 上下文
    D2D1_FACTORY_OPTIONS fo = {};
#ifdef DOCK_DEBUG_MODE
    fo.debugLevel = D2D1_DEBUG_LEVEL_NONE;   // 沙盒无调试层，保持 NONE
#endif
    DOCK_HR_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory1), &fo,
                                    reinterpret_cast<void**>(m_d2dFactory.GetAddressOf())),
                  "D2D1CreateFactory");
    DOCK_HR_CHECK(m_d2dFactory->CreateDevice(m_dxgiDevice.Get(), &m_d2dDevice),
                  "ID2D1Factory1::CreateDevice");
    DOCK_HR_CHECK(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                   &m_d2dContext),
                  "CreateDeviceContext");

    // 3. DirectWrite 工厂
    DOCK_HR_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())),
                  "DWriteCreateFactory");

    // 3.1 Tooltip 文本格式（Segoe UI 12px，详细设计 §2.6）
    // Tooltip 格式创建失败为非致命（后续绘制会跳过 tooltip），故不检查返回值。
    m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"", &m_tooltipFormat);

    // 4. WIC 工厂（图标解码）
    DOCK_HR_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&m_wicFactory)),
                  "CoCreateInstance WICImagingFactory");

    // 5. DPI（GetDpiForSystem: Win10 1607+，替代已弃用的 GetDesktopDpi）
    m_dpiScale = static_cast<float>(GetDpiForSystem()) / 96.0f;

    return S_OK;
}

HRESULT RenderManager::CreateCompositionResources(HWND hwnd) {
    // DirectComposition 设备
    DOCK_HR_CHECK(DCompositionCreateDevice(m_dxgiDevice.Get(),
                                           IID_PPV_ARGS(&m_dcDevice)),
                  "DCompositionCreateDevice");

    // 合成目标（topmost = TRUE）
    DOCK_HR_CHECK(m_dcDevice->CreateTargetForHwnd(hwnd, TRUE, &m_dcTarget),
                  "CreateTargetForHwnd");

    // Visual 树：Root → Background → Icon[N]
    DOCK_HR_CHECK(m_dcDevice->CreateVisual(&m_rootVisual), "CreateVisual root");
    DOCK_HR_CHECK(m_dcDevice->CreateVisual(&m_backgroundVisual), "CreateVisual bg");
    DOCK_HR_CHECK(m_rootVisual->AddVisual(m_backgroundVisual.Get(), FALSE, nullptr),
                  "AddVisual bg");

    m_iconVisuals.resize(static_cast<size_t>(m_config.iconCount));
    for (int i = 0; i < m_config.iconCount; ++i) {
        DOCK_HR_CHECK(m_dcDevice->CreateVisual(&m_iconVisuals[(size_t)i]),
                      "CreateVisual icon");
        DOCK_HR_CHECK(m_rootVisual->AddVisual(m_iconVisuals[(size_t)i].Get(),
                                              TRUE, nullptr),
                      "AddVisual icon");
    }

    // Tooltip 视觉（置于图标之上）
    DOCK_HR_CHECK(m_dcDevice->CreateVisual(&m_tooltipVisual), "CreateVisual tooltip");
    DOCK_HR_CHECK(m_rootVisual->AddVisual(m_tooltipVisual.Get(), TRUE, nullptr),
                  "AddVisual tooltip");
    {
        ComPtr<IDCompositionVisual3> v3;
        if (SUCCEEDED(m_tooltipVisual.As(&v3))) v3->SetOpacity(0.0f);
    }

    // 图标阴影（详细设计 §2.4.7）：ShadowEffect 输出仅为阴影本身，
    // 因此用效果图：Shadow → AffineTransform2D(下移2px) → Composite(SOURCE_OVER 原图)
    if (m_shadowEnabled) {
        ComPtr<IDCompositionDevice3> dev3;
        if (SUCCEEDED(m_dcDevice.As(&dev3))) {
            m_iconShadows.resize(m_iconVisuals.size());
            for (size_t i = 0; i < m_iconVisuals.size(); ++i) {
                ComPtr<IDCompositionShadowEffect>            shadow;
                ComPtr<IDCompositionAffineTransform2DEffect> offset;
                ComPtr<IDCompositionCompositeEffect>         composite;
                if (SUCCEEDED(dev3->CreateShadowEffect(&shadow)) &&
                    SUCCEEDED(dev3->CreateAffineTransform2DEffect(&offset)) &&
                    SUCCEEDED(dev3->CreateCompositeEffect(&composite))) {
                    shadow->SetStandardDeviation(4.0f);
                    D2D1_VECTOR_4F shadowColor{0.0f, 0.0f, 0.0f, 0.3f};
                    shadow->SetColor(shadowColor);
                    // 未指定的输入默认取 Visual 自身内容
                    offset->SetInput(0, shadow.Get(), 0);
                    offset->SetTransformMatrix(
                        D2D1::Matrix3x2F::Translation(0.0f, 2.0f));
                    composite->SetMode(D2D1_COMPOSITE_MODE_SOURCE_OVER);
                    composite->SetInput(0, offset.Get(), 0);  // 阴影在下
                    // input 1 缺省 = 原始图标内容，叠加在上
                    m_iconVisuals[(size_t)i]->SetEffect(composite.Get());
                    m_iconShadows[i].shadow    = shadow;
                    m_iconShadows[i].offset    = offset;
                    m_iconShadows[i].composite = composite;
                } else {
                }
            }
        } else {
        }
    }

    // 背景条：圆角半透明表面（macOS 风格 Dock 底座）
    DOCK_HR_CHECK(CreateBackgroundSurface(), "CreateBackgroundSurface");

    DOCK_HR_CHECK(m_dcTarget->SetRoot(m_rootVisual.Get()), "SetRoot");
    return S_OK;
}

HRESULT RenderManager::CreateHeadlessTarget() {
    // GPU 渲染目标位图
    D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    DOCK_HR_CHECK(m_d2dContext->CreateBitmap(
                      D2D1::SizeU(HEADLESS_W, HEADLESS_H), nullptr, 0,
                      &targetProps, &m_headlessTarget),
                  "CreateBitmap headless target");

    // CPU 回读位图
    D2D1_BITMAP_PROPERTIES1 readProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    DOCK_HR_CHECK(m_d2dContext->CreateBitmap(
                      D2D1::SizeU(HEADLESS_W, HEADLESS_H), nullptr, 0,
                      &readProps, &m_readbackBitmap),
                  "CreateBitmap readback");
    return S_OK;
}

void RenderManager::Shutdown() {
    ReleaseGdiResources();
    m_iconVisuals.clear();
    m_iconBitmaps.clear();
    m_iconShadows.clear();
    m_tooltipVisual.Reset();
    m_tooltipFormat.Reset();
    m_dcTarget.Reset();
    m_dcDevice.Reset();
    // 设备级资源：按依赖自顶向下释放（DComp → D2D → DWrite/WIC → DXGI → D3D）
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_dwriteFactory.Reset();
    m_wicFactory.Reset();
    m_dxgiDevice.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_headlessTarget.Reset();
    m_readbackBitmap.Reset();
    m_initialized = false;
}

RenderManager::~RenderManager() {
    // 防御性释放：确保 COM 设备/工厂在最晚的析构时机前被释放
    if (m_initialized) Shutdown();
}

// ═══════════════════════════════════════════════════════════
// 图标纹理加载（WIC → D2D Bitmap）
// ═══════════════════════════════════════════════════════════
ComPtr<ID2D1Bitmap1> RenderManager::DecodeToBitmap(const std::wstring& path) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(m_wicFactory->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) return nullptr;
    ComPtr<ID2D1Bitmap1> bitmap;
    hr = m_d2dContext->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
    if (FAILED(hr)) return nullptr;
    return bitmap;
}

// 把内存 PNG 字节包装为只读 WIC 流（windowscodecs 原生，无需 shlwapi/SHCreateMemStream）。
// 注意：InitializeFromMemory 不拷贝缓冲区，调用方须保证 bytes 在解码期间存活
// —— 本工程中 bytes 属 IconProvider::m_images/IconImage，生命周期长于同步解码调用。
static ComPtr<IWICStream> MakeMemStream(IWICImagingFactory* factory,
                                        const std::vector<uint8_t>& bytes) {
    ComPtr<IWICStream> stream;
    if (!factory || bytes.empty()) return nullptr;
    if (FAILED(factory->CreateStream(&stream)) || !stream) return nullptr;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))))
        return nullptr;
    return stream;
}

ComPtr<ID2D1Bitmap1> RenderManager::DecodeToBitmap(const IconProvider::IconImage& img) {
    if (!m_wicFactory || !m_d2dContext) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = E_FAIL;
    if (!img.pngBytes.empty()) {
        // 内存流解码：CacheOnLoad 使解码器立即缓存位图，解码后即可释放流，
        // 消解原 CacheOnDemand 持流不放导致的上游锁因（诊断报告行动 2）。
        ComPtr<IWICStream> stream = MakeMemStream(m_wicFactory.Get(), img.pngBytes);
        if (!stream) return nullptr;
        hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                   WICDecodeMetadataCacheOnLoad, &decoder);
    } else if (!img.filePath.empty()) {
        hr = m_wicFactory->CreateDecoderFromFilename(
            img.filePath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder);
    } else {
        return nullptr;
    }
    if (FAILED(hr) || !decoder) return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(m_wicFactory->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) return nullptr;
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(m_d2dContext->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap)))
        return nullptr;
    return bitmap;
}

ComPtr<ID2D1Bitmap1> RenderManager::CreateDefaultIconBitmap() {
    // 真实系统「通用文件」图标（非灰、非透明），作为解码彻底失败时的兜底，
    // 保证纹理数恒等于图标数且不出现灰色/失效图标（需求2）。
    SHFILEINFOW sfi = {};
    if (!SHGetFileInfoW(L"__openDock_default_file__", FILE_ATTRIBUTE_NORMAL, &sfi,
                        sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)
        || !sfi.hIcon) {
        return nullptr;
    }
    HICON hIcon = sfi.hIcon;
    // HICON → 32bpp 顶向下 DIB（256x256），再经 WIC → D2D 位图
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) { DestroyIcon(hIcon); return nullptr; }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = 256;
    bmi.bmiHeader.biHeight      = -256;   // 负高 = 顶向下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&bits), nullptr, 0);
    ComPtr<ID2D1Bitmap1> d2d;
    if (hbmp) {
        HGDIOBJ old = SelectObject(hdc, hbmp);
        BitBlt(hdc, 0, 0, 256, 256, nullptr, 0, 0, BLACKNESS);
        DrawIconEx(hdc, 0, 0, hIcon, 256, 256, 0, nullptr, DI_NORMAL);
        SelectObject(hdc, old);
        ComPtr<IWICBitmap> wicBmp;
        if (SUCCEEDED(m_wicFactory->CreateBitmapFromMemory(
                256, 256, GUID_WICPixelFormat32bppBGRA, 256 * 4, 256 * 4,
                reinterpret_cast<BYTE*>(bits), &wicBmp)) && wicBmp) {
            m_d2dContext->CreateBitmapFromWicBitmap(wicBmp.Get(), nullptr, &d2d);
        }
        DeleteObject(hbmp);
    }
    DeleteDC(hdc);
    DestroyIcon(hIcon);
    return d2d;
}

// 便利重载：字面量列表 → 走 vector<wstring> 兼容路径（消解 C2668，见头文件注释）
HRESULT RenderManager::LoadIconTextures(std::initializer_list<const wchar_t*> iconPaths) {
    std::vector<std::wstring> paths;
    paths.reserve(iconPaths.size());
    for (const wchar_t* p : iconPaths) if (p) paths.emplace_back(p);
    return LoadIconTextures(paths);
}

// 兼容重载：纯文件路径（GDI/渲染单测）→ 包装为 IconImage{ .filePath = p } 后统一走内存流版
HRESULT RenderManager::LoadIconTextures(const std::vector<std::wstring>& iconPaths) {
    std::vector<IconProvider::IconImage> imgs;
    imgs.reserve(iconPaths.size());
    for (const auto& p : iconPaths) {
        IconProvider::IconImage img;
        img.filePath = p;
        imgs.push_back(std::move(img));
    }
    return LoadIconTextures(imgs);
}

HRESULT RenderManager::LoadIconTextures(const std::vector<IconProvider::IconImage>& imgs) {
    if (m_renderMode == RenderMode::GDI_Fallback) {
        return LoadIconBitmapsGDI(imgs);   // GDI 路径：WIC → HBITMAP DIB
    }
    m_iconBitmaps.clear();
    m_iconBitmaps.reserve(imgs.size());

    int nReal = 0, nDefault = 0;
    for (size_t k = 0; k < imgs.size(); ++k) {
        const auto& img = imgs[k];
        // 解码失败（极罕见：IconProvider 已保证真实系统图标）→ 用系统默认文件图标兜底，
        // 保证 m_iconBitmaps.size() 恒等于 imgs.size()，且不出现灰/失效图标。
        ComPtr<ID2D1Bitmap1> bitmap = DecodeToBitmap(img);
        if (bitmap) {
            ++nReal;
        } else {
            bitmap = CreateDefaultIconBitmap();
            if (bitmap) { ++nDefault; DiagLog("render","TEX[%zu] decode FAIL, used system default icon", k); }
        }
        if (!bitmap) { DiagLog("render","TEX[%zu] ALL fallback failed (null)", k); continue; }

        m_iconBitmaps.push_back(bitmap);

        // Windowed 模式：为对应 Visual 烘焙 Surface 内容
        if (m_mode == Mode::Windowed) {
            size_t index = m_iconBitmaps.size() - 1;
            if (index < m_iconVisuals.size()) {
                CreateIconSurface(index);
            } else {
                DiagLog("render","TEX[%zu] index %zu >= iconVisuals %zu (skip bake)",
                          k, index, m_iconVisuals.size());
            }
        }
    }

    int nBitmaps = (int)m_iconBitmaps.size();
    int nVisuals = (int)m_iconVisuals.size();
    DiagLog("render","LoadIconTextures: imgs=%zu real=%d default=%d bitmaps=%d visuals=%d%s",
              imgs.size(), nReal, nDefault, nBitmaps, nVisuals,
              (nBitmaps != (int)imgs.size()) ? " COUNT_MISMATCH" : "");
    return S_OK;
}

D2D1_SIZE_F RenderManager::GetIconBitmapSize(size_t index) const {
    if (index >= m_iconBitmaps.size()) return D2D1::SizeF(0, 0);
    return m_iconBitmaps[index]->GetSize();
}

// ═══════════════════════════════════════════════════════════
// 动态重建图标集（Step 8：增删 / 重排后调用）
// ═══════════════════════════════════════════════════════════
HRESULT RenderManager::RebuildIconSet(const std::vector<IconProvider::IconImage>& imgs) {
    if (!m_initialized) return E_NOT_VALID_STATE;

    int n = static_cast<int>(imgs.size());
    m_config.iconCount = n;
    m_geom->computeBarSize(n, m_config.baseIconSize, m_config.iconSpacing,
                           m_config.dockPadding, m_dockWidth, m_dockHeight);

    // GDI 回退：仅需重新解码图标位图（画布尺寸不变，帧级全量重绘）
    if (m_renderMode == RenderMode::GDI_Fallback) {
        HRESULT hrG = (n > 0) ? LoadIconBitmapsGDI(imgs) : S_OK;
        if (n == 0) { for (auto& gi : m_gdiIcons) if (gi.bmp) DeleteObject(gi.bmp); m_gdiIcons.clear(); }
        return hrG;
    }

    // 清除旧图标视觉 / 阴影
    m_iconShadows.clear();
    for (auto& v : m_iconVisuals) {
        if (m_rootVisual) m_rootVisual->RemoveVisual(v.Get());
        v.Reset();
    }
    m_iconVisuals.clear();
    m_iconBitmaps.clear();

    if (m_mode == Mode::Windowed) {
        DiagLog("render","RebuildIconSet Windowed: n=%d gdi=%d", n,
                  (m_renderMode == RenderMode::GDI_Fallback) ? 1 : 0);
        // 重建图标视觉
        m_iconVisuals.resize(static_cast<size_t>(n > 0 ? n : 0));
        for (int i = 0; i < n; ++i) {
            DOCK_HR_CHECK(m_dcDevice->CreateVisual(&m_iconVisuals[(size_t)i]),
                          "CreateVisual icon(rebuild)");
            DOCK_HR_CHECK(m_rootVisual->AddVisual(m_iconVisuals[(size_t)i].Get(),
                                                  TRUE, nullptr),
                          "AddVisual icon(rebuild)");
        }
        // 图标阴影效果图（与 CreateCompositionResources 同构）
        if (m_shadowEnabled) {
            ComPtr<IDCompositionDevice3> dev3;
            if (SUCCEEDED(m_dcDevice.As(&dev3))) {
                m_iconShadows.resize(m_iconVisuals.size());
                for (size_t i = 0; i < m_iconVisuals.size(); ++i) {
                    ComPtr<IDCompositionShadowEffect>            shadow;
                    ComPtr<IDCompositionAffineTransform2DEffect> offset;
                    ComPtr<IDCompositionCompositeEffect>         composite;
                    if (SUCCEEDED(dev3->CreateShadowEffect(&shadow)) &&
                        SUCCEEDED(dev3->CreateAffineTransform2DEffect(&offset)) &&
                        SUCCEEDED(dev3->CreateCompositeEffect(&composite))) {
                        shadow->SetStandardDeviation(4.0f);
                        D2D1_VECTOR_4F shadowColor{0.0f, 0.0f, 0.0f, 0.3f};
                        shadow->SetColor(shadowColor);
                        offset->SetInput(0, shadow.Get(), 0);
                        offset->SetTransformMatrix(D2D1::Matrix3x2F::Translation(0.0f, 2.0f));
                        composite->SetMode(D2D1_COMPOSITE_MODE_SOURCE_OVER);
                        composite->SetInput(0, offset.Get(), 0);
                        m_iconVisuals[i]->SetEffect(composite.Get());
                        m_iconShadows[i].shadow    = shadow;
                        m_iconShadows[i].offset    = offset;
                        m_iconShadows[i].composite = composite;
                    }
                }
            }
        }
        // 背景重绘（新尺寸）
        DOCK_HR_CHECK(CreateBackgroundSurface(), "CreateBackgroundSurface(rebuild)");
        // 保证 Tooltip 仍位于最上层
        if (m_tooltipVisual) {
            m_rootVisual->RemoveVisual(m_tooltipVisual.Get());
            m_rootVisual->AddVisual(m_tooltipVisual.Get(), TRUE, nullptr);
        }
    }

    // 重新解码图标位图（Windowed 同时烘焙 Surface）
    if (n > 0) {
        DOCK_HR_CHECK(LoadIconTextures(imgs), "LoadIconTextures(rebuild)");
    } else {
    }

    if (m_mode == Mode::Windowed && m_dcDevice) {
        HRESULT hrCommit = m_dcDevice->Commit();
        DiagLog("render","RebuildIconSet Windowed END: n=%d bitmaps=%zu visuals=%zu commit=0x%08X",
                  n, m_iconBitmaps.size(), m_iconVisuals.size(), (unsigned)hrCommit);
    }
    return S_OK;
}

// Windowed：将图标位图绘制到 DComp Surface 并挂到 Visual
HRESULT RenderManager::CreateIconSurface(size_t index) {
    UINT size = static_cast<UINT>(m_config.baseIconSize * m_config.maxScale);

    ComPtr<IDCompositionSurface> surface;
    HRESULT hrCreate = m_dcDevice->CreateSurface(size, size,
                      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                      &surface);
    if (FAILED(hrCreate)) {
        DiagLog("render","CreateIconSurface[%zu] CreateSurface FAIL hr=0x%08X", index, (unsigned)hrCreate);
        return S_OK;
    }

    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {};
    HRESULT hrBegin = surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hrBegin)) {
        DiagLog("render","CreateIconSurface[%zu] BeginDraw FAIL hr=0x%08X", index, (unsigned)hrBegin);
        return S_OK;
    }

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> targetBitmap;
    HRESULT hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props,
                                                           &targetBitmap);
    HRESULT hrDraw = S_OK;
    if (SUCCEEDED(hr)) {
        m_d2dContext->SetTarget(targetBitmap.Get());
        m_d2dContext->BeginDraw();
        // 【图集安全】IDCompositionSurface::BeginDraw 返回的 IDXGISurface 是 DComp 内部的
        // 【共享图集纹理】，offset 为本 Surface 在该图集中的子矩形原点。ID2D1DeviceContext::Clear
        // 在【无裁剪】时作用于整个目标位图（= 整张图集），会把同一设备上已分配的其它
        // Surface 一并清成透明。必须先 PushAxisAlignedClip 限定到本 Surface 子矩形再 Clear。
        const float ox = static_cast<float>(offset.x);
        const float oy = static_cast<float>(offset.y);
        m_d2dContext->PushAxisAlignedClip(
            D2D1::RectF(ox, oy, ox + (float)size, oy + (float)size),
            D2D1_ANTIALIAS_MODE_ALIASED);
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
        // 基础尺寸绘制（放大靠 Visual Transform，不重绘）
        // 防御：位图缺失（WIC 解码失败/索引越界）时若仍调用 DrawBitmap(nullptr)，
        // D2D 会把设备上下文置入错误态 —— 该错误态持续到 EndDraw，使【本次 BeginDraw
        // 块内后续所有绘制静默失效】，表现为整批图标空白且只在真机可见。
        if (index < m_iconBitmaps.size() && m_iconBitmaps[index]) {
            m_d2dContext->DrawBitmap(
                m_iconBitmaps[index].Get(),
                D2D1::RectF(ox, oy, ox + m_config.baseIconSize, oy + m_config.baseIconSize),
                1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {
            DiagLog("render","CreateIconSurface[%zu] MISSING BITMAP (size=%zu) -> blank icon",
                      index, m_iconBitmaps.size());
        }
        m_d2dContext->PopAxisAlignedClip();
        hrDraw = m_d2dContext->EndDraw();
        m_d2dContext->SetTarget(nullptr);
    } else {
        DiagLog("render","CreateIconSurface[%zu] CreateBitmapFromDxgiSurface FAIL hr=0x%08X",
                  index, (unsigned)hr);
    }
    HRESULT hrEnd = surface->EndDraw();

    HRESULT hrSet = m_iconVisuals[index]->SetContent(surface.Get());
    DiagLog("render","CreateIconSurface[%zu] create=0x%08X begin=0x%08X d2dEnd=0x%08X surfEnd=0x%08X setContent=0x%08X",
              index, (unsigned)hrCreate, (unsigned)hrBegin, (unsigned)hrDraw,
              (unsigned)hrEnd, (unsigned)hrSet);
    if (FAILED(hrSet)) {
        DiagLog("render","CreateIconSurface[%zu] SetContent FAIL hr=0x%08X", index, (unsigned)hrSet);
    }
    return S_OK;
}

// Windowed：为根背景 Visual 烘焙圆角半透明 Dock 底座
HRESULT RenderManager::CreateBackgroundSurface() {
    if (!m_backgroundVisual) return S_OK;   // 防御：Headless 不创建背景

    // #N：底座背景条关闭时保持透明（无内容 / opacity=0），仅浮出图标；
    // 切换显隐由 SetBarVisible 触发本函数重建。
    if (!m_barVisible) {
        ComPtr<IDCompositionVisual3> v3;
        if (SUCCEEDED(m_backgroundVisual.As(&v3))) v3->SetOpacity(0.0f);
        return S_OK;
    }

    // Step 12 / #6：背景 Surface 必须覆盖整个窗口（含四边留白 m_winW×m_winH），
    // 而非仅 m_dockWidth×m_dockHeight。圆角条按 (m_offsetX,m_offsetY) 偏移绘制，
    // 若 Surface 仅 Dock 尺寸，条的大部分/全部会被裁切，呈现为「窗口左上角的
    // 半透明矩形蒙版」式错位（Dock 底座在多数停靠方向下被切掉一大块甚至不可见）。
    UINT w = static_cast<UINT>(std::max(1.0f, (float)m_winW));
    UINT h = static_cast<UINT>(std::max(1.0f, (float)m_winH));

    ComPtr<IDCompositionSurface> surface;
    DOCK_HR_CHECK(m_dcDevice->CreateSurface(w, h,
                      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                      &surface),
                  "CreateSurface bg");

    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {};
    DOCK_HR_CHECK(surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset),
                  "Surface BeginDraw bg");

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> targetBitmap;
    HRESULT hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props,
                                                           &targetBitmap);
    if (SUCCEEDED(hr)) {
        m_d2dContext->SetTarget(targetBitmap.Get());
        m_d2dContext->BeginDraw();
        // 【图集安全】同 CreateIconSurface：BeginDraw 给出的是共享图集纹理 + 子矩形原点
        // offset。此前本函数（a）无裁剪直接 Clear —— 会清掉同图集内【全部图标 Surface】，
        // 且本函数总在图标 Surface 之后被调用（Initialize 尾部 + 每次 SetContentInsets），
        // 表现即「底座在、图标全不见」；（b）完全忽略 offset —— 圆角条被画到图集里别人的
        // 位置上。两者都必须修正。
        const float bx = static_cast<float>(offset.x);
        const float by = static_cast<float>(offset.y);
        m_d2dContext->PushAxisAlignedClip(
            D2D1::RectF(bx, by, bx + (float)w, by + (float)h),
            D2D1_ANTIALIAS_MODE_ALIASED);
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));

        float r = std::min(m_cornerRadius, std::min(m_dockWidth, m_dockHeight) * 0.5f);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(bx + m_offsetX, by + m_offsetY,
                        bx + m_offsetX + m_dockWidth,
                        by + m_offsetY + m_dockHeight), r, r);

        ComPtr<ID2D1SolidColorBrush> brush;
        // 需求：显示区背景完全透明。底座条填充 alpha 强制为 0（即便 dockBarVisible=true
        // 也不绘制任何可见底色），仅浮出图标；圆角几何保留以便命中/布局逻辑不变。
        hr = m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.16f, 0.16f, 0.18f, 0.0f), &brush);
        if (SUCCEEDED(hr)) {
            m_d2dContext->FillRoundedRectangle(&rr, brush.Get());
        }
        m_d2dContext->PopAxisAlignedClip();
        hr = m_d2dContext->EndDraw();
        m_d2dContext->SetTarget(nullptr);
    }
    surface->EndDraw();
    DOCK_HR_CHECK(hr, "Bg surface draw");

    DOCK_HR_CHECK(m_backgroundVisual->SetContent(surface.Get()), "SetContent bg");
    return S_OK;
}

// ═══════════════════════════════════════════════════════════
// 动画更新
// ═══════════════════════════════════════════════════════════
void RenderManager::UpdateVisualTransforms(const std::vector<IconLayout>& layouts) {
    m_lastLayouts = layouts;

    if (m_renderMode == RenderMode::GDI_Fallback) {
        return;  // GDI：CommitFrame 时软件全量重绘
    }
    if (m_mode != Mode::Windowed) {
        return;  // Headless：CommitFrame 时统一离屏绘制
    }

    // ⚡ 零重绘路径：仅修改 Visual 属性，由 DWM 在 GPU 完成变换
    // ADR §1.5 INV-VISUAL：绘制像素范围必须由 iconVisualRect 推导，不得自行 mapLayout。
    const IconGeometryParams gp{ m_dockWidth, m_dockHeight,
                                 m_config.baseIconSize, m_config.dockPadding };
    for (size_t i = 0; i < layouts.size() && i < m_iconVisuals.size(); ++i) {
        const IconLayout& L = layouts[i];

        // 以图标中心为缩放原点：Surface 内容左上角在 (0,0)，基础尺寸 baseIconSize
        float half = m_config.baseIconSize * 0.5f;
        D2D1_MATRIX_3X2_F transform =
            D2D1::Matrix3x2F::Scale(L.scale, L.scale, D2D1::Point2F(half, half));

        // 平移：单一真源可见矩形中心 → 窗口绝对坐标
        const RectF vis = m_geom->iconVisualRect(L, gp);
        transform = transform * D2D1::Matrix3x2F::Translation(
            m_offsetX + vis.cx() - half,
            m_offsetY + vis.cy() - half);

        m_iconVisuals[i]->SetTransform(transform);

        // 透明度：IDCompositionVisual3（Win 8.1+）
        ComPtr<IDCompositionVisual3> v3;
        if (SUCCEEDED(m_iconVisuals[i].As(&v3))) {
            v3->SetOpacity(L.opacity);
        }
    }
}

void RenderManager::UpdateBackground(float opacity) {
    m_backgroundOpacity = opacity;
    if (m_mode == Mode::Windowed && m_backgroundVisual) {
        ComPtr<IDCompositionVisual3> v3;
        if (SUCCEEDED(m_backgroundVisual.As(&v3))) {
            v3->SetOpacity(opacity);
        }
    }
}

void RenderManager::SetAppearance(float bgOpacity, float cornerRadius) {
    m_bgOpacity    = bgOpacity;
    m_cornerRadius = cornerRadius;
}

void RenderManager::SetBarVisible(bool visible) {
    m_barVisible = visible;
    if (m_mode != Mode::Windowed || !m_backgroundVisual) return;
    ComPtr<IDCompositionVisual3> v3;
    if (SUCCEEDED(m_backgroundVisual.As(&v3))) {
        v3->SetOpacity(visible ? 1.0f : 0.0f);
    }
    // 显隐切换需重建背景 Surface（隐藏→透明留白、显示→重新烘焙圆角条）；
    // Headless 路径无背景视觉，跳过。
    if (m_initialized) {
        CreateBackgroundSurface();
        if (m_dcDevice) m_dcDevice->Commit();
    }
}

void RenderManager::SetContentInsets(int left, int top, int right, int bottom) {
    m_offsetX = (float)left;
    m_offsetY = (float)top;
    m_winW = (int)std::ceil(m_dockWidth  + left + right);
    m_winH = (int)std::ceil(m_dockHeight + top  + bottom);
    if (m_winW <= 0) m_winW = (int)std::ceil(m_dockWidth);
    if (m_winH <= 0) m_winH = (int)std::ceil(m_dockHeight);

    // 渲染器未初始化（Initialize 之前的 ApplyPlacement）时不触碰设备资源
    if (!m_initialized) return;

    if (m_renderMode == RenderMode::GDI_Fallback) {
        // GDI 回退：画布尺寸需与扩大后的窗口一致（窗口已在 ApplyPlacement 中扩大）
        if (m_gdiHwnd) RecreateGdiCanvas();
        return;
    }

    // DComp：背景条尺寸/偏移随 Dock 朝向（竖直交换宽高）与留白变化，
    // 必须在内容偏移就绪后重建 —— 否则背景条贴错位置或尺寸不符。
    // 切换停靠边时 m_config.position 已由 SetDockPosition 更新，此处一并生效。
    if (m_mode == Mode::Windowed && m_backgroundVisual) {
        CreateBackgroundSurface();
        if (m_dcDevice) m_dcDevice->Commit();
    }
}

void RenderManager::SetDockPosition(DockPosition pos) {
    if (!m_initialized) return;

    // ═══ 核心修复（用户反馈「左右吸附仍为横向排列」）═══
    // UpdateVisualTransforms 通过 m_config.position 选择 MapDockLayout 的分支：
    //   水平（上下）→ 主轴 X；竖直（左右）→ 主轴 Y（L.x 映射到 Y 轴铺开图标）。
    // 此前该值仅在 Initialize 时设定；运行时切换停靠边后渲染器仍用旧朝向，
    // 导致竖直窗口内图标沿 X 轴铺开（横向排列）。此处同步最新朝向与 Dock 尺寸，
    // 随后 ApplyPlacement→SetContentInsets 会据此重建背景条。
    m_config.position = pos;
    m_geom = MakeGeometry(pos);   // 统一几何：运行时切换停靠边 → 重建多态几何实例
    m_geom->computeBarSize(m_config.iconCount, m_config.baseIconSize, m_config.iconSpacing,
                           m_config.dockPadding, m_dockWidth, m_dockHeight);
}

void RenderManager::SetTooltipEnabled(bool enabled) {
    m_tooltipEnabled = enabled;
}

void RenderManager::SetShadowEnabled(bool enabled) {
    m_shadowEnabled = enabled;
}

bool RenderManager::MeasureText(const std::wstring& text, float& w, float& h) const {
    if (!m_dwriteFactory || !m_tooltipFormat) return false;
    constexpr float kMaxTipW = 240.0f;   // 限制最大宽度，超长名称自动换行（#3：避免 tooltip 过宽凌乱）
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_dwriteFactory->CreateTextLayout(
        text.c_str(), (UINT32)text.size(), m_tooltipFormat.Get(),
        kMaxTipW, 200.0f, &layout);
    if (FAILED(hr) || !layout) return false;
    if (SUCCEEDED(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP))) {
        // 宽度已被约束为 kMaxTipW；换行后实际宽度取度量值（<=kMaxTipW）
    }
    DWRITE_TEXT_METRICS m = {};
    if (FAILED(layout->GetMetrics(&m))) return false;
    w = std::min(m.width, kMaxTipW) + 16.0f;   // 左右 padding
    h = m.height + 10.0f;                       // 上下 padding
    return true;
}

HRESULT RenderManager::CreateTooltipSurface(const std::wstring& text) {
    if (!m_tooltipVisual || !m_d2dContext || !m_dcDevice) return E_FAIL;

    float tw = m_tooltipSizeW, th = m_tooltipSizeH;
    UINT w = static_cast<UINT>(std::max(1.0f, tw));
    UINT h = static_cast<UINT>(std::max(1.0f, th));

    ComPtr<IDCompositionSurface> surface;
    DOCK_HR_CHECK(m_dcDevice->CreateSurface(w, h,
                      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                      &surface),
                  "CreateSurface tooltip");

    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {};
    DOCK_HR_CHECK(surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset),
                  "Surface BeginDraw tooltip");

    ComPtr<ID2D1Bitmap1> targetBitmap;
    HRESULT hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), nullptr,
                                                          &targetBitmap);
    if (SUCCEEDED(hr)) {
        m_d2dContext->SetTarget(targetBitmap.Get());
        m_d2dContext->BeginDraw();
        // 【图集安全】同 CreateIconSurface / CreateBackgroundSurface：先裁剪到本 Surface 的
        // 图集子矩形再 Clear，且全部绘制坐标平移 offset。tooltip 每次悬停都会重建 Surface，
        // 是图集内分配最频繁的一块 —— 无裁剪 Clear 会在每次悬停时把邻居 Surface 抹掉。
        const float tx = static_cast<float>(offset.x);
        const float ty = static_cast<float>(offset.y);
        m_d2dContext->PushAxisAlignedClip(
            D2D1::RectF(tx, ty, tx + (float)w, ty + (float)h),
            D2D1_ANTIALIAS_MODE_ALIASED);
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));

        ComPtr<ID2D1SolidColorBrush> bg;
        if (SUCCEEDED(m_d2dContext->CreateSolidColorBrush(
                D2D1::ColorF(0.117f, 0.117f, 0.117f, 0.85f), &bg))) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                D2D1::RectF(tx, ty, tx + tw, ty + th), 4.0f, 4.0f);
            m_d2dContext->FillRoundedRectangle(&rr, bg.Get());
        }
        ComPtr<IDWriteTextLayout> tl;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                text.c_str(), (UINT32)text.size(), m_tooltipFormat.Get(), tw, th, &tl)) && tl) {
            tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);   // #3：长名称换行，避免凌乱
            ComPtr<ID2D1SolidColorBrush> tb;
            if (SUCCEEDED(m_d2dContext->CreateSolidColorBrush(
                    D2D1::ColorF(1, 1, 1, 1), &tb))) {
                m_d2dContext->DrawTextLayout(D2D1::Point2F(tx, ty), tl.Get(), tb.Get());
            }
        }
        m_d2dContext->PopAxisAlignedClip();
        hr = m_d2dContext->EndDraw();
        m_d2dContext->SetTarget(nullptr);
    }
    surface->EndDraw();
    DOCK_HR_CHECK(hr, "Tooltip surface draw");

    return m_tooltipVisual->SetContent(surface.Get());
}

void RenderManager::UpdateTooltip(int hoveredIndex, const std::vector<IconLayout>& layouts,
                                  const std::vector<std::wstring>& names, float dt) {
    bool show = m_tooltipEnabled && hoveredIndex >= 0 &&
                hoveredIndex < (int)layouts.size() &&
                hoveredIndex < (int)names.size();

    // 150ms 淡入淡出
    float rate = (dt > 0.0f) ? dt / 0.15f : 1.0f;
    if (show) m_tooltipAnim = std::min(1.0f, m_tooltipAnim + rate);
    else      m_tooltipAnim = std::max(0.0f, m_tooltipAnim - rate);

    // 重置 headless 绘制状态
    m_tooltipDrawOpacity = 0.0f;
    m_tooltipDrawText.clear();

    if (!show || m_tooltipAnim <= 0.01f) {
        if (m_mode == Mode::Windowed && m_tooltipVisual) {
            ComPtr<IDCompositionVisual3> v3;
            if (SUCCEEDED(m_tooltipVisual.As(&v3))) v3->SetOpacity(0.0f);
        }
        return;
    }

    const IconLayout& L = layouts[hoveredIndex];
    std::wstring text = names[hoveredIndex];

    // 文本变化 → 重新度量（必要时烘焙表面）
    bool textChanged = (text != m_tooltipText);
    if (textChanged || m_tooltipSizeW <= 0.0f) {
        float tw = 0, th = 0;
        if (MeasureText(text, tw, th)) { m_tooltipSizeW = tw; m_tooltipSizeH = th; }
        else { m_tooltipSizeW = 80.0f; m_tooltipSizeH = 24.0f; }
    }
    if (m_mode == Mode::Windowed && m_tooltipVisual && textChanged) {
        CreateTooltipSurface(text);
    }
    m_tooltipText = text;

    float tw = m_tooltipSizeW, th = m_tooltipSizeH;
    float halfScaled  = m_config.baseIconSize * L.scale * 0.5f;

    // 图标中心（与 UpdateVisualTransforms 一致，含 Step12 留白偏移）
    float dcx = 0.0f, dcy = 0.0f;
    m_geom->mapLayout(L.x, L.y, m_dockWidth, m_dockHeight,
                      m_config.baseIconSize, m_config.dockPadding, dcx, dcy);
    float cx = m_offsetX + dcx;
    float cy = m_offsetY + dcy;
    if (m_mode != Mode::Windowed) {   // Headless 验证：映射到 Dock 局部再居中（支持竖直朝向）
        cx = HEADLESS_W * 0.5f + (dcx - m_dockWidth * 0.5f);
        cy = HEADLESS_H * 0.5f + (dcy - m_dockHeight * 0.5f);
    }

    float slide = (1.0f - m_tooltipAnim) * 8.0f;   // 滑入位移
    float gap   = 6.0f;
    float tx = 0.0f, ty = 0.0f;
    if (!m_geom->isVertical()) {
        tx = cx - tw * 0.5f;                        // 水平：Tooltip 水平居中
        if (m_config.position == DockPosition::Top)
            ty = cy + halfScaled + gap + slide;     // 顶部 Dock：Tooltip 在下
        else
            ty = cy - halfScaled - gap - th - slide;// 底部 Dock：Tooltip 在上
    } else {
        ty = cy - th * 0.5f;                        // 竖直：Tooltip 垂直居中
        if (m_config.position == DockPosition::Left)
            tx = cx + halfScaled + gap + slide;     // 左侧 Dock：Tooltip 在右
        else
            tx = cx - halfScaled - gap - tw - slide;// 右侧 Dock：Tooltip 在左
    }

    if (m_mode == Mode::Windowed && m_tooltipVisual) {
        m_tooltipVisual->SetTransform(D2D1::Matrix3x2F::Translation(tx, ty));
        ComPtr<IDCompositionVisual3> v3;
        if (SUCCEEDED(m_tooltipVisual.As(&v3))) v3->SetOpacity(m_tooltipAnim);
    }

    // Headless 绘制状态
    m_tooltipDrawRect   = D2D1::RectF(tx, ty, tx + tw, ty + th);
    m_tooltipDrawText   = text;
    m_tooltipDrawOpacity = m_tooltipAnim;
}

void RenderManager::CommitFrame() {
    LARGE_INTEGER pfFreq, pfA, pfB;
    QueryPerformanceFrequency(&pfFreq);
    QueryPerformanceCounter(&pfA);

    if (m_renderMode == RenderMode::GDI_Fallback) {
        DrawGdiFrame();          // 软件全量重绘 DIB
        CommitGdiFrame();        // Windowed：UpdateLayeredWindow 呈现
    } else if (m_mode == Mode::Windowed) {
        if (m_dcDevice) m_dcDevice->Commit();
    } else {
        DrawHeadlessFrame();
    }

    QueryPerformanceCounter(&pfB);
    m_perfRenderUs += (double)(pfB.QuadPart - pfA.QuadPart) * 1e6 / (double)pfFreq.QuadPart;
}

void RenderManager::ResetPerfRenderUs() {
    m_perfRenderUs = 0.0;
}

// ═══════════════════════════════════════════════════════════
// Headless：全量离屏绘制（模拟合成结果）
// P2-1 说明：本函数逐图标/逐像素遍历，属【非热路径】——仅 Headless 验证与 GDI 兜底绘制使用，
// 生产路径走 DComp 零重绘 + 收敛即停（空闲 CPU 0%），不参与每帧合成，故不做逐像素循环优化。
// ═══════════════════════════════════════════════════════════
void RenderManager::DrawHeadlessFrame() {
    if (!m_headlessTarget) return;

    m_d2dContext->SetTarget(m_headlessTarget.Get());
    m_d2dContext->BeginDraw();

    // 背景：深灰
    m_d2dContext->Clear(D2D1::ColorF(0.125f, 0.125f, 0.125f, 1.0f));

    // 图标：单一真源 iconVisualRect（ADR §1.5）→ Dock 局部矩形，再居中到画布
    const IconGeometryParams gpHl{ m_dockWidth, m_dockHeight,
                                   m_config.baseIconSize, m_config.dockPadding };
    for (const IconLayout& L : m_lastLayouts) {
        const RectF vis = m_geom->iconVisualRect(L, gpHl);
        float half = vis.w() * 0.5f;
        float cx = HEADLESS_W * 0.5f + (vis.cx() - m_dockWidth * 0.5f);
        float cy = HEADLESS_H * 0.5f + (vis.cy() - m_dockHeight * 0.5f);
        D2D1_RECT_F dest = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);

        size_t bmpIndex = 0;  // 单纹理复用；多纹理时按索引取
        if (m_iconBitmaps.empty()) continue;
        if (m_lastLayouts.size() == m_iconBitmaps.size()) {
            bmpIndex = static_cast<size_t>(&L - m_lastLayouts.data());
        }
        m_d2dContext->DrawBitmap(m_iconBitmaps[bmpIndex].Get(), dest, L.opacity,
                                 D2D1_INTERPOLATION_MODE_LINEAR);
    }

    // Tooltip（Headless 验证用：淡入文本标签）
    if (m_tooltipDrawOpacity > 0.01f && !m_tooltipDrawText.empty()) {
        float tw = m_tooltipDrawRect.right - m_tooltipDrawRect.left;
        float th = m_tooltipDrawRect.bottom - m_tooltipDrawRect.top;
        ComPtr<ID2D1SolidColorBrush> bg;
        if (SUCCEEDED(m_d2dContext->CreateSolidColorBrush(
                D2D1::ColorF(0.117f, 0.117f, 0.117f, m_tooltipDrawOpacity * 0.85f), &bg))) {
            m_d2dContext->FillRoundedRectangle(
                D2D1::RoundedRect(m_tooltipDrawRect, 4.0f, 4.0f), bg.Get());
        }
        ComPtr<IDWriteTextLayout> tl;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                m_tooltipDrawText.c_str(), (UINT32)m_tooltipDrawText.size(),
                m_tooltipFormat.Get(), tw, th, &tl)) && tl) {
            tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);   // #3：长名称换行，避免凌乱
            ComPtr<ID2D1SolidColorBrush> tb;
            if (SUCCEEDED(m_d2dContext->CreateSolidColorBrush(
                    D2D1::ColorF(1, 1, 1, m_tooltipDrawOpacity), &tb))) {
                m_d2dContext->DrawTextLayout(
                    D2D1::Point2F(m_tooltipDrawRect.left, m_tooltipDrawRect.top),
                    tl.Get(), tb.Get());
            }
        }
    }

    HRESULT hr = m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);
    if (FAILED(hr)) {
        return;
    }

    // 回读到 CPU 位图
    D2D1_POINT_2U dst = { 0, 0 };
    D2D1_RECT_U   src = { 0, 0, HEADLESS_W, HEADLESS_H };
    m_readbackBitmap->CopyFromBitmap(&dst, m_headlessTarget.Get(), &src);
}

HRESULT RenderManager::ReadPixel(int x, int y, uint32_t* outColor) {
    if (!m_readbackBitmap || !outColor) return E_POINTER;
    if (x < 0 || y < 0 || x >= (int)HEADLESS_W || y >= (int)HEADLESS_H) return E_INVALIDARG;

    D2D1_MAPPED_RECT mapped = {};
    DOCK_HR_CHECK(m_readbackBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped), "Map readback");

    const uint8_t* row = mapped.bits + (size_t)y * mapped.pitch + (size_t)x * 4;
    // BGRA 字节序 → 0xAARRGGBB
    *outColor = (uint32_t(row[3]) << 24) | (uint32_t(row[2]) << 16)
              | (uint32_t(row[1]) << 8)  |  uint32_t(row[0]);

    m_readbackBitmap->Unmap();
    return S_OK;
}

HRESULT RenderManager::CaptureFrameToFile(const std::wstring& path) {
    if (!m_readbackBitmap) return E_FAIL;

    D2D1_MAPPED_RECT mapped = {};
    DOCK_HR_CHECK(m_readbackBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped), "Map readback");

    const UINT w = HEADLESS_W, h = HEADLESS_H;
    const UINT rowBytes = w * 4;
    const UINT imgBytes = rowBytes * h;

    // BMP 头（32bpp, top-down 用负高度）
#pragma pack(push, 1)
    struct { uint16_t bfType; uint32_t bfSize; uint16_t r1, r2; uint32_t bfOffBits; } fileHdr
        = { 0x4D42, 54 + imgBytes, 0, 0, 54 };
    struct { uint32_t size; int32_t w, h; uint16_t planes, bpp; uint32_t comp, imgSize;
             int32_t xppm, yppm; uint32_t clrUsed, clrImp; } infoHdr
        = { 40, (int32_t)w, -(int32_t)h, 1, 32, 0, imgBytes, 2835, 2835, 0, 0 };
#pragma pack(pop)

    FILE* fp = nullptr;
    _wfopen_s(&fp, path.c_str(), L"wb");
    if (!fp) { m_readbackBitmap->Unmap(); return E_FAIL; }

    fwrite(&fileHdr, sizeof(fileHdr), 1, fp);
    fwrite(&infoHdr, sizeof(infoHdr), 1, fp);
    for (UINT yy = 0; yy < h; ++yy) {
        fwrite(mapped.bits + (size_t)yy * mapped.pitch, 1, rowBytes, fp);
    }
    fclose(fp);
    m_readbackBitmap->Unmap();
    return S_OK;
}

// ═══════════════════════════════════════════════════════════
// GDI 回退（Step 9，详细设计 §6.1 降级策略）
//   DComp/D2D 不可用时：WIC 软件解码 + DIB 软件合成 + UpdateLayeredWindow
//   Acrylic 不可用 → 纯色半透明背景（本路径天然如此）
// ═══════════════════════════════════════════════════════════
HRESULT RenderManager::InitializeGDI(HWND hwnd) {
    m_gdiHwnd = hwnd;

    // 画布尺寸：Windowed = 窗口客户区；Headless = 离屏固定画布
    int w = (int)HEADLESS_W, h = (int)HEADLESS_H;
    if (m_mode == Mode::Windowed && hwnd) {
        RECT rc = {};
        if (GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            w = rc.right; h = rc.bottom;
        } else {
            w = (int)std::ceil(m_dockWidth);
            h = (int)std::ceil(m_dockHeight);
        }
        // UpdateLayeredWindow 与 WS_EX_NOREDIRECTIONBITMAP 不兼容 → 移除；确保 WS_EX_LAYERED
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex &= ~((LONG_PTR)WS_EX_NOREDIRECTIONBITMAP);
        ex |=  (LONG_PTR)WS_EX_LAYERED;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    }
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    // 32bpp 预乘 BGRA top-down DIB Section
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;    // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    m_gdiDc  = CreateCompatibleDC(screenDc);
    void* bits = nullptr;
    m_gdiBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (!m_gdiDc || !m_gdiBmp || !bits) {
        ReleaseGdiResources();
        return E_FAIL;
    }
    m_gdiOldBmp = (HBITMAP)SelectObject(m_gdiDc, m_gdiBmp);
    m_gdiBits   = static_cast<BYTE*>(bits);
    m_gdiW = w; m_gdiH = h;
    memset(m_gdiBits, 0, (size_t)w * h * 4);

    // GDI-only 路径可能未走 CreateDeviceResources → 独立创建 WIC 工厂（图标解码必需）
    if (!m_wicFactory) {
        DOCK_HR_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory)),
                      "CoCreateInstance WICImagingFactory (GDI)");
    }
    m_dpiScale = static_cast<float>(GetDpiForSystem()) / 96.0f;

    return S_OK;
}

// Step 12：按当前（已因留白扩大的）窗口尺寸重建 GDI 画布。
// 释放旧 DIB 并依据窗口客户区创建新的 32bpp 预乘 BGRA DIB Section。
void RenderManager::RecreateGdiCanvas() {
    if (!m_gdiHwnd) return;

    int w = m_winW > 0 ? m_winW : (int)std::ceil(m_dockWidth);
    int h = m_winH > 0 ? m_winH : (int)std::ceil(m_dockHeight);
    if (m_gdiHwnd) {
        RECT rc = {};
        if (GetClientRect(m_gdiHwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            w = rc.right; h = rc.bottom;   // 以实际窗口客户区为准（含留白）
        }
    }
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    // 释放旧画布（保留 DC，仅换 DIB）
    if (m_gdiOldBmp) { SelectObject(m_gdiDc, m_gdiOldBmp); m_gdiOldBmp = nullptr; }
    if (m_gdiBmp)   { DeleteObject(m_gdiBmp); m_gdiBmp = nullptr; }
    m_gdiBits = nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    if (!m_gdiDc) m_gdiDc = CreateCompatibleDC(screenDc);
    void* bits = nullptr;
    m_gdiBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (!m_gdiBmp || !bits) {
        return;
    }
    m_gdiOldBmp = (HBITMAP)SelectObject(m_gdiDc, m_gdiBmp);
    m_gdiBits   = static_cast<BYTE*>(bits);
    m_gdiW = w; m_gdiH = h;
    memset(m_gdiBits, 0, (size_t)w * h * 4);
}

// WIC 解码 → 32bpp 预乘 BGRA DIB（软件缩放在 DrawGdiFrame 中按帧采样完成）
RenderManager::GdiIcon RenderManager::DecodeToGdiIcon(const std::wstring& path) {
    IconProvider::IconImage img;
    img.filePath = path;
    return DecodeToGdiIcon(img);
}

RenderManager::GdiIcon RenderManager::DecodeToGdiIcon(const IconProvider::IconImage& img) {
    GdiIcon gi;
    if (!m_wicFactory) return gi;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = E_FAIL;
    if (!img.pngBytes.empty()) {
        // 内存流 + CacheOnLoad：不落盘、不持文件流（诊断报告行动 2）
        ComPtr<IWICStream> stream = MakeMemStream(m_wicFactory.Get(), img.pngBytes);
        if (!stream) return gi;
        hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                   WICDecodeMetadataCacheOnLoad, &decoder);
    } else if (!img.filePath.empty()) {
        hr = m_wicFactory->CreateDecoderFromFilename(
            img.filePath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder);
    } else {
        return gi;
    }
    if (FAILED(hr) || !decoder) return gi;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return gi;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(m_wicFactory->CreateFormatConverter(&converter))) return gi;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) return gi;
    UINT iw = 0, ih = 0;
    if (FAILED(converter->GetSize(&iw, &ih)) || iw == 0 || ih == 0) return gi;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)iw;
    bmi.bmiHeader.biHeight      = -(LONG)ih;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return gi; }
    UINT stride = iw * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, stride * ih,
                                     static_cast<BYTE*>(bits)))) {
        DeleteObject(bmp);
        return gi;
    }
    gi.bmp = bmp; gi.bits = static_cast<BYTE*>(bits);
    gi.w = (int)iw; gi.h = (int)ih; gi.stride = (int)stride;
    return gi;
}

HRESULT RenderManager::LoadIconBitmapsGDI(const std::vector<IconProvider::IconImage>& imgs) {
    for (auto& gi : m_gdiIcons) if (gi.bmp) DeleteObject(gi.bmp);
    m_gdiIcons.clear();
    if (!m_wicFactory) return E_NOT_VALID_STATE;

    for (const auto& img : imgs) {
        // 解码失败 → 回退真实系统默认文件图标 DIB（永不灰占位）
        GdiIcon gi = DecodeToGdiIcon(img);
        if (!gi.bmp) {
            gi = CreateDefaultGdiIcon();
        }
        if (!gi.bmp) continue;
        m_gdiIcons.push_back(gi);
    }

    return S_OK;
}

RenderManager::GdiIcon RenderManager::CreateDefaultGdiIcon() {
    // 真实系统「通用文件」图标 DIB（非灰、非透明），作为 GDI 解码彻底失败时的兜底。
    GdiIcon gi;
    SHFILEINFOW sfi = {};
    if (!SHGetFileInfoW(L"__openDock_default_file__", FILE_ATTRIBUTE_NORMAL, &sfi,
                        sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)
        || !sfi.hIcon) {
        return gi;
    }
    HICON hIcon = sfi.hIcon;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) { DestroyIcon(hIcon); return gi; }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 256; bmi.bmiHeader.biHeight = -256;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(hdc, bmp);
        BitBlt(hdc, 0, 0, 256, 256, nullptr, 0, 0, BLACKNESS);
        DrawIconEx(hdc, 0, 0, hIcon, 256, 256, 0, nullptr, DI_NORMAL);
        SelectObject(hdc, old);
        gi.bmp = bmp; gi.bits = static_cast<BYTE*>(bits);
        gi.w = 256; gi.h = 256; gi.stride = 256 * 4;
    } else if (bmp) { DeleteObject(bmp); }
    DeleteDC(hdc);
    DestroyIcon(hIcon);
    return gi;
}

// 预乘 SrcOver：dst = src*mul + dst*(1 - srcA*mul/255)
static inline void GdiBlendPixel(BYTE* dst, const BYTE* src, float mul) {
    float sa = src[3] * mul;
    float inv = 1.0f - sa / 255.0f;
    for (int c = 0; c < 4; ++c) {
        float v = src[c] * mul + dst[c] * inv;
        dst[c] = (BYTE)(v < 0.0f ? 0 : (v > 255.0f ? 255 : v + 0.5f));
    }
}

// P2-1 说明：GDI 软件兜底绘制（--force-gdi / 无 DComp 环境），逐像素清屏 + 逐图标 AlphaBlend，
// 属【非热路径】，不参与生产 DComp 合成；可读性优先，不投入逐像素循环优化。
void RenderManager::DrawGdiFrame() {
    if (!m_gdiBits) return;
    const int W = m_gdiW, H = m_gdiH;
    BYTE* canvas = m_gdiBits;

    // 1) 清屏：Headless = 深灰不透明（与 DrawHeadlessFrame 一致，0x20）；Windowed = 全透明
    if (m_mode == Mode::Headless) {
        for (int y = 0; y < H; ++y) {
            BYTE* row = canvas + (size_t)y * W * 4;
            for (int x = 0; x < W; ++x) {
                row[x * 4 + 0] = 0x20; row[x * 4 + 1] = 0x20;
                row[x * 4 + 2] = 0x20; row[x * 4 + 3] = 0xFF;
            }
        }
    } else {
        memset(canvas, 0, (size_t)W * H * 4);

        // 2) 背景条：需求改为完全透明 —— 即便 dockBarVisible=true 也强制 bgA=0，
        //    显示区不绘制任何可见底色（GDI 降级路径与 DComp 路径保持一致）。
        float bgA = 0.0f;
        if (m_barVisible && bgA > 0.003f) {
            int bx = (int)std::ceil(m_offsetX);
            int by = (int)std::ceil(m_offsetY);
            int bw = std::min(W - bx, (int)std::ceil(m_dockWidth));
            int bh = std::min(H - by, (int)std::ceil(m_dockHeight));
            float r  = std::min(m_cornerRadius, std::min(m_dockWidth, m_dockHeight) * 0.5f);
            // 预乘颜色（0.16,0.16,0.18）* alpha
            BYTE pb = (BYTE)(0.18f * 255.0f * bgA + 0.5f);
            BYTE pg = (BYTE)(0.16f * 255.0f * bgA + 0.5f);
            BYTE pr = (BYTE)(0.16f * 255.0f * bgA + 0.5f);
            BYTE pa = (BYTE)(255.0f * bgA + 0.5f);
            for (int y = 0; y < bh; ++y) {
                int py = by + y;
                if (py < 0 || py >= H) continue;
                BYTE* row = canvas + (size_t)py * W * 4;
                for (int x = 0; x < bw; ++x) {
                    int px = bx + x;
                    if (px < 0 || px >= W) continue;
                    // 圆角测试：四角圆外像素跳过
                    float dx = 0, dy = 0;
                    if (x < r && y < r)                { dx = r - x; dy = r - y; }
                    else if (x >= bw - r && y < r)     { dx = x - (bw - 1 - r); dy = r - y; }
                    else if (x < r && y >= bh - r)     { dx = r - x; dy = y - (bh - 1 - r); }
                    else if (x >= bw - r && y >= bh - r) { dx = x - (bw - 1 - r); dy = y - (bh - 1 - r); }
                    if (dx > 0 && dy > 0 && dx * dx + dy * dy > r * r) continue;
                    row[px * 4 + 0] = pb; row[px * 4 + 1] = pg;
                    row[px * 4 + 2] = pr; row[px * 4 + 3] = pa;
                }
            }
        }
    }

    // 3) 图标：软件最近邻缩放 + 预乘 SrcOver（WIC 解码位图 → DIB 画布）
    const IconGeometryParams gpGdi{ m_dockWidth, m_dockHeight,
                                    m_config.baseIconSize, m_config.dockPadding };
    for (size_t i = 0; i < m_lastLayouts.size(); ++i) {
        if (m_gdiIcons.empty()) break;
        const IconLayout& L = m_lastLayouts[i];
        const GdiIcon& icon = m_gdiIcons[m_lastLayouts.size() == m_gdiIcons.size()
                                             ? i : 0];
        if (!icon.bits || L.opacity <= 0.003f) continue;

        // 图标目标矩形（与 DComp 路径同一真源：ADR §1.5 iconVisualRect）
        const RectF vis = m_geom->iconVisualRect(L, gpGdi);
        float cx, cy;
        if (m_mode == Mode::Windowed) {
            cx = m_offsetX + vis.cx();
            cy = m_offsetY + vis.cy();
        } else {
            // Headless：把 Dock 局部矩形中心平移到离屏画布中心
            cx = HEADLESS_W * 0.5f + (vis.cx() - m_dockWidth * 0.5f);
            cy = HEADLESS_H * 0.5f + (vis.cy() - m_dockHeight * 0.5f);
        }
        float halfDst = vis.w() * 0.5f;
        int x0 = (int)std::floor(cx - halfDst), x1 = (int)std::ceil(cx + halfDst);
        int y0 = (int)std::floor(cy - halfDst), y1 = (int)std::ceil(cy + halfDst);
        int cx0 = std::max(0, x0), cx1 = std::min(W, x1);
        int cy0 = std::max(0, y0), cy1 = std::min(H, y1);
        float dstW = (float)(x1 - x0), dstH = (float)(y1 - y0);
        if (dstW <= 0 || dstH <= 0) continue;

        for (int y = cy0; y < cy1; ++y) {
            BYTE* row = canvas + (size_t)y * W * 4;
            int sy = (int)((y - y0) / dstH * icon.h);
            sy = std::min(icon.h - 1, std::max(0, sy));
            const BYTE* srow = icon.bits + (size_t)sy * icon.stride;
            for (int x = cx0; x < cx1; ++x) {
                int sx = (int)((x - x0) / dstW * icon.w);
                sx = std::min(icon.w - 1, std::max(0, sx));
                GdiBlendPixel(row + (size_t)x * 4, srow + (size_t)sx * 4, L.opacity);
            }
        }
    }
}

void RenderManager::CommitGdiFrame() {
    if (m_mode != Mode::Windowed || !m_gdiHwnd || !m_gdiDc) return;

    // UpdateLayeredWindow：预乘 DIB → 分层窗口（保持现有窗口位置与尺寸）
    SIZE  size  = { m_gdiW, m_gdiH };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!UpdateLayeredWindow(m_gdiHwnd, nullptr, nullptr, &size,
                             m_gdiDc, &ptSrc, 0, &bf, ULW_ALPHA)) {
    }
}

HRESULT RenderManager::ReadPixelGDI(int x, int y, uint32_t* outColor) {
    if (!m_gdiBits || !outColor) return E_POINTER;
    if (x < 0 || y < 0 || x >= m_gdiW || y >= m_gdiH) return E_INVALIDARG;
    const BYTE* p = m_gdiBits + (size_t)y * m_gdiW * 4 + (size_t)x * 4;
    // BGRA → 0xAARRGGBB
    *outColor = (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16)
              | (uint32_t(p[1]) << 8)  |  uint32_t(p[0]);
    return S_OK;
}

void RenderManager::ReleaseGdiResources() {
    for (auto& gi : m_gdiIcons) if (gi.bmp) DeleteObject(gi.bmp);
    m_gdiIcons.clear();
    if (m_gdiDc && m_gdiOldBmp) SelectObject(m_gdiDc, m_gdiOldBmp);
    if (m_gdiBmp) { DeleteObject(m_gdiBmp); m_gdiBmp = nullptr; }
    if (m_gdiDc)  { DeleteDC(m_gdiDc);      m_gdiDc  = nullptr; }
    m_gdiOldBmp = nullptr;
    m_gdiBits = nullptr;
    m_gdiW = m_gdiH = 0;
    m_gdiHwnd = nullptr;
}
