# CN 审查提醒分支整合底座设计

**日期：** 2026-10-02

**整合分支：** `codex/cn-prosecution-integration`

**输入分支：** `codex/cn-prosecution-reminders`、`glm/cn-prosecution-reminders`

## 1. 背景与结论

两个输入分支从共同提交 `c5515dd` 分别演进。Codex 分支包含 53 个独有提交，GLM 分支包含 37 个独有提交；三方合并会在数据库、OA 导入、同步核心、界面和 Python 数据源等 29 个文件产生直接冲突。

整合采用“Codex 安全底座 + GLM 业务能力分阶段移植”，不直接合并整条 GLM 分支，也不在冲突文件上批量选择 `ours` 或 `theirs`。

本规格只覆盖第一个可独立验收的子项目：建立统一领域模型和数据库 v6 迁移底座。国内申请界面、完整期限引擎、原生 USPTO 客户端及 Windows 便携打包分别在后续规格中实施。

## 2. 目标

本阶段完成后，整合分支应具备以下能力：

1. 保留 Codex 分支现有数据库写锁、迁移前备份、事务回滚、OA 原子合并和 Sidecar 生命周期行为。
2. 建立统一的远端案件结果、官方事件、批处理状态和案卷持久化模型。
3. 数据库版本提升到 v6，并能安全升级：
   - 未版本化旧数据库；
   - v1 至 v4 数据库；
   - Codex 结构的 v5 数据库；
   - GLM 结构的 v5 数据库。
4. 引入 GLM 后续功能所需的模型字段，但本阶段不改变最终用户界面。
5. 所有迁移可重复执行；重复启动不得丢数据、覆盖人工字段或重复创建索引。
6. 为后续迁入复审、视撤、申请人答复证据、上游限流和期限规则提供稳定接口。

## 3. 非目标

本阶段不实现以下内容：

- 不迁入 GLM 国内申请列表的 OA 列和联动页面。
- 不启用 63 条 CN 期限 JSON 作为生产运行时规则源。
- 不增加 USPTO API Key 设置页。
- 不切换 CN 数据源优先级。
- 不引入 `patx_sync` 或 Windows 便携打包。
- 不删除现有独立 OA 页面。

## 4. 方案比较

### 方案 A：直接合并两个分支

优点是保留 Git 历史形式完整。缺点是 29 个核心文件冲突，而且部分文件虽然没有文本冲突，仍有数据库版本和运行时规则语义冲突。风险不可接受。

### 方案 B：以 GLM 为底座，回迁 Codex 修复

优点是 GLM 已有国内申请界面、期限规则和原生客户端。缺点是需要重新移植 Codex 的 Sidecar 生命周期、连接写锁、数据库失败传播和完整 OA 导入修复，容易产生并发与数据安全回归。

### 方案 C：以 Codex 为底座，分层迁入 GLM 能力

保留已经验证的安全基础设施，先统一数据库和领域模型，再迁业务状态、期限、界面和原生数据源。每一层可单独测试、提交和回滚。

**选择方案 C。**

## 5. 统一领域模型

### 5.1 `RemoteDocument`

保留 Codex 已有字段，并确认以下字段是统一协议的一部分：

- `source`
- `document_type`
- `document_title`
- `raw_title`
- `document_code`
- `document_version`
- `official_date`
- `direction`
- `remote_document_id`
- `source_url`
- `download_url`
- `download_available`
- `fingerprint`
- `event_key`
- `source_trace`
- `raw_metadata`
- `confidence`
- `oa_ordinal`
- `ds`
- `wenjiandm`

本阶段不把申请人最后活动直接塞入 `RemoteDocument`。申请人活动属于案件级证据，应放入案件结果，避免伪装成官方发文。

### 5.2 `RemoteCaseResult`

采用 Codex 的公开顶层结构，增加 GLM 所需的案件级字段：

- `provider_used`：最终提供有效结果的数据源；
- `latest_applicant_activity`：最近一次可确认的申请人提交日期；
- `rate_limited_upstream`：任一上游数据源明确限流时为真；
- `terminal_state`：统一的终局状态代码；
- `reexamination_state`：统一的复审阶段代码。

该结构继续作为 JSON Sidecar 和原生 C++ 数据源的共同边界。解析器必须允许新字段缺失，以兼容旧 Sidecar。

### 5.3 `CaseSyncReport` 与 `BatchSummary`

`CaseSyncReport` 增加 `provider_used`、`rate_limited_upstream`、`latest_applicant_activity`、`terminal_state` 和 `reexamination_state`。批处理累计逻辑在遇到 `rate_limited_upstream=true` 时能够让上层停止继续请求，但停止策略在后续数据源整合阶段接线。

### 5.4 持久化模型

`ProsecutionDocumentRecord` 以 Codex 字段全集为准，继续使用：

- 非空 `event_key` 进行跨来源语义去重；
- `source_trace` 保存发现该事件的数据源集合；
- 旧数据缺少 `event_key` 时回退到来源、申请号和指纹组合键；
- 后台同步只允许更新同步字段，不覆盖处理人、撰写人、人工期限、备注等人工字段。

## 6. 数据库 v6

### 6.1 版本策略

`kSchemaVersionCurrent` 从 5 提升到 6。v6 的含义在两个历史 v5 数据库上保持一致，不允许继续通过“同版本、不同结构”表达分支差异。

### 6.2 结构探测

v5 到 v6 迁移不能假设 v5 来自哪个分支。迁移过程使用 `PRAGMA table_info`、`sqlite_master` 和索引探测，按实际结构补齐缺失项。

必须确保以下结构存在并符合统一约定：

- `prosecution_documents` 的 v5 跨来源字段和下载字段；
- `dossier_sync_state` 的同步状态字段；
- `patents.next_dossier_check_at` 等调度字段；
- OA 同步来源和远端文档标识字段；
- 统一的唯一索引和普通索引；
- 后续案件级状态所需的可空字段。

只允许增加列、表和索引；本阶段不删除或重建用户业务表。

### 6.3 迁移安全

迁移顺序：

1. 读取版本和实际结构；
2. 对需要升级的数据库创建 SQLite 一致性备份；
3. `BEGIN IMMEDIATE`；
4. 幂等补齐结构；
5. 验证关键表、列和索引；
6. 写入 `schema_info.version = 6`；
7. 提交事务；
8. 任一步失败时回滚并保留备份。

如果数据库自称 v6 但关键结构缺失，启动时仍执行结构修复；修复失败则数据库打开失败，禁止带着半完整结构继续运行。

### 6.4 人工数据保护

迁移不得修改以下已有数据：

- OA 处理人、撰写人和备注；
- `deadline_source=manual` 或 `official` 的期限；
- 人工填写的答复日期；
- 本地文件关联；
- 已下载文件路径。

## 7. 错误处理与兼容性

- 新 JSON 字段缺失时使用空值或 `false`，旧 Sidecar 仍可工作。
- 未知终局或复审状态原样保留在元数据中，不自动改变申请状态。
- 数据库写入失败必须返回错误，不能仅记录日志后报告同步成功。
- 上游限流是可识别状态，不归类为普通网络失败。
- 本阶段不因远端授权、驳回或视撤事件直接修改 `Patent.application_status`。

## 8. 测试设计

### 8.1 迁移矩阵

增加四类离线 SQLite fixture 测试：

1. 未版本化旧库升级到 v6；
2. v4 升级到 v6；
3. Codex v5 形状升级到 v6；
4. GLM v5 形状升级到 v6。

每个测试验证版本、关键字段、关键索引和原数据保持不变。每个 fixture 再次运行迁移，验证幂等性。

### 8.2 失败与备份

通过不可写目标或故意破坏的结构触发迁移失败，验证：

- 事务回滚；
- 原版本没有错误提升；
- 原业务数据仍可读取；
- 迁移备份由 SQLite backup API 生成。

### 8.3 协议测试

增加 JSON 解析测试，覆盖：

- 旧协议缺少新增字段；
- 新协议包含数据源、限流、申请人活动和阶段状态；
- 未知字段不导致解析失败；
- `rate_limited_upstream` 正确传播到 `CaseSyncReport`。

### 8.4 回归测试

本阶段必须继续通过：

- Rust 构建；
- 完整 C++/GUI 构建；
- 现有 4 个 CTest；
- Python Sidecar 测试环境建立后的完整测试集。

## 9. 提交与 GitHub 备份

本阶段拆分为小提交：

1. 规格与实现计划；
2. v5 形状测试和失败测试；
3. v6 幂等迁移实现；
4. 统一远端案件模型及协议测试；
5. 全量验证和文档更新。

每个检查点通过对应测试后推送到 `origin/codex/cn-prosecution-integration`。不改写 Codex 或 GLM 原分支历史。

## 10. 后续阶段边界

本底座验收后，后续依次编写独立规格并实施：

1. GLM 案件状态与申请人答复证据；
2. 63 条 CN 期限规则的运行时统一；
3. OA 并入国内申请界面；
4. USPTO ODP Key、本地安全配置与原生客户端；
5. `patx_sync`、Windows CI 和便携打包。
