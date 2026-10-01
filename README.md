# patX — 专利/IP 管理系统 (C++17 + wxWidgets + SQLite)

patX 是一个跨平台专利与知识产权管理桌面软件：国内专利、OA 处理（含中国申请官方发文提醒）、PCT、软件著作权、集成电路布图、国外专利、年费与期限规则，全部存储在本地 SQLite 数据库中。

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
| **审查信息同步 (Dossier Sync)** | 按 USPTO Global Dossier → EPO Global Dossier → CNIPA 顺序查询中国申请的官方发文元数据；识别 OA、驳回、授权、补正等事件，跨来源去重后建立提醒 |
| 年费 | 从真实授权专利生成（中国官费标准）；未配置规则的地区显示 "Rule not configured"，不再显示假数据 |
| 期限规则 | 数据库存储的规则表（管辖地/事件/月/日/可延期），支持增删改与启用禁用；OA 期限共用同一引擎；人工修改的期限（deadline_source=manual）不会被同步覆盖 |

## 中国申请官方发文提醒

首期只检查申请号或公开号可识别为中国申请的案件。查询顺序如下：

1. USPTO Global Dossier；
2. EPO Global Dossier；
3. CNIPA（前两层没有可用结果时使用）。

USPTO 和 EPO 的公共案卷元数据查询不使用 USPTO ODP API Key，也不使用 EPO OPS Key。EPO OPS Key 仍只用于软件原有的同族和法律状态查询。

使用方式与保护规则：

- 在 OA 页选中案件后点击「查询最新审查意见」，也可以从「审查信息同步」菜单更新全部活跃案件。
- 软件只读取 `ORIGINAL` 版本的官方发文元数据，不下载审查意见正文。
- 事件类型包括第一次、第二次和后续审查意见，以及驳回、授权、补正和其他官方通知。
- 同一官方事件按稳定事件键跨来源去重；首次发现时新增提醒，重复检查不会重复创建。
- 已有记录缺少官文日时只补日期；日期不一致时标记为「待人工确认」，不自动覆盖。
- 人工录入的处理人、撰写人、摘要和期限不会被同步覆盖；授权和驳回只建立提醒，不改变专利主档状态。
- 默认每天检查一次，可在「审查信息同步 → 审查提醒设置」中选择每 1、3 或 7 天检查。软件启动约 30 秒后执行首次调度，运行期间每 30 分钟检查是否有到期案件。
- 没有新事件时保持静默；有新事件、日期冲突或登录失效时显示桌面通知。点击登录提示后才显示 CNIPA 登录入口，不会自动打开登录页面。

CNIPA 需要登录时，请在「审查信息同步 → CNIPA 登录」中用自己的账号完成可见浏览器登录。软件不读取密码、不自动处理验证码；登录 Profile 会保存在本机供后续复用。浏览器优先使用系统已安装的 Chrome 或 Edge。实现边界和调试方法详见 [`tools/web_dossier/README.md`](tools/web_dossier/README.md)。

## OA Excel 导入与处理人更新

导入 OA 工作表时，patX 会先预检，再按「案件编号（格科编号）+ 规范化 OA 类型 + 发文日」精确匹配已有 OA 记录。匹配与更新规则如下：

- 没有匹配记录时，OA 正常新增，并保存 Excel 中的新处理人。
- 仅匹配到 1 条记录且原处理人为空时，自动补入 Excel 中的处理人。
- 原处理人与 Excel 中的非空处理人不一致时，预检界面会汇总冲突；用户可以整批选择保留原处理人，或者以 Excel 覆盖。
- Excel 中的处理人为空时，绝不会清空已有处理人。
- 同一匹配条件对应多条已有记录时，该行会跳过并计入「匹配冲突」，不会猜测或覆盖。
- 对匹配成功的已有记录只更新处理人字段；撰稿人、摘要、期限等人工维护字段保持不变。
- 在预检界面取消时，不会写入该 OA 工作表；同一文件中的其他类型工作表不受影响。

OA 类型会在匹配前进行规范化，例如「一通」与「第一次审查意见通知书」按同一类型处理。当前不会按申请号匹配 OA，也不会自动清理历史重复记录。

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

Python sidecar 测试：

```bash
cd tools
python3 -m pytest web_dossier/tests -q
```

覆盖：数据库 CRUD、版本化迁移、统一查询过滤、期限规则引擎、真 XLSX 导出、CSV 引号规则、结构化字段导入、Global Dossier/CNIPA 解析、三层降级、跨来源事件去重、后台调度、通知分流及阻塞 RPC 取消。


## 数据安全

- 版本化迁移（`schema_info` 表）：只 ALTER 加列，永不 DROP/重建；迁移前自动生成 `patents.db.pre_migration_<时间戳>.bak`，迁移在事务内执行并验证。
- NAS 同步为**文件级、单人使用**设计：同步前先对本地库做备份，冲突时保留本地较新版本；它不是多人实时协作系统。
- 统一日志（`patx.log`）：记录迁移/导入导出/同步与解析；网页同步的密码/Cookie 永不落日志。

## 已知限制（不夸大）

- OCR 未实现：PDF 文本依赖 `pdftotext`（poppler-utils）；无文本层的扫描件会标记待人工处理，而不是假装解析成功。
- `claim(s)` 依赖解析基于规则表达式，复杂从属关系可能漏解析（不影响权利要求文本本身保存）。
- Global Dossier 页面或 CNIPA 页面结构发生变化时，解析可能暂停并把案件标记为待人工处理；软件不会猜测结果。
- EPO OPS Key 目前保存在本地数据库文件中，仅供同族和法律状态功能使用；如需 DPAPI/凭据管理器加密存储可作为后续增强。

## License

见仓库源码；仅供内部专利管理使用。
