# uPl-project-switch

**ustxPlayer 的工程格式转换工具** —— uPl 生态内的通用工程格式转换器。

> 本仓库是 [ustxPlayer](https://github.com/lyrinXD/ustxPlayer) 的下属项目，对应主项目中「其他」页面提供的工程格式转换方案。

## 简介

uPl 是 [ustxPlayer](https://github.com/lyrinXD/ustxPlayer) 与 [ustPlayer](https://github.com/SYEternalR/ustPlayer) 两个程序共同的项目名缩写。两个项目都历经多次改版：以 ustxPlayer 为例，它始终使用 JSON 格式，只是具体的数据存储结构随版本变化而不同，因此不同版本之间互不直接兼容。

**当前基础功能**：将旧版 `.uplr` 工程转换为新版 `.uprj`，并把 `settings` 重写为规范结构。

## 使用

将 `.uplr` 工程文件拖到可执行文件上即可，程序会自动识别并在同目录生成转换后的工程文件。

### 编译

Windows 下使用 MinGW GCC 编译：

```bash
gcc -Os -s -static -o uPl-project-switch.exe uPl-project-switch.c
```

## 未来计划

- **长期目标（包罗万象）**：作为 uPl 生态的通用工程转换器，逐步覆盖两个程序、各个历史版本之间的格式互转。
- **兼容更多版本 / 格式**：当前仅支持 ustxPlayer 旧版 `.uplr` → 新版 `.uprj`；后续适配更多版本，包括 [ustxPlayer](https://github.com/lyrinXD/ustxPlayer) 后续将采用的 7Z 承载方式，以及 [ustPlayer](https://github.com/SYEternalR/ustPlayer) 的 `.ust` 等其他格式。
- **模块化**：目前为单一 C 源文件实现，后续将拆分为解析、转换、输出等独立模块，便于扩展与维护。

## 许可证

本项目遵循 **GNU GPL v3** 开源协议，详见 [LICENSE](LICENSE)。