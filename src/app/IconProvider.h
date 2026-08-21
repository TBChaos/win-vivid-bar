// src/app/IconProvider.h
// 图标提供器 — 路径解析 / EXE 图标提取 / 占位容错
// 设计参考：详细设计说明 §2.7
#pragma once
#include <windows.h>
#include "../Common.h"
#include "ConfigManager.h"
#include <cstdint>
#include <string>

class IconProvider {
public:
    // 内存图标：优先 pngBytes（IconProvider 提取后填入，不落盘）；
    // filePath 可选，仅 main.cpp 的 GDI 单测路径直接构造使用。
    struct IconImage {
        std::vector<uint8_t> pngBytes;
        std::wstring filePath;
    };

    // 解析配置中的图标项，返回可加载的内存图像列表。
    // 文件不存在 → 回退系统默认图标（错误容忍，不崩溃）
    std::vector<IconImage> LoadIcons(const AppConfig& config);

    // 从 EXE/DLL 提取图标并编码为内存 PNG（不落盘）
    HRESULT ExtractIconFromExe(const std::wstring& exePath, int iconIndex,
                               IconImage& outImage);

    std::wstring GetDisplayName(size_t index) const;
    size_t Count() const { return m_images.size(); }

private:
    // 生成系统默认文件类型图标（按扩展名 / 文件夹），返回内存 PNG；
    // 永不产生灰色占位，保证总有真实/默认图标（需求2）。
    IconImage GetDefaultIconForPath(const std::wstring& path);

    std::vector<IconImage>    m_images;
    std::vector<std::wstring> m_displayNames;

    // 已解析缓存：源路径 -> 已提取的内存图标。重建（增删/重排/拖入）时直接复用字节，
    // 避免重复提取/重复编码。
    std::map<std::wstring, IconImage> m_resolvedCache;
};
