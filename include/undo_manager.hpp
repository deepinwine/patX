#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

class Database;

class UndoManager {
public:
    UndoManager();
    ~UndoManager();

    void SetDatabase(sqlite3* db);

    void BeginBatch();
    void LogOperation(const std::string& operation, const std::string& table_name,
                     int record_id, const std::string& old_data,
                     const std::string& new_data = "");

    // Undo the most recent batch. Returns number of records reverted.
    int Undo();

    bool CanUndo() const;
    std::string GetLastError() const;

private:
    sqlite3* db_ = nullptr;
    int64_t current_batch_id_ = 0;
    std::string last_error_;

    bool RestoreDeletedRecord(const std::string& table_name, const std::string& old_data);
    bool RestoreUpdatedRecord(const std::string& table_name, int record_id, const std::string& old_data);

    std::string SerializePatentToInsert(const std::string& json);
    std::string SerializeOARecordToInsert(const std::string& json);
    std::string SerializeGenericToInsert(const std::string& table, const std::string& json);
    std::string SerializeGenericToUpdate(const std::string& table, const std::string& json, int id);

    // Simple JSON helpers (no external dep)
    static std::string JsonGetString(const std::string& json, const std::string& key);
    static int JsonGetInt(const std::string& json, const std::string& key, int default_val = 0);
};
