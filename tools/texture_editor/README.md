# mc-rebedrock 实体纹理配置编辑器（Web UI）

一个**纯 Python + 浏览器**的开发工具，用来在不重编译任何 C++ 的前提下，实时修改并验证
实体的纹理配置，快速修复**纹理面映射错误**与 **PNG 分块错误**。它以 `pig`
（`resources/animation/pig.geo.json` + 原版 `pig.png`）为默认例子，但支持
`resources/animation/` 下任意模型（chest / player / quadruped …）。

核心思路：纹理的“面映射”完全由每个 cube 的 `uv`（box-UV 网起点）、`size` 以及
`texture_width`/`texture_height` 决定。工具提供一个所见即所得的编辑器：

- **Box-UV 网格面板**：面板按 PNG **自身像素尺寸**完整显示贴图（不再被截断/压扁）；
  每个 cube 每个面采样的矩形（F/B/E/W/U/D 标色，`↻` 表示已旋转）按游戏同样的规则
  （声明 texel × `PNG/声明`）铺在 PNG 上，虚线框标出声明网格边界。点击选中 cube，
  **拖拽平移整个 box-UV 网**（即修改该 cube 的 `uv`），方向键微调 1 texel（Shift = 4）。
- **单框重贴与旋转**：点击某个 UV 框后可改它的“归属面”（把 `B Body` 改成 `F Body`，
  即该框改给另一个几何面采样）或勾选“旋转 180°”（倒置采样）。这些写入 cube 的
  `faces` 扩展（`{"<位置>": {"as": "<面>", "rotate": 0|180}}`），**3D 预览与游戏端都
  生效**（游戏端 `SkeletalModel` 解析它、`item_entity.vert` 按来源矩形采样并旋转）；
  未改动的框不产生任何字段（恒等包 `0x00543210`，行为与原版 box-UV 完全一致）。
  需要一次 C++/着色器重编译来启用游戏端读取，之后就是纯配置。
- **实时 3D 预览**：前端用 JS 软件光栅化器复刻游戏渲染（透视相机、透视校正插值），
  拖拽旋转 / 滚轮缩放，并给出前/后/左/右/顶/3/4 快捷视角。
- **映射自检**：页面把前端算出的矩形与服务端参考实现（与 `boxUvFaceRect` /
  `item_entity.vert` 同源的 Python）逐一比对，显示“映射与游戏一致 ✓”；有任何偏差会
  亮红灯并列出不匹配的面，防止前端逻辑悄悄偏离。
- **保存**：写回源文件 `resources/animation/<model>.geo.json`（先备份一份 `.bak`），并
  **自动镜像**到所有分发的运行时副本（`build*/game/resources`），因此游戏下次启动读到
  的就是最新配置。写出的 JSON 沿用项目原本的排版（数值数组、cube 各占一行），改一个
  texel 就只有一行 diff。“导出 geo.json”下载的是与保存**完全相同**的字节。

## 预览与游戏一致的地方

预览是**证据**而不是近似，下列每一条都与运行时逐条对齐；改动任意一侧都必须同步另一侧：

| 环节 | 规则 | 运行时出处 |
| --- | --- | --- |
| 面矩形 | 标准 box-UV 网 | `animation::boxUvFaceRect` |
| UV 归一化 | 除以**声明尺寸** `texture_width/height`，不是 PNG 像素尺寸 | `item_entity.vert` 的 `textureSize` |
| 采样 | NEAREST + **REPEAT**（越界绕回，不是夹取） | `createTextureSampler` |
| 投影 | **透视相机**（近大远小），纹理按透视校正插值 | 游戏的透视相机 |
| 排序 | 更靠 -Z 的面胜出（与游戏的遮挡一致） | 逐 face 光栅 + z-buffer |
| 透明 | alpha < 0.1 丢弃 | `item_entity.frag` |
| 光照 | 固定世界方向 (-0.45, 0.85, 0.30)，`0.42 + 0.58 * diffuse` | `item_entity.frag` |
| 骨骼旋转 | 欧拉顺序 Z→Y→X，矩阵 `Rz*Ry*Rx`，绕 pivot | `animation::rotationMatrix` |
| cube 旋转 | 绕 cube 自己的 `pivot` | `drawWorldEntities` |
| `inflate` | 盒子向外扩张，**UV 网不变** | `ModelCube::renderSize` + `item_entity.vert` |
| `neverRender` | 只传递变换，不画自己的 cube | `drawWorldEntities` |
| 缺贴图 | 用 box-UV 网现场画一张程序化皮肤（同样的配色） | `createEntityTextureArray` 的 fallback |

> 归一化用的是**声明尺寸**：PNG 只要是它的整数倍放大（例如 64×32 的声明配 128×64 的
> 高清皮肤），每个面依然落在同一块图案上。两者不成整数倍时才会出现半像素错位，面板会
> 给出黄色提示。

`player` 在游戏里走的是另一条渲染路径（每面一层的玩家皮肤，不是 box-UV），`chest`
是方块渲染；本工具对它们仍按 box-UV 预览，用来检查网格本身，不代表游戏里的最终画面。

## 工具不会替你做主

编辑器只记录你输入的数值，不会“顺手”改成它认为合适的值：

- `uv` **不做任何夹取**。实体采样器是 REPEAT，越出贴图的网在游戏里会绕回另一侧——
  这是一种（通常非预期的）合法映射，面板用虚线标出并在提示里列出 cube，但**照样保存**。
- 数值输入框按原样显示与写回，不做四舍五入。
- 渲染逻辑不会往文档里补默认字段：缺 `uv`/`size` 的 cube 在内存里按加载器的默认值参与
  绘制，但不会因此被写进你的 `.geo.json`。
- 纹理宽高必须是 ≥ 1 的整数；不满足时**恢复原值并报错**，而不是悄悄取整。
- 缺 `texture_width` 时回退到运行时加载器的 16×16（`SkeletalModel::loadGeometry`），
  而不是工具自己觉得好看的 64×32。
- 保存只更新项目里已存在的模型，不会新建几何文件。

## 运行

```sh
python3 tools/texture_editor/server.py
# 打开 http://127.0.0.1:8765/
```

零第三方 Python 依赖（纯 `http.server` + `entity_uv_lib.py` 标准库数学；PNG 读尺寸、
写程序化贴图都只用 `struct` + `zlib`）。端口可用 `--port` 或环境变量
`MC_TEXTURE_EDITOR_PORT` 覆盖；`--root <path>` 可指定项目根目录。

> 当前进程内的游戏不会自动热重载：保存后**重启游戏**即可看到新配置。

## 它能修什么（对应本项目的“纹理面映射错误 / png 分块错误”）

| 症状 | 修法 |
| --- | --- |
| 某个 cube 的四面 / 上下采样到了错误的贴图区域（面映射错误） | 在网格面板拖拽该 cube 的网，或直接改 `uv` |
| 单个面左右颠倒了 | 勾选该 cube 的 `mirror`（水平翻转） |
| 某个 cube 的网跑到了贴图外（预览里出现绕回的重复图案） | 面板用虚线标出该面；把 `uv` 移回范围内，或放大声明尺寸 |
| PNG 与声明尺寸不成整数倍，画面出现半像素错位 | 把声明尺寸改成能整除 PNG 的比例，或换一张贴图 |

## 架构 / 一致性

```
tools/entity_uv_lib.py            box-UV 面矩形公式的唯一 Python 参考实现（无依赖）
tools/entity_uv_render.py         numpy+Pillow 软件光栅器（CLI 预览用）
tools/entity_uv_preview.py        CLI 预览工具（薄封装，接口不变）
tools/texture_editor/server.py    stdlib HTTP 服务：提供模型文档 / 贴图 / 保存（含镜像 + 备份 + 校验）
tools/texture_editor/static/      前端：网格编辑器 + JS 软件光栅 3D 预览 + 映射自检
```

四处必须保持一致：`entity_uv_lib.py::face_rects`、`SkeletalModel.cpp::boxUvFaceRect`、
`item_entity.vert`，以及前端的 `faceRect`；旋转矩阵同理
（`rot_matrix` / `animation::rotationMatrix` / `rotMatrix`）。前端自检徽标会在漂移时报警。

## 自检

```sh
python3 tools/entity_uv_lib.py                 # 断言矩形/旋转顺序与 tests/box_uv_test.cpp 一致，
                                               # 并验证保存排版能逐字节还原现有模型
python3 tools/entity_uv_preview.py pig         # CLI 预览
ctest --preset macos-debug -R box_uv           # C++ 侧的同一批断言（按你的构建预设替换）
```
