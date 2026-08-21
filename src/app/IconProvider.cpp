// src/app/IconProvider.cpp
#include "IconProvider.h"
#include <objbase.h>    // CreateStreamOnHGlobal / GetHGlobalFromStream（内存 PNG 编码）
#include <shellapi.h>   // SHGetFileInfo / SHFILEINFO
#include <shlobj.h>     // SHDefExtractIcon
#include <shobjidl.h>   // IShellLinkW / IPersistFile（.lnk 解析）
#include <wincodec.h>   // WIC（图标编码为 PNG）

// 注：本文件原有的诊断日志已全部移除（Release 运行期不写任何日志）。

static bool FileExistsW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool IsImageFile(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".png" || ext == L".ico" || ext == L".bmp"
        || ext == L".jpg" || ext == L".jpeg";
}

static bool IsLnkFile(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".lnk";
}

static bool IsDirectoryW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 解析 .lnk 快捷方式指向的真实目标路径（需要 COM 已初始化）。
// 成功返回 true 且 targetPath 非空；失败（.lnk 损坏/目标缺失）返回 false。
static bool ResolveShortcut(const std::wstring& lnkPath, std::wstring& targetPath) {
    targetPath.clear();
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (FAILED(hr) || !psl) return false;

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
    if (SUCCEEDED(hr) && ppf) {
        hr = ppf->Load(lnkPath.c_str(), STGM_READ);
        if (SUCCEEDED(hr)) {
            wchar_t buf[MAX_PATH] = {};
            WIN32_FIND_DATAW ffd = {};
            if (SUCCEEDED(psl->GetPath(buf, MAX_PATH, &ffd, SLGP_UNCPRIORITY))) {
                targetPath.assign(buf);
            }
        }
        ppf->Release();
    }
    psl->Release();
    return !targetPath.empty();
}

// 前向声明 —— 全部返回【内存 PNG 字节】，不落盘（消除跨引擎文件锁竞争 / %TEMP% 泄漏）。
static std::vector<uint8_t> SaveHiconToPng(HICON hIcon, int size);
static std::vector<uint8_t> SaveHbitmapToPng(HBITMAP hbmp, int size);
// 把内存 BGRA 像素（size×size，顶向下）编码为内存 PNG 字节（HICON/HBITMAP 共用）
static std::vector<uint8_t> EncodeBGRAtoPng(RGBQUAD* bits, int size);

std::vector<IconProvider::IconImage> IconProvider::LoadIcons(const AppConfig& config) {
    m_images.clear();
    m_displayNames.clear();

    for (size_t i = 0; i < config.icons.size(); ++i) {
        const IconEntry& entry = config.icons[i];
        IconImage resolved;

        // T09：.lnk 缓存键去重 —— 快捷方式的图标来自其真实目标，故缓存键用【解析后的
        // 目标路径】而非 .lnk 自身；多 .lnk 指向同一 exe / 直接放该 exe 都能复用同一份
        // 提取结果（同一份内存 PNG 字节，天然去重）。
        std::wstring effectiveKey = entry.path;
        if (IsLnkFile(entry.path)) {
            std::wstring lnkTarget;
            if (ResolveShortcut(entry.path, lnkTarget) && !lnkTarget.empty())
                effectiveKey = lnkTarget;
        }

        // 缓存命中：复用已提取的内存字节，避免重建时重复提取/重复编码。
        auto cached = m_resolvedCache.find(effectiveKey);
        if (cached != m_resolvedCache.end()
            && (!cached->second.pngBytes.empty()
                || (!cached->second.filePath.empty() && FileExistsW(cached->second.filePath)))) {
            resolved = cached->second;
        }
        else if (FileExistsW(entry.path) && IsImageFile(entry.path)) {
            // 直接可用的图像：用户自带资产，只读打开、不由本进程生成，无写竞争，
            // 故按路径引用（不复制字节），由 RenderManager 的 filePath 分支解码。
            resolved.filePath = entry.path;
        } else if (FileExistsW(entry.path) && IsLnkFile(entry.path)) {
            // .lnk 快捷方式：从（已解析的）真实目标提取图标（Step 8 添加快捷方式）
            const std::wstring& target = effectiveKey;
            if (target != entry.path && FileExistsW(target)) {
                if (FAILED(ExtractIconFromExe(target, 0, resolved)))
                    resolved = GetDefaultIconForPath(target);
            } else {
                // 解析失败（.lnk 损坏 / 目标缺失）→ 退化为从 .lnk 自身取图标
                if (FAILED(ExtractIconFromExe(entry.path, 0, resolved)))
                    resolved = GetDefaultIconForPath(entry.path);
            }
        } else if (FileExistsW(entry.path)) {
            // 注：entry.index 语义是「exe/dll 内部图标资源序号」，默认 0 = 主图标。
            // 非 0 属罕见显式用法（请求非主资源）。曾有种子配置把「Dock 排列下标」误填进
            // 此字段（notepad=0/cmd=1/explorer=2/calc=3），导致 explorer 取到非主图标资源、
            // 表现为「没有图标」。原先此处会打一条告警日志，现已随诊断日志一并移除。
            // EXE/DLL：尝试提取
            if (FAILED(ExtractIconFromExe(entry.path, entry.index, resolved))) {
                resolved = GetDefaultIconForPath(entry.path);
            }
        } else if (IsDirectoryW(entry.path)) {
            // #4/#N：取「真实文件夹图标」——尊重自定义图标/叠加/缩略图（IShellItemImageFactory）。
            // 回退 SHGetFileInfo 通用文件夹图标；仍失败则系统默认文件图标（永不灰占位）。
            bool ok = false;
            IShellItemImageFactory* pImgFac = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(entry.path.c_str(), nullptr,
                                                       IID_PPV_ARGS(&pImgFac))) && pImgFac) {
                SIZE sz = { 256, 256 };
                HBITMAP hbmp = nullptr;
                // SIIGBF_BIGGEROK=0x4：允许返回比请求更大的图标（质量更好）。
                // 部分 SDK 头未声明该枚举值，故用字面量。
                if (SUCCEEDED(pImgFac->GetImage(sz, 0x4, &hbmp)) && hbmp) {
                    // SaveHbitmapToPng 内部已 DeleteObject(hbmp)
                    auto bytes = SaveHbitmapToPng(hbmp, 256);
                    if (!bytes.empty()) { resolved.pngBytes = std::move(bytes); ok = true; }
                }
                pImgFac->Release();
            }
            if (!ok) {
                // 回退：系统通用文件夹图标（SHGetFileInfo）
                SHFILEINFOW sfi = {};
                if (SHGetFileInfoW(entry.path.c_str(), FILE_ATTRIBUTE_DIRECTORY,
                                   &sfi, sizeof(sfi),
                                   SHGFI_ICON | SHGFI_LARGEICON)) {
                    auto bytes = SaveHiconToPng(sfi.hIcon, 256);
                    if (!bytes.empty()) { resolved.pngBytes = std::move(bytes); ok = true; }
                    DestroyIcon(sfi.hIcon);
                }
            }
            if (!ok) {
                resolved = GetDefaultIconForPath(entry.path);
            }
        } else {
            resolved = GetDefaultIconForPath(entry.path);    // 错误容忍：系统默认图标（非灰）
        }

        const bool got = !resolved.pngBytes.empty() || !resolved.filePath.empty();

        // 缓存真实解析结果（键为 effectiveKey：.lnk 用解析目标，便于去重复用）
        if (got) m_resolvedCache[effectiveKey] = resolved;

        if (got) {
            m_images.push_back(std::move(resolved));
            m_displayNames.push_back(entry.name.empty() ? L"App" : entry.name);
        }
    }

    return m_images;
}

// 将 HICON 渲染到 32bpp 顶向下 DIB，再用 WIC 编码为【内存 PNG 字节】。
// 返回非空 vector 表示成功；失败返回 {}（调用方应回退默认图标）。
static std::vector<uint8_t> SaveHiconToPng(HICON hIcon, int size) {
    if (!hIcon) return {};

    // 1) 渲染 HICON → 32bpp 顶向下 DIB（内存 BGRA）
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return {};
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size;   // 负高 = 顶向下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&bits), nullptr, 0);
    if (!hbmp) { DeleteDC(hdc); return {}; }
    HGDIOBJ old = SelectObject(hdc, hbmp);
    BitBlt(hdc, 0, 0, size, size, nullptr, 0, 0, BLACKNESS);
    DrawIconEx(hdc, 0, 0, hIcon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(hdc, old);
    DeleteDC(hdc);

    // 2) 编码为内存 PNG 字节（与 SaveHbitmapToPng 共用）
    std::vector<uint8_t> out = EncodeBGRAtoPng(bits, size);
    DeleteObject(hbmp);
    return out;
}

// #4：把 HBITMAP（来自 IShellItemImageFactory::GetImage 的真实文件夹图标）编码为内存 PNG
static std::vector<uint8_t> SaveHbitmapToPng(HBITMAP hbmp, int size) {
    if (!hbmp) return {};
    HDC hdcSrc = CreateCompatibleDC(nullptr);
    if (!hdcSrc) { DeleteObject(hbmp); return {}; }
    HBITMAP hOld = (HBITMAP)SelectObject(hdcSrc, hbmp);

    // 拷到 32bpp 顶向下 DIB，交由 EncodeBGRAtoPng 统一编码
    HDC hdcDst = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP hDib = CreateDIBSection(hdcDst, &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&bits), nullptr, 0);
    if (!hDib) {
        SelectObject(hdcSrc, hOld); DeleteDC(hdcSrc); DeleteDC(hdcDst);
        DeleteObject(hbmp);
        return {};
    }
    HBITMAP hOld2 = (HBITMAP)SelectObject(hdcDst, hDib);
    BitBlt(hdcDst, 0, 0, size, size, hdcSrc, 0, 0, SRCCOPY);
    SelectObject(hdcDst, hOld2);
    SelectObject(hdcSrc, hOld);
    DeleteDC(hdcSrc); DeleteDC(hdcDst);

    std::vector<uint8_t> out = EncodeBGRAtoPng(bits, size);
    DeleteObject(hDib);
    DeleteObject(hbmp);   // GetImage 返回的位图由调用方负责释放
    return out;
}

// 把内存 BGRA 像素（size×size，顶向下）编码为【内存 PNG 字节】（HGLOBAL 内存流，绝不落盘）。
// 长期方案（诊断报告行动 2）：不再生成 %TEMP%/openDock_icons/*.png，
// 一次性消解「跨引擎文件锁竞争 + PID 假不变量 + %TEMP% 泄漏」三害。
static std::vector<uint8_t> EncodeBGRAtoPng(RGBQUAD* bits, int size) {
    if (!bits) return {};
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) return {};

    IStream* pStream = nullptr;
    // TRUE：流 Release 时自动释放其 HGLOBAL
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) || !pStream) {
        pFactory->Release();
        return {};
    }

    std::vector<uint8_t> out;
    IWICBitmapEncoder* pEncoder = nullptr;
    if (SUCCEEDED(pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder)) && pEncoder) {
        if (SUCCEEDED(pEncoder->Initialize(pStream, WICBitmapEncoderNoCache))) {
            IWICBitmapFrameEncode* pFrame = nullptr;
            if (SUCCEEDED(pEncoder->CreateNewFrame(&pFrame, nullptr)) && pFrame) {
                if (SUCCEEDED(pFrame->Initialize(nullptr)) &&
                    SUCCEEDED(pFrame->SetSize(static_cast<UINT>(size), static_cast<UINT>(size)))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                    if (SUCCEEDED(pFrame->SetPixelFormat(&fmt))) {
                        UINT stride = static_cast<UINT>(size) * 4;
                        pFrame->WritePixels(static_cast<UINT>(size), stride,
                                            stride * static_cast<UINT>(size),
                                            reinterpret_cast<BYTE*>(bits));
                        pFrame->Commit();
                    }
                }
                pFrame->Release();
            }
            pEncoder->Commit();
        }
        pEncoder->Release();
    }

    // 取回 HGLOBAL 内容为字节数组。注意用流的实际写入长度（Seek END）而非 GlobalSize：
    // HGLOBAL 容量按块增长，GlobalSize 可能大于有效 PNG 长度，多余尾部字节会让解码器
    // 读到垃圾（部分解码器容忍、部分报错），故以流位置为准截断。
    ULARGE_INTEGER used = {};
    LARGE_INTEGER  zero = {};
    if (FAILED(pStream->Seek(zero, STREAM_SEEK_END, &used))) used.QuadPart = 0;
    HGLOBAL hGlobal = nullptr;
    if (SUCCEEDED(GetHGlobalFromStream(pStream, &hGlobal)) && hGlobal) {
        SIZE_T cap = GlobalSize(hGlobal);
        SIZE_T sz  = (used.QuadPart > 0 && used.QuadPart <= cap)
                   ? static_cast<SIZE_T>(used.QuadPart) : cap;
        if (sz > 0) {
            BYTE* p = static_cast<BYTE*>(GlobalLock(hGlobal));
            if (p) { out.assign(p, p + sz); GlobalUnlock(hGlobal); }
        }
    }
    pStream->Release();   // 同时释放 hGlobal
    pFactory->Release();
    return out;
}

HRESULT IconProvider::ExtractIconFromExe(const std::wstring& exePath, int iconIndex,
                                         IconImage& outImage) {
    // Step 2：真实图标提取
    // 1) 优先用 SHDefExtractIcon 取目标索引图标（最权威，含多分辨率）
    HICON hIcon = nullptr;
    HRESULT hr = SHDefExtractIconW(exePath.c_str(), iconIndex, 0, &hIcon, nullptr, 256);
    if (FAILED(hr) || !hIcon) {
        // Bug #1 根因修复：SHDefExtractIconW 在「冷缩略图缓存 / 启动早期」可能间歇性失败
        // （返回失败或空句柄）。旧代码直接回退到 SHGetFileInfoW(SHGFI_USEFILEATTRIBUTES)，
        // 该标志忽略真实文件、只按扩展名返回通用「白页」文档图标 → 默认 exe 图标变白页。
        // 修复：文件真实存在时，优先用更可靠、不依赖缩略图缓存的路径取【真实】图标：
        //   回退A：ExtractIconExW（直接读 exe 资源，最稳）；
        //   回退B：SHGetFileInfoW(真实路径, 不带 USEFILEATTRIBUTES) 读真实文件图标；
        //   回退C：仅当文件确实【不存在】时才用 USEFILEATTRIBUTES（通用白页，作为最后兜底）。

        // 回退A：文件存在 → ExtractIconExW 取真实图标（不经过缩略图缓存，最可靠）
        if (FileExistsW(exePath)) {
            HICON hL = nullptr, hS = nullptr;
            if (ExtractIconExW(exePath.c_str(), iconIndex, &hL, &hS, 1) > 0) {
                if (hL) { if (hIcon) DestroyIcon(hIcon); hIcon = hL; hr = S_OK;
                          if (hS) DestroyIcon(hS); }
                else if (hS) { hIcon = hS; hr = S_OK; }
            }
        }
        // 回退B：文件存在 → 真实文件图标（不带 USEFILEATTRIBUTES，避免白页）
        if ((FAILED(hr) || !hIcon) && FileExistsW(exePath)) {
            SHFILEINFOW sfi = {};
            if (SHGetFileInfoW(exePath.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
                if (hIcon) DestroyIcon(hIcon);
                hIcon = sfi.hIcon; hr = S_OK;
            }
        }
        // 回退C：仅当文件确实不存在 → 才用 USEFILEATTRIBUTES（通用白页，最后兜底）
        if ((FAILED(hr) || !hIcon) && !FileExistsW(exePath)) {
            SHFILEINFOW sfi = {};
            if (SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES) && sfi.hIcon) {
                hIcon = sfi.hIcon; hr = S_OK;
            }
        }
    }

    if (hIcon) {
        std::vector<uint8_t> bytes = SaveHiconToPng(hIcon, 256);
        DestroyIcon(hIcon);
        if (!bytes.empty()) {
            outImage.pngBytes = std::move(bytes);
            outImage.filePath.clear();
            return S_OK;
        }
        // 编码失败 → 默认图标
    }

    // 任何失败路径：回退系统默认文件图标（按扩展名取真实默认图标），保证主流程不崩溃
    outImage = GetDefaultIconForPath(exePath);
    return outImage.pngBytes.empty() ? E_FAIL : S_OK;
}

std::wstring IconProvider::GetDisplayName(size_t index) const {
    return (index < m_displayNames.size()) ? m_displayNames[index] : L"";
}

IconProvider::IconImage IconProvider::GetDefaultIconForPath(const std::wstring& path) {
    // 进入本函数即意味着真实图标提取/编码已失败，最终极可能落到
    // SHGetFileInfoW("__openDock_default_file__") 的通用白页图标。
    IconImage resolved;
    // 文件夹：取系统真实文件夹图标（SHGetFileInfo 通用文件夹图标）
    if (IsDirectoryW(path)) {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON)) {
            resolved.pngBytes = SaveHiconToPng(sfi.hIcon, 256);
            DestroyIcon(sfi.hIcon);
        }
        return resolved;   // 极罕见（系统失败）才为空
    }
    // 文件：真实存在 → 读真实文件图标（不带 USEFILEATTRIBUTES，避免返回白页通用图标）
    if (FileExistsW(path)) {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
            resolved.pngBytes = SaveHiconToPng(sfi.hIcon, 256);
            DestroyIcon(sfi.hIcon);
            if (!resolved.pngBytes.empty()) return resolved;
        }
    } else {
        // 文件不存在 → 仅此时用 USEFILEATTRIBUTES（通用文件图标，永不灰占位）
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
            resolved.pngBytes = SaveHiconToPng(sfi.hIcon, 256);
            DestroyIcon(sfi.hIcon);
            if (!resolved.pngBytes.empty()) return resolved;
        }
    }
    // 终极兜底：通用文件图标（任意（不存在）文件名，仍能拿到系统默认图标），永不灰占位
    SHFILEINFOW sfi2 = {};
    if (SHGetFileInfoW(L"__openDock_default_file__", FILE_ATTRIBUTE_NORMAL, &sfi2,
                       sizeof(sfi2), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
        resolved.pngBytes = SaveHiconToPng(sfi2.hIcon, 256);
        DestroyIcon(sfi2.hIcon);
    }
    return resolved;
}

