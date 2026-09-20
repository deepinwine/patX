# patX — 专利/IP 管理系统 (C++17 + wxWidgets + SQLite)

patX 是一个跨平台专利与知识产权管理桌面软件：国内专利、OA 处理（含 CNIPA 网页自动查询最新审查意见）、PCT、软件著作权、集成电路布图、国外专利、年费与期限规则，全部存储在本地 SQLite 数据库中。

当前版本：**0.4.0**（版本号唯一来源：`CMakeLists.txt` 的 `project(patX VERSION ...)`，经 `include/patx/version.h.in` 生成到代码各处）。

## 架构

```
wxWidgets GUI (src/cpp/main_gui.cpp, src/cpp/ui/*)
        │
patx_core 静态库（无 GUI 依赖，可独立构建与测试）
        │
        ├── database/            核心业务表 + 版本化迁移
        ├── io/excel_io          Excel/CSV 导入导出（OpenXLSX）
        └── web_dossier_rules    网页审查同步的 OA 合并规则（纯逻辑，可测）
        │
     SQLite (WAL)
```

- **GUI 与数据/网络完全分层**：`patx_core` 不依赖 wxWidgets，可在 macOS/Linux/CI 上独立构建并运行单元测试。
- **网页审查同步在 worker 线程执行**（Python sidecar 进程 + stdio JSON-RPC），结果经 `wxThreadEvent` 回到主线程，不冻结界面。
- 仓库中的 Rust 代码（`src/rust`）是历史遗留，**未参与当前构建与运行链路**，仅作参考保留；后续如有百万级全文检索需求再评估接入。

### 与旧版 README 的差异（诚实声明）

旧 README 宣称的 "C++/Rust 混合架构"、50x/100x 性能对比表已删除——此前的 benchmark 用 "C++ 时间 × 固定倍数" 伪造 Python 基线，属于虚假数据，相关代码已从仓库移除。本项目现阶段以正确性为先，不发布未经实测的性能数字。

## 功能模块

| 模块 | 说明 |
|------|------|
| 国内专利 | 完整 CRUD、批量等级/状态、列头筛选、自然排序、结构化字段（技术路线/研发项目/标签/代理人/1st-5th OA 提醒等，不再拼进备注） |
| OA 处理 | 期限倒计时配色、5/30 天到期过滤、PDF 导入自动匹配申请号并按规则表计算建议绝限 |
| PCT / 软著 / IC / 国外专利 | 完整 CRUD（编辑真正更新全字段）、搜索/状态/处理人筛选 |
| **审查信息同步 (Dossier Sync)** | CNIPA 网页自动查询最新审查意见：公开号→申请号解析、审查文件清单入库（指纹去重）、最新 OA 识别与官文日获取、按保护规则更新 OA 记录 |
| 年费 | 从真实授权专利生成（中国官费标准）；未配置规则的地区显示 "Rule not configured"，不再显示假数据 |
| 期限规则 | 数据库存储的规则表（管辖地/事件/月/日/可延期），支持增删改与启用禁用；OA 期限共用同一引擎；人工修改的期限（deadline_source=manual）不会被同步覆盖 |

## 审查意见网页同步（CNIPA）

不需要任何 API Key。首次使用在菜单 `审查信息同步 → CNIPA 登录` 中用自己的账号在可见浏览器里完成登录（软件不读取密码、不自动化验证码），之后：

- OA 页选中记录点【查询最新审查意见】即可按案件自动查询；或菜单里更新全部活跃案件
- 人工节奏逐件查询（单页面、最小间隔、遇到限流提示本批立即停止）
- 发现新 OA 只**新增**记录（source=cnipa）；人工录入的处理人/撰写人/摘要/期限永不被覆盖
- 已有记录缺官文日时仅补日期；日期不一致标记 `date_conflict` 待人工确认，不自动改写
- 浏览器优先使用系统已安装的 Chrome/Edge；详细红线与调试方法见 `tools/web_dossier/README.md`

## 构建

### Windows（主要平台，vcpkg）

```powershell
git clone https://github.com/deepinwine/patX
cd patX
cmake -B build -S . `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

依赖由 `vcpkg.json` manifest 自动安装：wxwidgets、openxlsx、curl、sqlite3、nlohmann-json。

### macOS / Linux（核心库 + 测试；GUI 需另装 wxWidgets）

```bash
brew install sqlite3 curl wxwidgets   # wxwidgets 仅 GUI 需要
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j8
ctest --test-dir build
```

未安装 wxWidgets 时自动只构建 `patx_core` 与测试（`-DPATX_BUILD_GUI=OFF` 可强制关闭）。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

覆盖：数据库 CRUD（全部模块全字段往返）、v1→v2 迁移（含 notes 前缀搬迁与迁移前备份）、统一查询过滤（含 LIKE 通配符转义）、期限规则引擎、真 XLSX 导出（重新打开校验单元格）、CSV 引号规则、结构化字段导入、网页同步 OA 合并规则（含冲突保护）。


## 数据安全

- 版本化迁移（`schema_info` 表）：只 ALTER 加列，永不 DROP/重建；迁移前自动生成 `patents.db.pre_migration_<时间戳>.bak`，迁移在事务内执行并验证。
- NAS 同步为**文件级、单人使用**设计：同步前先对本地库做备份，冲突时保留本地较新版本；它不是多人实时协作系统。
- 统一日志（`patx.log`）：记录迁移/导入导出/同步与解析；网页同步的密码/Cookie 永不落日志。

## 已知限制（不夸大）

- OCR 未实现：PDF 文本依赖 `pdftotext`（poppler-utils）；无文本层的扫描件会标记待人工处理，而不是假装解析成功。
- `claim(s)` 依赖解析基于规则表达式，复杂从属关系可能漏解析（不影响权利要求文本本身保存）。
- （历史说明）USPTO ODP API 同步因无法取得 API Key 已于 2026-09 移除；架构保留 provider 抽象，未来如需恢复可重新实现。
- Windows 下 API Key 存于本地数据库文件；如需 DPAPI/凭据管理器加密存储可作为后续增强。

## License

见仓库源码；仅供内部专利管理使用。
