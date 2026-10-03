# CN 审查提醒整合底座实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在 Codex 安全底座上建立数据库 v6 和统一远端案件协议，使 Codex/GLM 两类 v5 数据库都能无损升级，并为后续案件状态、期限和国内申请界面整合提供稳定接口。

**架构：** 数据库迁移继续由 `RunSchemaMigrations` 统一管理，新增一个幂等的 v6 形状修复函数；它既执行 v5→v6 升级，也修复“版本已是 v6 但关键结构缺失”的数据库。远端协议继续使用公开的 `RemoteCaseResult`，把 GLM 的案件级字段传播到 `CaseSyncReport` 和 `DossierSyncState`，不改变 `Patent.application_status`。

**技术栈：** C++17、SQLite3、nlohmann/json、CMake/CTest、现有自定义 C++ 测试框架、Python Sidecar pytest。

---

## 文件结构

- 修改：`include/patx/schema_migrations.hpp`——将当前版本提升到 v6，并说明当前形状修复语义。
- 修改：`src/cpp/database/schema_migrations.cpp`——增加 v6 字段、索引、形状验证、备份和幂等修复。
- 修改：`include/database.hpp`——扩展 `DossierSyncState` 的案件级同步字段。
- 修改：`src/cpp/database.cpp`——新库创建 v6 字段并读写新增同步状态。
- 修改：`tests/test_database.cpp`——定义 Codex v5、GLM v5、缺损 v6 和数据保护契约。
- 修改：`include/web_dossier.hpp`——统一 `RemoteCaseResult` 与 `CaseSyncReport` 案件级字段。
- 修改：`src/cpp/web_dossier_rules.cpp`——解析、传播并持久化新增案件级字段。
- 修改：`tests/test_web_dossier_rules.cpp`——覆盖旧协议兼容、新协议解析和报告传播。
- 修改：`docs/superpowers/plans/2026-10-02-cn-prosecution-integration-foundation.md`——执行过程中勾选已完成步骤。

### 任务 1：用测试锁定双 v5 到 v6 的迁移契约

**文件：**

- 修改：`tests/test_database.cpp:100-450`

- [x] **步骤 1：把现有当前版本断言改为 v6**

将所有表示迁移后当前版本的硬编码 `5` 改为 `6`；备份数据库仍应保留原始版本 0 或 4。保留 v4→v5 语义测试，但把测试名改为 `database_v4_to_current_migration_is_incremental_and_preserves_data`。

关键断言：

```cpp
CHECK_EQ(db.SchemaVersion(), patx::kSchemaVersionCurrent);
CHECK_EQ(db.SchemaVersion(), 6);
CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state",
                      "latest_applicant_activity"));
CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "terminal_state"));
CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state",
                      "reexamination_state"));
```

- [x] **步骤 2：新增 Codex v5 形状升级测试**

新增 `database_codex_v5_to_v6_preserves_data_and_is_idempotent`。使用原始 SQLite 句柄创建 `schema_info(version=5)`、完整的 Codex v5 `prosecution_documents`、`dossier_sync_state`、`patents`、`oa_records` 和两个关键索引；插入一条同步状态和一条带 `handler='张三'`、`deadline_source='manual'` 的 OA。

调用：

```cpp
auto first = patx::RunSchemaMigrations(raw, path);
CHECK(first.ok);
CHECK_EQ(first.from_version, 5);
CHECK_EQ(first.to_version, 6);
CHECK(first.performed_backup);

auto repeated = patx::RunSchemaMigrations(raw, path);
CHECK(repeated.ok);
CHECK_EQ(repeated.from_version, 6);
CHECK_EQ(repeated.to_version, 6);
```

验证三个 v6 字段存在、两个关键索引存在、OA 的处理人和人工期限原值不变、案卷记录的 `event_key` 和 `source_trace` 不变。

- [x] **步骤 3：新增 GLM v5 形状升级测试**

新增 `database_glm_v5_to_v6_repairs_missing_codex_shape`。fixture 使用 GLM v5 的业务列，但故意不创建 `idx_patents_next_dossier_check`，模拟 GLM 分支依靠初始化阶段补索引的数据库。迁移后验证：

```cpp
CHECK_EQ(patx::ReadSchemaVersion(raw), 6);
CHECK(SqliteHasIndex(raw, "idx_patents_next_dossier_check"));
CHECK(SqliteHasIndex(raw, "idx_prosecution_docs_event_key"));
CHECK(SqliteHasColumn(raw, "dossier_sync_state", "latest_applicant_activity"));
CHECK(SqliteHasColumn(raw, "dossier_sync_state", "terminal_state"));
CHECK(SqliteHasColumn(raw, "dossier_sync_state", "reexamination_state"));
```

- [x] **步骤 4：新增缺损 v6 自愈和失败回滚测试**

新增 `database_reopens_v6_and_repairs_missing_required_shape`：先创建版本 6 数据库，但删除一个 v6 字段不可行，因此 fixture 直接创建缺少 `terminal_state` 的 `dossier_sync_state`。调用迁移后应补列、保留版本 6，并创建迁移前备份。

新增 `database_v6_repair_failure_rolls_back_and_keeps_version`：创建 `dossier_sync_state` 视图代替表，使 `ALTER TABLE` 失败；验证迁移返回失败、版本仍为 6、原始标记数据仍可读取。

- [x] **步骤 5：运行测试并确认红灯**

运行：

```bash
cmake --build build -j8 --target patx_tests
./build/patx_tests
```

预期：测试编译成功，但因 `kSchemaVersionCurrent` 仍为 5、v6 字段不存在而失败；失败名称应来自本任务新增或更新的迁移测试。

- [x] **步骤 6：提交迁移契约测试**

```bash
git add tests/test_database.cpp
git commit -m "test(数据库): 定义双v5升级到v6契约"
```

### 任务 2：实现数据库 v6 幂等迁移

**文件：**

- 修改：`include/patx/schema_migrations.hpp:17-40`
- 修改：`src/cpp/database/schema_migrations.cpp:30-860`
- 修改：`include/database.hpp:240-252`
- 修改：`src/cpp/database.cpp:400-435,2420-2485`
- 测试：`tests/test_database.cpp`

- [x] **步骤 1：提升模式版本并定义 v6 字段**

把当前版本改为 6：

```cpp
inline constexpr int kSchemaVersionCurrent = 6;
```

在 `DossierSyncState` 增加：

```cpp
std::string latest_applicant_activity;
std::string terminal_state;
std::string reexamination_state;
```

这些字段允许空字符串；不增加瞬态的 `rate_limited_upstream` 数据库列。

- [x] **步骤 2：实现 v6 形状检测和幂等补齐**

在迁移实现的匿名命名空间增加：

```cpp
bool HasV6Shape(sqlite3* db);
bool EnsureV6Shape(sqlite3* db, std::string* error);
```

`EnsureV6Shape` 必须：

1. 验证 `patents`、`oa_records`、`prosecution_documents` 和 `dossier_sync_state` 都是表；
2. 给 `dossier_sync_state` 幂等增加三个案件级字段；
3. 幂等创建 `idx_prosecution_docs_event_key`；
4. 幂等创建 `idx_patents_next_dossier_check`；
5. 返回 `HasV6Shape(db)` 的最终结果。

核心字段定义：

```cpp
const std::vector<std::pair<std::string, std::string>> state_columns = {
    {"latest_applicant_activity", "TEXT DEFAULT ''"},
    {"terminal_state", "TEXT DEFAULT ''"},
    {"reexamination_state", "TEXT DEFAULT ''"},
};
```

- [x] **步骤 3：实现 v5→v6 和当前 v6 修复事务**

在迁移循环增加 `version == 5` 分支：

```cpp
if (!Exec(db, "BEGIN IMMEDIATE;", &result.error)) { /* return failure */ }
if (!EnsureV6Shape(db, &result.error) ||
    !Exec(db, "DELETE FROM schema_info;", &result.error) ||
    !Exec(db, "INSERT INTO schema_info(version) VALUES(6);", &result.error) ||
    !Exec(db, "COMMIT;", &result.error)) {
    Exec(db, "ROLLBACK;");
    result.ok = false;
    return result;
}
version = 6;
```

当读出的版本已经是 6 但 `HasV6Shape` 为假时，也必须走一次带备份的 `BEGIN IMMEDIATE` 修复事务；修复成功仍保持版本 6，失败则回滚。

- [x] **步骤 4：让备份判定覆盖缺损 v6**

`needs_backup` 除了旧版本升级，还应覆盖现有业务数据库的 v6 形状修复：

```cpp
const bool needs_v6_repair = version == kSchemaVersionCurrent && !HasV6Shape(db);
const bool needs_backup = create_backup && db_path != ":memory:" &&
    (version < kSchemaVersionCurrent || needs_v6_repair ||
     (stamp_current_shape && has_patents));
```

未知高版本数据库必须返回错误，不允许降级或改写。

- [x] **步骤 5：更新新库建表和状态读写**

在 `Database::InitTables` 的 `dossier_sync_state` 建表 SQL 中加入三个字段。更新 `UpsertDossierSyncState` 的 INSERT、冲突 UPDATE 和 `GetDossierSyncStates` 的 SELECT/列读取，保证三个字段往返一致。

- [x] **步骤 6：运行迁移测试确认绿灯**

运行：

```bash
cmake --build build -j8 --target patx_tests
./build/patx_tests
```

预期：全部数据库和 Excel 测试通过，进程退出码 0。

- [x] **步骤 7：提交 v6 实现**

```bash
git add include/patx/schema_migrations.hpp include/database.hpp \
  src/cpp/database/schema_migrations.cpp src/cpp/database.cpp
git commit -m "feat(数据库): 统一双v5到v6迁移底座"
```

### 任务 3：用测试锁定统一远端案件协议

**文件：**

- 修改：`tests/test_web_dossier_rules.cpp:1460-1645`

- [x] **步骤 1：扩展新协议解析测试**

在 `TestRemoteCaseParsingAndApplicationBoundary` 的 JSON 根对象加入：

```json
"latest_applicant_activity": "2026-02-18",
"rate_limited_upstream": true,
"terminal_state": "REEXAMINATION_PENDING",
"reexamination_state": "REQUEST_PERIOD"
```

解析后断言：

```cpp
CHECK_STR_EQ(remote.latest_applicant_activity, "2026-02-18");
CHECK(remote.rate_limited_upstream);
CHECK_STR_EQ(remote.terminal_state, "REEXAMINATION_PENDING");
CHECK_STR_EQ(remote.reexamination_state, "REQUEST_PERIOD");
```

- [x] **步骤 2：验证报告和数据库状态传播**

调用 `ApplyRemoteCaseResult` 后验证：

```cpp
CHECK_STR_EQ(first.provider_used, "epo_global_dossier");
CHECK_STR_EQ(first.latest_applicant_activity, "2026-02-18");
CHECK(first.rate_limited_upstream);
CHECK_STR_EQ(first.terminal_state, "REEXAMINATION_PENDING");
CHECK_STR_EQ(first.reexamination_state, "REQUEST_PERIOD");

auto states = db.GetDossierSyncStates();
CHECK_STR_EQ(states.front().latest_applicant_activity, "2026-02-18");
CHECK_STR_EQ(states.front().terminal_state, "REEXAMINATION_PENDING");
CHECK_STR_EQ(states.front().reexamination_state, "REQUEST_PERIOD");
```

- [x] **步骤 3：新增旧协议兼容测试**

新增 `TestLegacyRemoteCaseProtocolDefaults`，解析只包含 `ok`、`code`、`provider_used` 和空 `documents` 的 JSON，验证新增字符串为空、布尔值为假，并且解析成功。

- [x] **步骤 4：运行测试并确认红灯**

运行：

```bash
cmake --build build -j8 --target patx_web_dossier_tests
```

预期：编译因 `RemoteCaseResult` 和 `CaseSyncReport` 缺少新增成员而失败，错误明确指向本任务的新断言。

- [x] **步骤 5：提交协议契约测试**

```bash
git add tests/test_web_dossier_rules.cpp
git commit -m "test(审查提醒): 定义统一案件结果协议"
```

### 任务 4：实现统一案件协议和状态传播

**文件：**

- 修改：`include/web_dossier.hpp:79-125`
- 修改：`src/cpp/web_dossier_rules.cpp:378-540`
- 测试：`tests/test_web_dossier_rules.cpp`

- [x] **步骤 1：扩展公开协议结构**

在 `RemoteCaseResult` 增加：

```cpp
std::string latest_applicant_activity;
bool rate_limited_upstream = false;
std::string terminal_state;
std::string reexamination_state;
```

在 `CaseSyncReport` 增加同名字段，并增加：

```cpp
std::string provider_used;
```

- [x] **步骤 2：解析新增 JSON 字段并保持旧协议兼容**

在 `ParseRemoteCaseResultJson` 使用带默认值的 `value`：

```cpp
out.latest_applicant_activity = parsed.value("latest_applicant_activity", "");
out.rate_limited_upstream = parsed.value("rate_limited_upstream", false);
out.terminal_state = parsed.value("terminal_state", "");
out.reexamination_state = parsed.value("reexamination_state", "");
```

不要要求字段存在，不改变未知字段处理方式。

- [x] **步骤 3：传播报告并持久化案件状态**

在 `ApplyRemoteCaseResult` 初始化阶段复制案件级字段：

```cpp
report.provider_used = remote.provider_used;
report.latest_applicant_activity = remote.latest_applicant_activity;
report.rate_limited_upstream = remote.rate_limited_upstream;
report.terminal_state = remote.terminal_state;
report.reexamination_state = remote.reexamination_state;

state.latest_applicant_activity = remote.latest_applicant_activity;
state.terminal_state = remote.terminal_state;
state.reexamination_state = remote.reexamination_state;
```

不在此阶段自动填写 OA `response_date`，也不修改 `Patent.application_status`。

- [x] **步骤 4：运行协议和完整 CTest**

运行：

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

预期：4/4 CTest 通过。

- [x] **步骤 5：提交协议实现**

```bash
git add include/web_dossier.hpp src/cpp/web_dossier_rules.cpp
git commit -m "feat(审查提醒): 统一案件级同步协议"
```

### 任务 5：全量验证、计划勾选和 GitHub 备份

**文件：**

- 修改：`docs/superpowers/plans/2026-10-02-cn-prosecution-integration-foundation.md`

- [x] **步骤 1：建立隔离 Python 测试环境**

测试环境放在已忽略的 `build/python-test-env`：

```bash
python3 -m venv build/python-test-env
build/python-test-env/bin/python -m pip install pytest
```

- [x] **步骤 2：运行完整验证**

依次运行：

```bash
cargo test
cmake --build build -j8
ctest --test-dir build --output-on-failure
cd tools
../build/python-test-env/bin/python -m pytest web_dossier/tests -q
```

预期：所有命令退出码 0；CTest 4/4 通过；Python 测试无失败。

- [x] **步骤 3：检查数据库版本、文档和工作区**

运行：

```bash
rg -n "kSchemaVersionCurrent|SchemaVersion\(\), 5|schema.*v5" \
  include src tests README.md docs/superpowers/specs \
  docs/superpowers/plans/2026-10-02-cn-prosecution-integration-foundation.md
git diff --check
git status --short
```

确认没有把仍表示“当前版本”的 5 留在代码和断言中；历史 v5 fixture 和双 v5 迁移说明应保留。

- [x] **步骤 4：勾选计划并提交验证记录**

将本计划已执行步骤全部改成 `[x]`，然后：

```bash
git add docs/superpowers/plans/2026-10-02-cn-prosecution-integration-foundation.md
git commit -m "docs(整合): 记录v6底座实现验证"
```

- [x] **步骤 5：推送 GitHub 备份**

```bash
git push origin codex/cn-prosecution-integration
```

推送后使用 `git status --short --branch` 确认本地分支与远端同步且工作区干净。

## 验证结果

2026-10-04 在分支 `codex/cn-prosecution-integration` 上完成全量验证：

- `cargo test`：10 个测试通过，0 个失败。
- `cmake --build build -j8`：构建成功。
- `ctest --test-dir build --output-on-failure`：4/4 个测试通过，0 个失败。
- `cd tools && ../build/python-test-env/bin/python -m pytest web_dossier/tests -q`：278 个测试通过，0 个失败。
- Python 测试环境：`build/python-test-env/bin/python`（Python 3.14.4、pytest 9.1.1）；该隔离环境安装了 `tools/web_dossier/requirements.txt` 中声明的依赖。
- `git diff --check`：通过，无空白错误。
- 版本语义检索确认当前模式版本为 v6；检索结果中的 v5 仅用于历史迁移 fixture、迁移日志和双 v5 兼容性说明。
