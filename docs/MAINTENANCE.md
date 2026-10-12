# TH07 维护指南

## 职责与依赖

`src/` 拥有本作游戏对象、固定逻辑步、菜单、Replay 和资源语义。平台适配提供渲染、输入、音频与文件访问。公共算法来自本作 `third_party/eagler-common` 的 Git 锁定；主项目根 common 更新不会自动改变这里的依赖。

游戏仓库只交付 Runtime。Web 页面、资源安装、房间界面和站点组装统一由主项目维护，不恢复旧 `launcher/` 副本。

## 构建输入与输出

实际入口是 `CMakeLists.txt`。建议从主项目使用 `npm run runtime -- th07`，显式提供 Emscripten、Python、字体和资源；本地路径不提交到源码。TH06/TH07 的 source-only 只验证无 DATA 编译，可用基础 ImGui 构建练习；额外 runtime 扩展须显式提供。

CMake 输出位于本游戏 build/web/<variant>/；不含 DATA 的测试产物不能当作可玩完整包。

CMake 在 Emscripten 构建中使用 vendored/ 下的 SDL、SDL_image 和 SDL_ttf。其他目标先查找系统 SDL3 三个组件，缺少任一组件时回退到这些子模块；练习构建还检查所需的 FreeType 目标。切换来源时应重新配置构建目录，不能复用不相容的 CMakeCache。

## 本轮验证

normal 与基础 thprac 无 DATA 构建通过；原生测试 4 项通过。

原生测试使用 CMake/CTest，直接编译实际 C++ 实现；Release 配置保留 assert：

```text
cmake -S tests -B build/core-tests
cmake --build build/core-tests --config Release
ctest --test-dir build/core-tests -C Release --output-on-failure
```

当前登记 4 项，覆盖 netplay core、回滚 journal、输入与副作用。它们不覆盖所有协作规则、关卡或原版对照。浏览器测试仍需显式资源和 Runtime；按 `tests/` 实际入口选择场景。

旧函数片段拼接、历史对象文件字节、源码位置和分支数量门禁已退役。新增测试应执行真实输入/状态/输出，不以固定源码形状替代游戏行为。

## 已知维护边界

旧 ChainCallback 类型擦除及部分 draw 侧兼容状态仍需按真实 ABI 和固定步回归整理，不能继续用函数指针强转扩大边界。TH06/TH07 暂不宣称 Presentation Lab 完整支持。

仓库已有 Android/Apple 平台包装和 CMake 分支，本轮未在对应设备重建验收；它们是游戏 Runtime 平台代码，不是新选型的平台启动器。Android 打包资源应通过 SDL IO/FileSystem 访问，不能假设 APK 资源是普通文件路径。

## 维护顺序

先复现问题并确定状态所有者，修改最小职责边界，再执行对应原生、Runtime 和浏览器验证。升级 common 时记录实际 gitlink 和影响的接口。跨作品抽取需核对实现、ABI 和消费者，不因文件同名就共用。

项目[能力进度](https://github.com/YomotsuHisami/eagler-touhou/blob/main/docs/TOUHOU_PROGRESS.md)区分实现、构建和实测。[输入与 Replay](https://github.com/YomotsuHisami/eagler-touhou/blob/main/docs/playbooks/input-replay.md)、[多人联机](https://github.com/YomotsuHisami/eagler-touhou/blob/main/docs/playbooks/multiplayer.md)说明进一步验证。
