# video-compare 中文文档

并排比较两个(或多个)视频文件,可逐帧对齐、缩放、做差分。

本文档涵盖**使用方法**、**版本历史**和**已知问题**。完整的英文说明见 [README.md](README.md),
逐版本变更见 [CHANGELOG.md](CHANGELOG.md)。

这是 [pixop/video-compare](https://github.com/pixop/video-compare) 的 fork,在上游基础上增加了
**逐帧步进对齐**和**裁剪预览**两个功能。

---

## 安装

**macOS** — `brew install video-compare`

**Arch Linux** — `yay -S video-compare`(AUR)

**Windows** — 从 [Actions](https://github.com/powder21/video-compare/actions) 页面下载构建产物,
解压后 `.exe` 和所有 DLL 在同一目录,直接运行。上游的预编译版见
[pixop releases](https://github.com/pixop/video-compare/releases)。

**从源码编译** — 需要 FFmpeg、SDL2(≥2.0.10)和 SDL2_ttf 的开发库。

```sh
# Debian/Ubuntu
apt install build-essential libavformat-dev libavcodec-dev libavfilter-dev \
            libavutil-dev libswscale-dev libswresample-dev libsdl2-dev libsdl2-ttf-dev
# Fedora
dnf install make gcc-c++ ffmpeg-devel SDL2-devel SDL2_ttf-devel

make && make install
```

Windows 用 MSYS2 的 **MINGW64** 终端:先跑 `./download_and_extract_windows_deps.sh ffmpeg`
(以及 `sdl2`、`sdl2_ttf`)拉依赖,再 `mingw32-make`。

---

## 快速上手

```sh
video-compare left.mp4 right.mp4              # 最常见的用法
video-compare a.mp4 b.mp4 c.mp4               # 一个左视频对多个右视频,Tab 切换
video-compare -w 1280x left.mp4 right.mp4     # 指定窗口宽度
video-compare -m vstack left.mp4 right.mp4    # 上下堆叠而非左右分割
```

启动后**左右移动鼠标**拖动分割线,**滚轮**缩放,**空格**播放/暂停。

---

## 键盘

### 主键盘区

![键盘快捷键](keyboard-zh.svg)

> 图中只画了单键功能。带修饰键的(`Shift+`、`Ctrl+`、`Alt+`)见下方表格。

### 播放与定位

| 按键 | 功能 |
|---|---|
| `Space` | 播放 / 暂停 |
| `←` `→` | 后退 / 前进 1 秒 |
| `↑` `↓` | 前进 / 后退 15 秒 |
| `PgUp` `PgDn` | 前进 / 后退 600 秒 |
| `J` `L` | 减速 / 加速播放 |
| `,` `.` | 缓冲区内循环播放(双向 / 单向) |
| `A` `D` | 在缓冲区内浏览上一帧 / 下一帧 |
| `Shift+A` | 跳到上一帧(帧内编码格式效果最佳) |
| `Shift+D` | 解码并前进一帧 |

### 逐帧对齐(本 fork 新增)

比较两个视频时,若右视频比左视频早/晚若干帧,用这组键把它们对齐。**偏移量会显示在屏幕上。**

| 按键 | 功能 |
|---|---|
| `+` `-` | 当前右视频前进 / 后退 **1 帧** |
| `Ctrl` + `+` `-` | 前进 / 后退 **10 帧** |
| `Alt` + `+` `-` | 前进 / 后退 **100 帧** |
| `Ctrl+0` | 清除当前右视频的偏移 |
| `Tab` | 切换到下一个右视频 |
| `Ctrl+Shift+1..0` | 直接切到第 1~10 个右视频 |

> 步进只在**暂停**时可用。屏幕上没有"当前是哪个右视频"的提示,可用 `Shift+X` 在终端确认。

### 视图

| 按键 | 功能 |
|---|---|
| `1` `2` `3` | 隐藏/显示 左视频 / 右视频 / HUD |
| `4` | 1:1 像素 |
| `5` `6` `7` `8` `9` | 缩放 50% / 100% / 200% / 400% / 800% |
| `Z` `C` | 放大鼠标周围区域(显示在左下角 / 右下角) |
| `E` | 以鼠标位置为中心重新居中 |
| `R` | 全局居中并复位缩放到 100% |
| `S` | 交换左右视频 |
| `Shift+M` | 循环切换布局(分割 / 上下 / 左右) |
| `Shift+S` | 循环切换宽高比模式 |
| `Alt+Enter` | 全屏 |

### 差分与分析

| 按键 | 功能 |
|---|---|
| `0` | 切换 视频 / 减法(差分)模式 |
| `Y` | 循环切换减法模式 |
| `U` | 仅亮度减法模式 |
| `M` | 在终端打印图像相似度指标 |
| `P` | 在终端打印鼠标位置和像素值 |
| `F1` `F2` `F3` | 直方图 / 矢量示波器 / 波形图 |
| `V` | 视频信息浮层 |
| `X` | 显示帧率(视频 / UI) |

### 裁剪与保存(本 fork 新增)

| 按键 | 功能 |
|---|---|
| `Shift+F` | **选区并打开裁剪预览**;预览中 `Enter` 只存拼接图,`Shift+Enter` 存左视频 + 每个右视频 + 拼接图 |
| `F` | 保存两侧当前帧和屏幕内容为 PNG |
| `Shift+L` `Shift+R` `Shift+B` | 交互式裁剪 左视频 / 右视频 / 两侧同区域 |
| `Backspace` | 清除裁剪 |

### 其他

| 按键 | 功能 |
|---|---|
| `H` | 显示/隐藏屏幕帮助 |
| `Esc` | 退出 |
| `I` `T` | 切换 输入对齐缩放质量 / 纹理过滤方式 |
| `Ctrl+C` / `Cmd+C` | 复制左视频当前时间戳 |
| `Ctrl+V` / `Cmd+V` | 粘贴时间戳并跳转 |
| `Shift+X` | 在终端打印内部状态(调试用) |
| `Shift+W` / `Ctrl+W` / `Ctrl+Shift+W` | 恢复保存的窗口尺寸 / 恢复启动尺寸 / 保存当前尺寸 |

> 按住 `Ctrl` 或 `Shift` 可让相对定位、播放速度、缩放的调整幅度更小。

---

## 鼠标

| 操作 | 功能 |
|---|---|
| 左右移动 | 拖动分割线 |
| 滚轮 | 以光标下的像素为中心缩放 |
| 按住右键拖动 | 平移画面 |
| 左键单击 | 按光标横向位置跳转(目标位置显示在右下角) |

> 单击跳转会落到最近的**关键帧**,不是精确帧。

---

## 常用命令行选项

完整列表见 `video-compare --help`。

| 选项 | 说明 |
|---|---|
| `-w, --window-size` | 窗口尺寸,如 `800x600`、`1280x`、`x480` |
| `-m, --mode` | 布局:`split`(默认)/ `vstack` / `hstack` |
| `-t, --time-shift` | 时移右视频,如 `0.150`、`-0.1`、`x1.04+0.1` |
| `-f, --frame-buffer-size` | 帧缓冲大小,默认 50(影响步退能走多远不用 seek) |
| `-S, --subtraction-mode` | 以差分模式启动 |
| `-u, --fullscreen` | 全屏启动 |
| `-d, --high-dpi` | 高 DPI 模式(Retina 上显示 HD 内容推荐) |
| `-b, --10-bpc` | 每通道 10 bit |
| `-a, --auto-loop-mode` | 缓冲满后自动循环:`off`(默认)/ `on` / `pp`(乒乓) |
| `-v, --verbose` | 详细输出(库版本、渲染细节等) |
| `-c, --show-controls` | 打印全部快捷键后退出 |

用 `::` 分隔符可为**单个右视频**覆盖任意选项,例如给第二个右视频单独指定解码器。

---

## 版本历史

### v1.2 — 2026-07-17

**步进现在准了。**

- 步进超过约 8 帧后,两边可能**轮流播放**(一边动、另一边冻住,然后反过来)。原因是偏移量每帧都从帧时长的滑动均值重算,而这个均值又是从被偏移量移动过的时间戳测出来的 —— 它在喂自己。现在偏移在你按下按键的那一刻就固定成时间值,取自被跨过的帧之间的真实距离。
- 先步退再步进会**跳帧**,而且明明差了一帧却报告零偏移。被退过的帧现在会留着,步进时原样还回去,`+` 和 `-` 严格互逆。
- 步退越过视频开头不再记录不存在的帧数 —— 那会在恢复播放时把**左视频**往前拖。
- **屏幕上的偏移数字现在总是和画面一致**,不会再出现"两边对齐却显示 -1 帧"。
- 同一文件比较多次、且其中某个右视频被步进过时,播放中 seek 可能冻住窗口。
- 每个右视频现在按自己的帧率来对齐,而不是当前选中那个的。

### v1.1 — 2026-07-16

**步进不再冻死窗口,Windows 恢复可编译。**

- 步进右视频后恢复播放,或步进超出内存中的帧,窗口会冻死,只能强杀进程。只在同一文件互相比较时发生。
- MinGW 构建被钉死在一个依赖脚本已不再下载的 FFmpeg 版本上,导致每次 Windows 构建都失败。

### v1.0 — 2026-04-13

**两个新功能。**

**逐帧步进** — 用 `+` / `-` 相对左视频步进当前右视频(`Ctrl` 十帧,`Alt` 百帧),偏移量显示在屏幕上,`Ctrl+0` 清除。

**裁剪预览** — `Shift+F` 选区后打开预览而非直接保存。可绕光标缩放、平移,`Enter` 存拼接图,`Shift+Enter` 存左视频 + 每个右视频 + 拼接图。保存前用原生对话框选目录,文件名带时间戳前缀。

---

## 已知问题

以下是代码审查发现、经复现或推演确认,但**有意未修**的问题。每条都注明了原因 ——
它们要么现实中概率极低,要么改了也无法通过运行程序验证。本 fork 的原则是:**验证不了的就不改。**

| 位置 | 问题 | 为什么还留着 |
|---|---|---|
| `video_compare.cpp:918` | 局部 seek 可能在 demuxer 线程正处于 `av_read_frame` 时调用 `av_seek_frame`(同一个 `AVFormatContext`),属未定义行为。完整 seek 路径有握手机制,局部的没有。 | 需要慢速 I/O(网络流、高负载机器)才撞得上竞态窗口,无法按需复现。 |
| `video_compare.cpp:1269` | 恢复播放时的重对齐若取不到帧,失败被丢弃,画面静止且无任何提示。 | 需要 seek 目标越过文件末尾,而常规 seek 路径通常先一步拦下。复现不出来。 |
| `video_compare.cpp:928` | 时移乘数不为 1 时,局部 seek 的目标漏算了乘数自身的贡献。完整 seek 路径算了。 | 需要带乘数的 `--time-shift`,未测试。 |
| `video_compare.cpp:1142` | 前进步进触发 seek 后遇到文件末尾,会报"reached end of video",但偏移其实已经变了。 | 纯提示问题。 |
| `video_compare.cpp:1262` | 步进仍会往帧时长均值里灌入不是帧时长的样本,`delta_pts_` 可能偏离真实值。 | v1.2 起偏移量已不依赖它。剩余影响只到同步容差和帧上标注的时长。 |
| `video_compare.cpp:1636` | 步退后按 `Shift+D`,帧从该步退留存的缓冲里取。推断正确,实际使用也符合预期。 | 无日志证据 —— 调试打印无法把它和普通播放区分开。 |

### 已知取舍(非 bug)

- 局部 seek 一律关闭单解码器模式,直到下次真正 seek 才重算。结果正确,代价是同一文件比较时在这期间会解码两遍。
- 跨过丢帧步进时,报告的是实际跨过的**两个帧时长**而非一帧。数字跟随画面 —— 在有丢帧的素材上,"帧数"和"时间跨度"本就不是同一个量。

---

## 致谢

`video-compare` 由 Jon Frydensbjerg 创建,主要基于 [pockethook/player](https://github.com/pockethook/player)。
感谢 [FFmpeg](https://github.com/FFmpeg/FFmpeg)、[SDL2](https://github.com/libsdl-org/SDL)、
[stb](https://github.com/nothings/stb) 的作者们。
