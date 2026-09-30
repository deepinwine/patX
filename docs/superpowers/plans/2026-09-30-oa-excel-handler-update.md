# OA Excel 处理人更新实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** OA Excel 导入能够精确匹配已有 OA、写入新处理人，并通过导入前预检安全处理处理人冲突和重复记录。

**架构：** `ExcelIO` 先把 OA 工作表解析为内存行，再按「案件编号＋规范化 OA 类型＋发文日」生成导入计划。GUI 通过一次回调展示计划统计并返回保留、覆盖或取消策略；执行阶段只新增 OA 或修改唯一命中记录的 `handler`，其他人工字段保持不变。

**技术栈：** C++17、OpenXLSX、SQLite、wxWidgets、CMake/CTest、自有测试注册器。

---

## 文件结构

- 修改 `include/excel_io.hpp`：声明 OA 预检统计、冲突策略、回调和扩展后的导入结果。
- 修改 `src/cpp/io/excel_io.cpp`：解析 OA 行、生成计划、调用预检回调并执行安全更新。
- 修改 `src/cpp/main_gui.cpp`：展示 OA 预检结果，收集整批冲突策略，并扩展完成摘要。
- 修改 `tests/test_excel.cpp`：使用真实 XLSX 覆盖新增、重复导入、处理人补入、冲突和不唯一匹配。
- 修改 `README.md`：记录 OA Excel 精确匹配和人工字段保护规则。

## 任务 1：定义 OA 导入预检契约

**文件：**
- 修改：`include/excel_io.hpp:25-105`
- 修改：`tests/test_excel.cpp`

- [ ] **步骤 1：编写真实 XLSX 的失败测试**

在 `tests/test_excel.cpp` 增加创建 OA 工作簿的辅助函数，并测试回调收到新增计划、全新处理人成功写入：

```cpp
static std::string WriteOAWorkbook(
    const std::vector<std::vector<std::string>>& rows,
    const std::string& suffix) {
    std::string path = TempDbPath(suffix + ".xlsx");
    ExcelIO io;
    ExportTable table;
    table.sheet_name = "OA";
    table.headers = {"编号", "专利名称", "OA类型", "发文日", "官方期限", "处理人"};
    table.rows = rows;
    CHECK(io.ExportXlsx(table, path));
    return path;
}

TEST(oa_excel_import_previews_and_adds_new_handler) {
    Database db(":memory:");
    ExcelIO io;
    auto path = WriteOAWorkbook({
        {"GK-OA-1", "测试专利", "第一次审查意见通知书",
         "2026-09-01", "2027-01-01", "新处理人"}
    }, "oa_handler_add");

    OAImportPreview preview;
    int review_calls = 0;
    auto result = io.ImportPatents(path, db, nullptr,
        [&](const OAImportPreview& value) {
            preview = value;
            review_calls++;
            return OAHandlerConflictPolicy::PreserveExisting;
        });

    CHECK_EQ(review_calls, 1);
    CHECK_EQ(preview.added, 1);
    CHECK_EQ(result.added, 1);
    auto rows = db.GetOAByPatent("GK-OA-1");
    CHECK_EQ(rows.size(), 1u);
    CHECK_STR_EQ(rows[0].handler, "新处理人");
    std::filesystem::remove(path);
}
```

- [ ] **步骤 2：运行测试并确认因 API 缺失而失败**

运行：

```bash
cmake --build build -j4
```

预期：编译失败，提示缺少 `OAImportPreview`、`OAHandlerConflictPolicy` 和四参数 `ImportPatents()`。

- [ ] **步骤 3：声明最小公共契约**

在 `include/excel_io.hpp` 增加：

```cpp
enum class OAHandlerConflictPolicy {
    PreserveExisting,
    OverwriteWithExcel,
    CancelImport,
};

struct OAImportPreview {
    int added = 0;
    int handler_updates = 0;
    int unchanged = 0;
    int handler_conflicts = 0;
    int match_conflicts = 0;
};

using OAImportReviewCallback =
    std::function<OAHandlerConflictPolicy(const OAImportPreview&)>;
```

扩展 `ImportResult`：

```cpp
int handler_conflicts = 0;
int match_conflicts = 0;
bool cancelled = false;
```

给 `ImportPatents()` 和 `ImportPatentsFromXlsx()` 增加末尾可选参数：

```cpp
OAImportReviewCallback oa_review_callback = nullptr
```

- [ ] **步骤 4：重新构建，确认只剩实现缺失或断言失败**

运行：

```bash
cmake --build build -j4
ctest --test-dir build -R patx_tests --output-on-failure
```

预期：构建成功，但新增测试因回调未调用而失败。

## 任务 2：实现精确匹配、预检和最小写入

**文件：**
- 修改：`src/cpp/io/excel_io.cpp:458-930`
- 修改：`tests/test_excel.cpp`

- [ ] **步骤 1：补充重复导入和空处理人补入的失败测试**

```cpp
TEST(oa_excel_import_matches_without_duplicate_and_fills_empty_handler) {
    Database db(":memory:");
    OARecord existing;
    existing.geke_code = "GK-OA-2";
    existing.oa_type = "一通";
    existing.issue_date = "2026-09-02";
    existing.writer = "人工撰写人";
    int id = db.InsertOA(existing);

    auto path = WriteOAWorkbook({
        {"GK-OA-2", "测试专利", "第一次审查意见通知书",
         "2026-09-02", "2027-01-02", "李四"}
    }, "oa_handler_fill");
    ExcelIO io;
    auto result = io.ImportPatents(path, db);

    CHECK_EQ(result.added, 0);
    CHECK_EQ(result.updated, 1);
    CHECK_EQ(db.GetOAByPatent("GK-OA-2").size(), 1u);
    auto updated = db.GetOAById(id);
    CHECK_STR_EQ(updated.handler, "李四");
    CHECK_STR_EQ(updated.writer, "人工撰写人");

    auto repeated = io.ImportPatents(path, db);
    CHECK_EQ(repeated.added, 0);
    CHECK_EQ(repeated.updated, 0);
    CHECK_EQ(repeated.skipped, 1);
    CHECK_EQ(db.GetOAByPatent("GK-OA-2").size(), 1u);
    std::filesystem::remove(path);
}
```

- [ ] **步骤 2：运行新增测试并确认重复记录失败**

运行：

```bash
cmake --build build -j4
ctest --test-dir build -R patx_tests --output-on-failure
```

预期：测试失败，现有实现返回 `added == 1` 并产生第二条 OA。

- [ ] **步骤 3：实现内存导入计划**

在 `excel_io.cpp` 的匿名命名空间新增内部类型：

```cpp
enum class OAImportAction {
    Insert,
    UpdateEmptyHandler,
    Unchanged,
    HandlerConflict,
    MatchConflict,
};

struct PlannedOAImport {
    OARecord incoming;
    OAImportAction action = OAImportAction::Insert;
    int existing_id = 0;
};

struct OAImportPlan {
    std::vector<PlannedOAImport> rows;
    OAImportPreview preview;
};
```

实现计划生成函数。它调用 `db.GetOAByPatent(incoming.geke_code)`，只在 `oa_type` 和 `issue_date` 都非空时，用以下条件筛选：

```cpp
NormalizeOaTypeCn(existing.oa_type) == NormalizeOaTypeCn(incoming.oa_type) &&
existing.issue_date == incoming.issue_date
```

分类规则：零命中为 `Insert`；多命中为 `MatchConflict`；唯一命中后依据新旧 `handler` 分类为自动补入、未变化或处理人冲突。

- [ ] **步骤 4：在 OA 工作表分支先解析、后确认、再执行**

把当前逐行 `InsertOA()` 改为：

```cpp
std::vector<OARecord> imported_rows = ParseOARows(...);
OAImportPlan plan = BuildOAImportPlan(imported_rows, db);
OAHandlerConflictPolicy policy = oa_review_callback
    ? oa_review_callback(plan.preview)
    : OAHandlerConflictPolicy::PreserveExisting;

if (policy == OAHandlerConflictPolicy::CancelImport) {
    result.cancelled = true;
    sheet_skipped += static_cast<int>(plan.rows.size());
} else {
    ApplyOAImportPlan(plan, policy, db, sheet_added, sheet_updated,
                      sheet_skipped, result);
}
```

`ApplyOAImportPlan()` 的更新路径必须先 `GetOAById()`，只修改 `handler` 后调用 `UpdateOA()`；不得把 Excel 行整体覆盖到已有记录。

- [ ] **步骤 5：运行 OA 导入测试并确认通过**

运行：

```bash
cmake --build build -j4
ctest --test-dir build -R patx_tests --output-on-failure
```

预期：新增、补入、重复导入测试全部通过。

- [ ] **步骤 6：提交核心导入行为**

```bash
git add include/excel_io.hpp src/cpp/io/excel_io.cpp tests/test_excel.cpp
git commit -m "feat(OA导入): 精确匹配并更新处理人"
```

## 任务 3：补齐冲突保护与取消边界

**文件：**
- 修改：`tests/test_excel.cpp`
- 修改：`src/cpp/io/excel_io.cpp`

- [ ] **步骤 1：编写保留、覆盖和空值保护的失败测试**

测试建立处理人为「旧处理人」的已有 OA，并分别导入「新处理人」或空值：

```cpp
auto preserve = io.ImportPatents(path, db, nullptr,
    [](const OAImportPreview& preview) {
        CHECK_EQ(preview.handler_conflicts, 1);
        return OAHandlerConflictPolicy::PreserveExisting;
    });
CHECK_EQ(preserve.handler_conflicts, 1);
CHECK_STR_EQ(db.GetOAById(id).handler, "旧处理人");

auto overwrite = io.ImportPatents(path, db, nullptr,
    [](const OAImportPreview&) {
        return OAHandlerConflictPolicy::OverwriteWithExcel;
    });
CHECK_EQ(overwrite.updated, 1);
CHECK_STR_EQ(db.GetOAById(id).handler, "新处理人");
```

另建空处理人工作簿并断言已有值不被清空，`updated == 0`。

- [ ] **步骤 2：编写取消和不唯一匹配的失败测试**

取消测试断言回调返回 `CancelImport` 后，新增行和更新行均未写入。匹配冲突测试预先插入两条相同「案件编号＋OA 类型＋发文日」记录，断言：

```cpp
CHECK_EQ(preview.match_conflicts, 1);
CHECK_EQ(result.match_conflicts, 1);
CHECK_EQ(result.updated, 0);
CHECK_EQ(result.added, 0);
```

- [ ] **步骤 3：运行测试并确认冲突计数或取消语义失败**

运行：

```bash
cmake --build build -j4
ctest --test-dir build -R patx_tests --output-on-failure
```

预期：至少一个新增测试失败，失败原因是冲突策略、取消或多命中计数尚未完整实现。

- [ ] **步骤 4：完成策略执行和结果统计**

确保执行规则如下：

```cpp
case OAImportAction::HandlerConflict:
    result.handler_conflicts++;
    if (policy == OAHandlerConflictPolicy::OverwriteWithExcel) {
        OARecord existing = db.GetOAById(item.existing_id);
        existing.handler = item.incoming.handler;
        if (db.UpdateOA(existing.id, existing)) sheet_updated++;
    } else {
        sheet_skipped++;
    }
    break;
case OAImportAction::MatchConflict:
    result.match_conflicts++;
    sheet_skipped++;
    break;
```

取消必须发生在 `ApplyOAImportPlan()` 之前，因此该 OA 工作表没有任何写入。

- [ ] **步骤 5：运行核心回归测试**

运行：

```bash
cmake --build build -j4
ctest --test-dir build -R 'patx_tests|patx_web_dossier_tests' --output-on-failure
```

预期：两组测试全部通过，网页同步的人工字段保护测试保持通过。

- [ ] **步骤 6：提交冲突保护**

```bash
git add src/cpp/io/excel_io.cpp tests/test_excel.cpp
git commit -m "fix(OA导入): 保护处理人冲突与重复匹配"
```

## 任务 4：接入 GUI 预检确认和结果摘要

**文件：**
- 修改：`src/cpp/main_gui.cpp:2000-2033`

- [ ] **步骤 1：先构建纯函数化消息测试接口**

为避免直接测试 wxWidgets 对话框，在 `include/excel_io.hpp` 声明并在 `excel_io.cpp` 实现：

```cpp
std::string FormatOAImportPreview(const OAImportPreview& preview);
```

在 `tests/test_excel.cpp` 先写失败测试，断言消息同时包含「新增 2」「自动更新处理人 1」「处理人冲突 3」「匹配冲突 1」。

- [ ] **步骤 2：运行测试并确认格式化函数缺失**

运行：

```bash
cmake --build build -j4
```

预期：链接失败，提示缺少 `FormatOAImportPreview()`。

- [ ] **步骤 3：实现格式化函数并接入 GUI**

`ImportExcel()` 调用扩展后的 `ImportPatents()`：

```cpp
auto result = excel.ImportPatents(
    path, *db,
    [&progress](int current, int total) -> bool {
        progress.Update(std::min(current * 100 / std::max(total, 1), 99));
        return !progress.WasCancelled();
    },
    [this](const OAImportPreview& preview) {
        const wxString message = wxString::FromUTF8(
            FormatOAImportPreview(preview).c_str());
        if (preview.handler_conflicts > 0) {
            int answer = wxMessageBox(
                message + UTF8_STR("\n\n是：以 Excel 覆盖冲突处理人\n"
                                   "否：保留原处理人\n取消：不导入该 OA 工作表"),
                UTF8_STR("OA 导入预检"),
                wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
            if (answer == wxYES)
                return OAHandlerConflictPolicy::OverwriteWithExcel;
            if (answer == wxNO)
                return OAHandlerConflictPolicy::PreserveExisting;
            return OAHandlerConflictPolicy::CancelImport;
        }
        int answer = wxMessageBox(
            message + UTF8_STR("\n\n是否继续导入？"),
            UTF8_STR("OA 导入预检"),
            wxYES_NO | wxICON_QUESTION, this);
        return answer == wxYES
            ? OAHandlerConflictPolicy::PreserveExisting
            : OAHandlerConflictPolicy::CancelImport;
    });
```

完成摘要追加：

```text
处理人冲突: N
匹配冲突: N
```

当 `result.cancelled` 为真时，显示「OA 工作表已取消，未写入该表数据」。

- [ ] **步骤 4：运行测试并构建 GUI**

运行：

```bash
cmake --build build -j4
ctest --test-dir build -R patx_tests --output-on-failure
```

预期：格式化测试通过，`patx` GUI 构建成功。

- [ ] **步骤 5：提交 GUI 集成**

```bash
git add include/excel_io.hpp src/cpp/io/excel_io.cpp src/cpp/main_gui.cpp tests/test_excel.cpp
git commit -m "feat(OA导入): 增加预检与处理人冲突选择"
```

## 任务 5：更新文档并完成全量验证

**文件：**
- 修改：`README.md`

- [ ] **步骤 1：补充用户文档**

在 OA 功能说明中明确：

```markdown
- OA Excel 导入按「案件编号＋OA 类型＋发文日」匹配已有记录；新 OA 可直接写入新处理人。
- 已有 OA 的处理人为空时自动补入；新旧处理人冲突时，导入前选择保留原值或以 Excel 覆盖。
- Excel 空值不会清空已有处理人；匹配到多条 OA 时跳过并报告冲突。
```

- [ ] **步骤 2：运行全部自动测试**

运行：

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest web_dossier/tests -q
cd ..
git diff --check
```

预期：C++ 测试全部通过，Python 278 项通过，`git diff --check` 无输出。

- [ ] **步骤 3：提交文档**

```bash
git add README.md
git commit -m "docs(OA导入): 补充处理人更新与冲突规则"
```

- [ ] **步骤 4：推送 GitHub 备份并核对远端**

运行：

```bash
git push origin codex/cn-prosecution-reminders
local_sha=$(git rev-parse HEAD)
remote_sha=$(git ls-remote --heads origin codex/cn-prosecution-reminders | awk '{print $1}')
test "$local_sha" = "$remote_sha"
git status --short --branch
```

预期：本地和远端 SHA 相同，工作区干净。
