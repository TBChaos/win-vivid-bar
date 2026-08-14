// src/render/RenderManager.h
// 渲染管理器 — DirectComposition + Direct2D
// 双模式设计（详细执行流程 §3.2）：
//   Windowed  → 绑定 HWND，DComp Visual 树，零重绘动画
//   Headless  → 沙盒离屏渲染，像素采样验证（无窗口）
// 设计参考：详细设计说明 §2.4
#pragma once
#include "../Common.h"
#include "../core/LayoutEngine.h"
#include "../core/EdgeGeometry.h"   // 统一四边几何（IEdgeGeometry / MakeGeometry）
#include "../app/IconProvider.h"    // IconProvider::IconImage（内存 PNG 字节 / 可选路径）
#include <d2d1.h>
#include <dwrite.h>
#include <unordered_map>
#include <vector>

class RenderManager {
public:
    enum class Mode {
        Windowed,    // 正常模式：绑定到 HWND
        Headless     // 沙盒模式：离屏渲染，不创建窗口
    };

    // 实际渲染后端（详细设计 §6.1 降级策略）：DComp 不可用时回退 GDI
    enum class RenderMode {
        DirectComposition,   // 默认：DComp + D2D
        GDI_Fallback         // 回退：GDI + Layered Window (UpdateLayeredWindow)
    };

    // Headless 离屏画布尺寸
    static constexpr UINT HEADLESS_W = 256;
    static constexpr UINT HEADLESS_H = 256;

    HRESULT Initialize(Mode mode, HWND hwnd, const DockConfig& config);
    void Shutdown();
    ~RenderManager();   // 防御性释放设备级 COM 资源

    // ═══ 性能验收（Step 11）═══
    void  ResetPerfRenderUs();        // 清零提交耗时累计
    double GetPerfRenderUs() const { return m_perfRenderUs; }

    // 图标纹理管理（WIC 解码 → D2D Bitmap）
    // 生产路径：IconImage（优先内存 PNG 字节，不落盘）
    HRESULT LoadIconTextures(const std::vector<IconProvider::IconImage>& imgs);
    // 兼容路径：纯文件路径（GDI/渲染单测用），内部包装为 IconImage{ .filePath = p }
    HRESULT LoadIconTextures(const std::vector<std::wstring>& iconPaths);
    // 便利重载：字面量列表 { L"a.png", L"b.png" }（GDI/渲染单测）。
    // 必须显式声明：否则 { L"a", L"b" } 对 vector<IconImage> 的【迭代器对构造函数】
    // (const wchar_t*, const wchar_t*) 也「可行」，与 vector<wstring> 重载二义（C2668）。
    // 本重载对字面量列表是恒等转换，严格优于两个 vector 重载的用户自定义转换，故唯一。
    HRESULT LoadIconTextures(std::initializer_list<const wchar_t*> iconPaths);

    // 动态重建图标集（Step 8：删除/重排/添加后）—— 重建视觉树与背景尺寸
    HRESULT RebuildIconSet(const std::vector<IconProvider::IconImage>& imgs);

    // 拖拽过程轻量重排：仅按 path 顺序复用【已解码】位图重排视觉树与背景，
    // 不重新解码图标、不重定位窗口、不重建背景条 —— 用于拖拽过程中顺序改变时，
    // 避免整组纹理重载 / 窗口重定位导致的原有图标闪烁（外部拖入尤为明显）。
    void RelayoutIcons(const std::vector<std::wstring>& orderedPaths);
    // 记录当前已加载位图对应的图标路径顺序（轻量重排复用，不解码）；RebuildIcons/Initialize 时调用。
    void SetIconRenderPaths(const std::vector<std::wstring>& paths);

    // 动画更新
    //   Windowed: 仅更新 Visual Transform/Opacity（零重绘路径）
    //   Headless: 记录布局，待 CommitFrame 时离屏绘制
    void UpdateVisualTransforms(const std::vector<IconLayout>& layouts);

    // 背景更新（仅在 Dock 显示/隐藏时调用）
    void UpdateBackground(float opacity);

    // 外观参数（背景不透明度 / 圆角半径），须在 Initialize 前设置以烘焙背景
    void SetAppearance(float bgOpacity, float cornerRadius);
    // Dock 底座背景条是否显示（#N：默认隐藏，仅浮出图标；关闭时不绘制/不呈现底座）
    void SetBarVisible(bool visible);

    // 悬停 Tooltip（详细设计 §2.6）：Switch + 每帧更新位置/透明度（150ms 淡入淡出）
    void SetTooltipEnabled(bool enabled);
    void UpdateTooltip(int hoveredIndex, const std::vector<IconLayout>& layouts,
                       const std::vector<std::wstring>& names, float dt);

    // 图标阴影（详细设计 §2.4.7）：IDCompositionShadowEffect，由 shadowEnabled 开关
    void SetShadowEnabled(bool enabled);

    // 提交帧（Windowed: DComp Commit；Headless: 离屏重绘）
    void CommitFrame();

    // ═══ Step 12：放大溢出留白（内容相对窗口的偏移）═══
    // 须在 Initialize 之后调用（依赖 m_dockWidth/Height）。lb/tr/rb 为四边留白（px）。
    void SetContentInsets(int left, int top, int right, int bottom);

    // Step 14：运行时切换停靠边（上下左右）—— 同步渲染器朝向与 Dock 条尺寸，
    // 后续由 SetContentInsets 按新偏移重建背景条。须在 DockEngine::SetDockPosition 中调用。
    void SetDockPosition(DockPosition pos);

    // ═══ Headless 专用 ═══
    HRESULT ReadPixel(int x, int y, uint32_t* outColor);          // BGRA -> 0xAARRGGBB
    HRESULT CaptureFrameToFile(const std::wstring& path);         // 保存 BMP

    // ═══ 调试查询 ═══
    size_t GetIconBitmapCount() const {
        return m_renderMode == RenderMode::GDI_Fallback ? m_gdiIcons.size()
                                                        : m_iconBitmaps.size();
    }
    D2D1_SIZE_F GetIconBitmapSize(size_t index) const;
    Mode GetMode() const { return m_mode; }
    bool IsInitialized() const { return m_initialized; }

    // ═══ Step 14 朝向同步查询（回归测试用）═══
    DockPosition GetDockPosition() const { return m_config.position; }
    float GetDockWidth()  const { return m_dockWidth; }
    float GetDockHeight() const { return m_dockHeight; }

    // ═══ 降级 / GDI 回退（Step 9）═══
    void SetForceGdiFallback(bool b) { m_forceGdi = b; }   // 测试钩子：强制走 GDI
    RenderMode GetRenderMode() const { return m_renderMode; }
    // 从 GDI 离屏 DIB 读取像素（BGRA -> 0xAARRGGBB），仅 GDI 模式有效
    HRESULT ReadPixelGDI(int x, int y, uint32_t* outColor);

private:
    HRESULT CreateDeviceResources();                  // D3D11 + D2D + DWrite（两模式共用）
    HRESULT CreateCompositionResources(HWND hwnd);    // DComp 设备/目标/Visual 树（Windowed）
    HRESULT CreateHeadlessTarget();                   // 离屏 D2D 目标（Headless）
    HRESULT CreateIconSurface(size_t index);          // Windowed：为图标 Visual 创建内容
    HRESULT CreateBackgroundSurface();                // Windowed：为背景 Visual 烘焙圆角半透明条
    HRESULT CreateTooltipSurface(const std::wstring& text); // Windowed：为 tooltip Visual 烘焙文本
    bool    MeasureText(const std::wstring& text, float& w, float& h) const; // 文本度量
    void    DrawHeadlessFrame();                      // Headless：全量离屏绘制

    // ═══ GDI 回退（Step 9，详细设计 §6.1）═══
    HRESULT InitializeGDI(HWND hwnd);                 // 创建 32bpp 预乘 DIB 离屏画布
    HRESULT LoadIconBitmapsGDI(const std::vector<IconProvider::IconImage>& imgs); // WIC 解码 → HBITMAP
    // 解码兜底：单个图标解码失败时回退到真实系统默认文件图标（非灰、非透明），
    // 保证纹理数恒等于图标数，避免「数量错位 → 整组图标错乱/全灰」的渲染失败模式
    ComPtr<ID2D1Bitmap1> DecodeToBitmap(const std::wstring& path);
    // 内存流版：pngBytes 非空走 WIC 内存流 + CacheOnLoad（不落盘、不持文件流）
    ComPtr<ID2D1Bitmap1> DecodeToBitmap(const IconProvider::IconImage& img);
    // 真实系统「通用文件」图标（D2D 位图），作为解码彻底失败时的兜底（永不灰占位）
    ComPtr<ID2D1Bitmap1> CreateDefaultIconBitmap();
    void    DrawGdiFrame();                           // 软件绘制：背景 + 图标（AlphaBlend）
    void    CommitGdiFrame();                         // UpdateLayeredWindow 呈现（Windowed）
    void    ReleaseGdiResources();
    // Step 12：按当前窗口尺寸重建 GDI 画布（窗口因留白扩大后调用）
    void    RecreateGdiCanvas();

    Mode m_mode = Mode::Headless;
    bool m_initialized = false;
    DockConfig m_config;
    std::unique_ptr<IEdgeGeometry> m_geom;   // 统一四边几何（编译期模板 + 多态）

    // ═══ GDI 回退状态（Step 9）═══
    RenderMode m_renderMode = RenderMode::DirectComposition;
    bool     m_forceGdi = false;          // 测试钩子：跳过 DComp 强制 GDI
    HWND     m_gdiHwnd  = nullptr;        // Windowed 时呈现目标（Headless 为 null）
    HDC      m_gdiDc    = nullptr;        // 兼容内存 DC（选入 m_gdiBmp）
    HBITMAP  m_gdiBmp   = nullptr;        // 32bpp 预乘 alpha DIB Section
    HBITMAP  m_gdiOldBmp = nullptr;
    BYTE*    m_gdiBits  = nullptr;        // DIB 像素（BGRA 预乘，top-down）
    int      m_gdiW = 0, m_gdiH = 0;
    struct GdiIcon {                      // WIC 解码后的图标位图（预乘 BGRA DIB）
        HBITMAP bmp  = nullptr;
        BYTE*   bits = nullptr;           // DIB 像素（top-down，随 bmp 生命周期）
        int w = 0, h = 0, stride = 0;
    };
    std::vector<GdiIcon> m_gdiIcons;

    GdiIcon  DecodeToGdiIcon(const std::wstring& path);   // GDI 版：解码为 DIB Icon（失败返回空）
    GdiIcon  DecodeToGdiIcon(const IconProvider::IconImage& img);  // GDI 版：内存流优先
    // 真实系统「通用文件」图标（DIB），作为 GDI 解码彻底失败时的兜底（永不灰占位）
    GdiIcon  CreateDefaultGdiIcon();

    // ═══ D3D / DXGI ═══
    ComPtr<ID3D11Device>         m_d3dDevice;
    ComPtr<ID3D11DeviceContext>  m_d3dContext;
    ComPtr<IDXGIDevice>          m_dxgiDevice;

    // ═══ D2D ═══
    ComPtr<ID2D1Factory1>        m_d2dFactory;
    ComPtr<ID2D1Device>          m_d2dDevice;
    ComPtr<ID2D1DeviceContext>   m_d2dContext;
    std::vector<ComPtr<ID2D1Bitmap1>> m_iconBitmaps;

    // ═══ DirectWrite / WIC ═══
    ComPtr<IDWriteFactory>       m_dwriteFactory;
    ComPtr<IDWriteTextFormat>    m_tooltipFormat;     // Segoe UI 12px
    ComPtr<IWICImagingFactory>   m_wicFactory;

    // ═══ DirectComposition（Windowed）═══
    ComPtr<IDCompositionDevice>  m_dcDevice;
    ComPtr<IDCompositionTarget>  m_dcTarget;
    ComPtr<IDCompositionVisual>  m_rootVisual;
    ComPtr<IDCompositionVisual>  m_backgroundVisual;
    ComPtr<IDCompositionVisual>  m_tooltipVisual;    // 悬停名称标签（置于最上层）
    std::vector<ComPtr<IDCompositionVisual>> m_iconVisuals;
    std::vector<std::wstring> m_iconRenderPaths;      // 当前已加载位图对应的图标 path 顺序（轻量重排复用，不解码）
    // 图标阴影效果图（Windowed）：Shadow → AffineTransform2D(偏移) → Composite(叠原图)
    struct ShadowGraph {
        ComPtr<IDCompositionShadowEffect>            shadow;
        ComPtr<IDCompositionAffineTransform2DEffect> offset;
        ComPtr<IDCompositionCompositeEffect>         composite;
    };
    std::vector<ShadowGraph> m_iconShadows;

    // ═══ Headless 离屏资源 ═══
    ComPtr<ID2D1Bitmap1>         m_headlessTarget;    // GPU 渲染目标
    ComPtr<ID2D1Bitmap1>         m_readbackBitmap;    // CPU 可读回拷贝
    std::vector<IconLayout>      m_lastLayouts;       // 最近一次布局

    // ═══ 状态 ═══
    double m_perfRenderUs = 0.0;     // 累计提交耗时（微秒，性能验收）
    float m_backgroundOpacity = 1.0f;
    float m_bgOpacity   = 0.5f;    // 背景条不透明度（来自 config.backgroundOpacity）
    float m_cornerRadius = 16.0f;  // 背景条圆角半径（来自 config.cornerRadius）
    bool  m_barVisible  = true;    // #N Dock 底座背景条是否显示（默认 true；配置默认 false 隐藏）
    float m_dockWidth  = 0.0f;
    float m_dockHeight = 0.0f;
    float m_dpiScale   = 1.0f;
    // Step 12：内容相对窗口的绘制偏移（四边留白），放大图标/tooltip 在留白内绘制
    float m_offsetX = 0.0f;
    float m_offsetY = 0.0f;
    int   m_winW    = 0;   // 窗口/画布尺寸（基础 Dock 条 + 留白）
    int   m_winH    = 0;

    // ═══ Tooltip 状态 ═══
    bool   m_tooltipEnabled = true;
    bool   m_shadowEnabled  = true;
    float  m_tooltipAnim    = 0.0f;   // 0..1 淡入/滑入进度
    std::wstring m_tooltipText;        // 已烘焙文本（避免每帧重建表面）
    float  m_tooltipSizeW = 0.0f, m_tooltipSizeH = 0.0f;
    // Headless 绘制状态
    D2D1_RECT_F   m_tooltipDrawRect = {};
    std::wstring  m_tooltipDrawText;
    float         m_tooltipDrawOpacity = 0.0f;
};
