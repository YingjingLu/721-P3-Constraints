#include "execution/table_generator/table_generator.h"

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "execution/util/bit_util.h"
#include "loggers/execution_logger.h"
#include "parser/expression/column_value_expression.h"
#include "storage/index/bwtree_index.h"
#include "storage/index/index_builder.h"

namespace terrier::execution::sql {
template <typename T>
T *TableGenerator::CreateNumberColumnData(ColumnInsertMeta *col_meta, uint32_t num_vals) {
  auto *val = new T[num_vals];
  static uint64_t rotate_counter = 0;

  switch (col_meta->dist_) {
    case Dist::Uniform: {
      std::mt19937 generator{};
      std::uniform_int_distribution<T> distribution(static_cast<T>(col_meta->min_), static_cast<T>(col_meta->max_));

      for (uint32_t i = 0; i < num_vals; i++) {
        val[i] = distribution(generator);
      }

      break;
    }
    case Dist::Serial: {
      for (uint32_t i = 0; i < num_vals; i++) {
        val[i] = static_cast<T>(col_meta->counter_);
        col_meta->counter_++;
      }
      break;
    }
    case Dist::Rotate: {
      for (uint32_t i = 0; i < num_vals; i++) {
        if (rotate_counter > col_meta->max_) {
          rotate_counter = col_meta->min_;
        }
        val[i] = static_cast<T>(rotate_counter);
        rotate_counter++;
      }
      std::shuffle(&val[0], &val[num_vals], std::mt19937(std::random_device()()));
      break;
    }
    default:
      throw std::runtime_error("Implement me!");
  }

  return val;
}

bool *TableGenerator::CreateBooleanColumnData(ColumnInsertMeta *col_meta, uint32_t num_vals) {
  auto *val = new bool[num_vals];

  switch (col_meta->dist_) {
    case Dist::Uniform: {
      std::mt19937 generator{};
      std::uniform_int_distribution<int16_t> distribution(0, 1);
      for (uint32_t i = 0; i < num_vals; i++) {
        val[i] = distribution(generator) % 2 == 0;
      }
      break;
    }
    case Dist::Serial: {
      // Split the false/true values by half
      uint32_t half = num_vals / 2;
      for (uint32_t i = 0; i < num_vals; i++) {
        val[i] = (i >= half);
      }
      break;
    }
    default:
      throw std::runtime_error("Unsupported distribution type for boolean columns");
  }

  return val;
}

// Generate column data
std::pair<byte *, uint32_t *> TableGenerator::GenerateColumnData(ColumnInsertMeta *col_meta, uint32_t num_rows) {
  // Create data
  byte *col_data = nullptr;
  switch (col_meta->type_) {
    case type::TypeId::BOOLEAN: {
      col_data = reinterpret_cast<byte *>(CreateBooleanColumnData(col_meta, num_rows));
      break;
    }
    case type::TypeId::TINYINT: {
      col_data = reinterpret_cast<byte *>(CreateNumberColumnData<int8_t>(col_meta, num_rows));
      break;
    }
    case type::TypeId::SMALLINT: {
      col_data = reinterpret_cast<byte *>(CreateNumberColumnData<int16_t>(col_meta, num_rows));
      break;
    }
    case type::TypeId::INTEGER: {
      col_data = reinterpret_cast<byte *>(CreateNumberColumnData<int32_t>(col_meta, num_rows));
      break;
    }
    case type::TypeId::BIGINT:
    case type::TypeId::DECIMAL: {
      col_data = reinterpret_cast<byte *>(CreateNumberColumnData<int64_t>(col_meta, num_rows));
      break;
    }
    default: {
      throw std::runtime_error("Implement me!");
    }
  }

  // Create bitmap
  uint32_t *null_bitmap = nullptr;
  TERRIER_ASSERT(num_rows != 0, "Cannot have 0 rows.");
  uint64_t num_words = util::BitUtil::Num32BitWordsFor(num_rows);
  null_bitmap = new uint32_t[num_words];
  util::BitUtil::Clear(null_bitmap, num_rows);
  if (col_meta->nullable_) {
    std::mt19937 generator;
    std::bernoulli_distribution coin(0.1);
    for (uint32_t i = 0; i < num_rows; i++) {
      if (coin(generator)) util::BitUtil::Set(null_bitmap, i);
    }
  }

  return {col_data, null_bitmap};
}

// Fill a given table according to its metadata
void TableGenerator::FillTable(catalog::table_oid_t table_oid, common::ManagedPointer<storage::SqlTable> table,
                               const catalog::Schema &schema, TableInsertMeta *table_meta) {
  uint32_t batch_size = 10000;
  uint32_t num_batches =
      table_meta->num_rows_ / batch_size + static_cast<uint32_t>(table_meta->num_rows_ % batch_size != 0);
  std::vector<catalog::col_oid_t> table_cols;
  for (const auto &col : schema.GetColumns()) {
    table_cols.emplace_back(col.Oid());
  }
  auto pri = table->InitializerForProjectedRow(table_cols);
  auto offset_map = table->ProjectionMapForOids(table_cols);
  uint32_t vals_written = 0;

  std::vector<uint16_t> offsets;
  for (const auto &col_meta : table_meta->col_meta_) {
    const auto &table_col = schema.GetColumn(col_meta.name_);
    offsets.emplace_back(offset_map[table_col.Oid()]);
  }

  for (uint32_t i = 0; i < num_batches; i++) {
    std::vector<std::pair<byte *, uint32_t *>> alloc_buffers;
    std::vector<std::pair<byte *, uint32_t *>> column_data;

    // Generate column data for all columns
    uint32_t num_vals = std::min(batch_size, table_meta->num_rows_ - (i * batch_size));
    TERRIER_ASSERT(num_vals != 0, "Can't have empty columns.");
    for (auto &col_meta : table_meta->col_meta_) {
      if (col_meta.is_clone_) {
        column_data.emplace_back(column_data[col_meta.clone_idx_]);
      } else {
        column_data.emplace_back(GenerateColumnData(&col_meta, num_vals));
        alloc_buffers.emplace_back(column_data.back());
      }
    }

    // Insert into the table
    for (uint32_t j = 0; j < num_vals; j++) {
      auto *const redo = exec_ctx_->GetTxn()->StageWrite(exec_ctx_->DBOid(), table_oid, pri);
      for (uint16_t k = 0; k < column_data.size(); k++) {
        auto offset = offsets[k];
        if (table_meta->col_meta_[k].nullable_ && util::BitUtil::Test(column_data[k].second, j)) {
          redo->Delta()->SetNull(offset);
        } else {
          byte *data = redo->Delta()->AccessForceNotNull(offset);
          uint32_t elem_size = type::TypeUtil::GetTypeSize(table_meta->col_meta_[k].type_);
          std::memcpy(data, column_data[k].first + j * elem_size, elem_size);
        }
      }
      table->Insert(exec_ctx_->GetTxn(), redo);
      vals_written++;
    }

    // Free allocated buffers
    for (const auto &col_data : alloc_buffers) {
      delete[] col_data.first;
      delete[] col_data.second;
    }
  }
  // EXECUTION_LOG_INFO("Wrote {} tuples into table {}.", vals_written, table_meta->name_);
}

void TableGenerator::CreateTable(TableInsertMeta *metadata) {
  // Create Schema.
  std::vector<catalog::Schema::Column> cols;
  for (const auto &col_meta : metadata->col_meta_) {
    if (col_meta.type_ != type::TypeId::VARCHAR) {
      cols.emplace_back(col_meta.name_, col_meta.type_, col_meta.nullable_, DummyCVE());
    } else {
      cols.emplace_back(col_meta.name_, col_meta.type_, 100, col_meta.nullable_, DummyCVE());
    }
  }
  catalog::Schema tmp_schema(cols);
  // Create Table.
  auto table_oid = exec_ctx_->GetAccessor()->CreateTable(ns_oid_, metadata->name_, tmp_schema);
  auto &schema = exec_ctx_->GetAccessor()->GetSchema(table_oid);
  auto *tmp_table = new storage::SqlTable(common::ManagedPointer(store_), schema);
  exec_ctx_->GetAccessor()->SetTablePointer(table_oid, tmp_table);
  auto table = exec_ctx_->GetAccessor()->GetTable(table_oid);
  FillTable(table_oid, table, schema, metadata);
}

void TableGenerator::CreateIndex(IndexInsertMeta *index_meta) {
  storage::index::IndexBuilder index_builder;

  // Get Corresponding Table
  auto table_oid = exec_ctx_->GetAccessor()->GetTableOid(ns_oid_, index_meta->table_name_);
  auto table = exec_ctx_->GetAccessor()->GetTable(table_oid);
  auto &table_schema = exec_ctx_->GetAccessor()->GetSchema(table_oid);

  // Create Index Schema
  std::vector<catalog::IndexSchema::Column> index_cols;
  for (const auto &col_meta : index_meta->cols_) {
    const auto &table_col = table_schema.GetColumn(col_meta.table_col_name_);
    parser::ColumnValueExpression col_expr(table_oid, table_col.Oid(), table_col.Type());
    if (table_col.Type() != type::TypeId::VARCHAR) {
      index_cols.emplace_back(col_meta.name_, col_meta.type_, col_meta.nullable_, col_expr);
    } else {
      index_cols.emplace_back(col_meta.name_, col_meta.type_, 100, col_meta.nullable_, col_expr);
    }
  }
  catalog::IndexSchema tmp_index_schema{index_cols, storage::index::IndexType::BWTREE, false, false, false, false};
  // Create Index
  auto index_oid = exec_ctx_->GetAccessor()->CreateIndex(ns_oid_, table_oid, index_meta->index_name_, tmp_index_schema);
  auto &index_schema = exec_ctx_->GetAccessor()->GetIndexSchema(index_oid);
  index_builder.SetKeySchema(index_schema);
  auto *tmp_index = index_builder.Build();
  exec_ctx_->GetAccessor()->SetIndexPointer(index_oid, tmp_index);

  auto index = exec_ctx_->GetAccessor()->GetIndex(index_oid);
  // Fill up the index
  FillIndex(index, index_schema, *index_meta, table, table_schema);
}

void TableGenerator::GenerateTestTables(bool is_mini_runner) {
  /**
   * This array configures each of the test tables. Each able is configured
   * with a name, size, and schema. We also configure the columns of the table. If
   * you add a new table, set it up here.
   */
  std::vector<TableInsertMeta> insert_meta{
      // The empty table
      {"empty_table", 0, {{"colA", type::TypeId::INTEGER, false, Dist::Serial, 0, 0}}},

      // Table 1
      {"test_1",
       TEST1_SIZE,
       {{"colA", type::TypeId::INTEGER, false, Dist::Serial, 0, 0},
        {"colB", type::TypeId::INTEGER, false, Dist::Uniform, 0, 9},
        {"colC", type::TypeId::INTEGER, false, Dist::Uniform, 0, 9999},
        {"colD", type::TypeId::INTEGER, false, Dist::Uniform, 0, 99999}}},

      // Table 2
      {"test_2",
       TEST2_SIZE,
       {{"col1", type::TypeId::SMALLINT, false, Dist::Serial, 0, 0},
        {"col2", type::TypeId::INTEGER, true, Dist::Uniform, 0, 9},
        {"col3", type::TypeId::BIGINT, false, Dist::Uniform, 0, common::Constants::K_DEFAULT_VECTOR_SIZE},
        {"col4", type::TypeId::INTEGER, true, Dist::Uniform, 0, 2 * common::Constants::K_DEFAULT_VECTOR_SIZE}}},

      // Empty table with two columns
      {"empty_table2",
       0,
       {{"colA", type::TypeId::INTEGER, false, Dist::Serial, 0, 0},
        {"colB", type::TypeId::BOOLEAN, false, Dist::Uniform, 0, 0}}},

      // Table with all types
      {"all_types_table",
       TABLE_ALLTYPES_SIZE,
       {// {"varchar_col", type::TypeId::VARCHAR, false, Dist::Serial, 0, 0},
        // {"date_col", type::TypeId::DATE, false, Dist::Serial, 0, 0},
        // {"real_col", type::TypeId::DECIMAL, false, Dist::Serial, 0, 0},
        {"bool_col", type::TypeId::BOOLEAN, false, Dist::Serial, 0, 0},
        {"tinyint_col", type::TypeId::TINYINT, false, Dist::Uniform, 0, 127},
        {"smallint_col", type::TypeId::SMALLINT, false, Dist::Serial, 0, 1000},
        {"int_col", type::TypeId::INTEGER, false, Dist::Uniform, 0, 0},
        {"bigint_col", type::TypeId::BIGINT, false, Dist::Uniform, 0, 1000}}},

      // Empty table with columns of various types
      {"all_types_empty_table",
       0,
       {{"varchar_col", type::TypeId::VARCHAR, false, Dist::Serial, 0, 0},
        {"date_col", type::TypeId::DATE, false, Dist::Serial, 0, 0},
        {"real_col", type::TypeId::DECIMAL, false, Dist::Serial, 0, 0},
        {"bool_col", type::TypeId::BOOLEAN, false, Dist::Serial, 0, 0},
        {"tinyint_col", type::TypeId::TINYINT, false, Dist::Uniform, 0, 127},
        {"smallint_col", type::TypeId::SMALLINT, false, Dist::Serial, 0, 1000},
        {"int_col", type::TypeId::INTEGER, false, Dist::Uniform, 0, 0},
        {"bigint_col", type::TypeId::BIGINT, false, Dist::Uniform, 0, 1000}}},
  };

  if (is_mini_runner) {
    auto mini_runner_table_metas = GenerateMiniRunnerTableMetas();
    insert_meta.insert(insert_meta.end(), mini_runner_table_metas.begin(), mini_runner_table_metas.end());
  }

  for (auto &table_meta : insert_meta) {
    CreateTable(&table_meta);
  }

  InitTestIndexes();
}

void TableGenerator::GenerateMiniRunnerIndexes() {
  std::vector<TableInsertMeta> table_metas;
  std::vector<uint32_t> idx_key = {1, 4, 8, 15};
  std::vector<uint32_t> row_nums = {1, 100, 1000, 10000};
  std::vector<type::TypeId> types = {type::TypeId::INTEGER};
  for (auto row_num : row_nums) {
    for (type::TypeId type : types) {
      auto table_name = GenerateTableIndexName(type, row_num);
      std::vector<ColumnInsertMeta> col_metas;
      for (uint32_t j = 1; j <= 15; j++) {
        std::stringstream col_name;
        col_name << "col" << j;
        col_metas.emplace_back(col_name.str(), type, false, Dist::Serial, 0, 0);
      }

      auto meta = TableInsertMeta(table_name, row_num, col_metas);
      CreateTable(&meta);

      // Create Index Schema
      for (auto key_num : idx_key) {
        std::stringstream idx_name;
        idx_name << table_name << "_index_" << key_num;
        auto idx_name_str = idx_name.str();

        std::vector<std::string> index_strs;
        std::vector<IndexColumn> idx_meta_cols;
        index_strs.reserve(key_num);
        idx_meta_cols.reserve(key_num);
        for (uint32_t j = 1; j <= key_num; j++) {
          std::stringstream col_name;
          col_name << "col" << j;

          index_strs.push_back(col_name.str());
          idx_meta_cols.emplace_back(index_strs.back().c_str(), type, false, index_strs.back().c_str());
        }

        auto meta = IndexInsertMeta(idx_name_str.c_str(), table_name.c_str(), idx_meta_cols);
        CreateIndex(&meta);
      }
    }
  }
}

void TableGenerator::FillIndex(common::ManagedPointer<storage::index::Index> index,
                               const catalog::IndexSchema &index_schema, const IndexInsertMeta &index_meta,
                               common::ManagedPointer<storage::SqlTable> table, const catalog::Schema &table_schema) {
  // Initialize table projected row
  std::vector<catalog::col_oid_t> table_cols;
  for (const auto &col : table_schema.GetColumns()) {
    table_cols.emplace_back(col.Oid());
  }
  auto table_pri = table->InitializerForProjectedRow(table_cols);
  auto table_offset_map = table->ProjectionMapForOids(table_cols);
  byte *table_buffer = common::AllocationUtil::AllocateAligned(table_pri.ProjectedRowSize());
  auto table_pr = table_pri.InitializeRow(table_buffer);

  // Initialize index projected row
  auto index_pri = index->GetProjectedRowInitializer();
  byte *index_buffer = common::AllocationUtil::AllocateAligned(index_pri.ProjectedRowSize());
  auto index_pr = index_pri.InitializeRow(index_buffer);

  std::vector<uint16_t> table_offsets;
  for (const auto &index_col_meta : index_meta.cols_) {
    const auto &table_col = table_schema.GetColumn(index_col_meta.table_col_name_);
    table_offsets.emplace_back(table_offset_map[table_col.Oid()]);
  }

  uint32_t num_inserted = 0;
  for (const storage::TupleSlot &slot : *table) {
    // Get table data
    table->Select(exec_ctx_->GetTxn(), slot, table_pr);
    // Fill up the index data
    for (uint32_t index_col_idx = 0; index_col_idx < index_meta.cols_.size(); index_col_idx++) {
      // Get the offset of this column in the table
      auto table_offset = table_offsets[index_col_idx];
      // Get the offset of this column in the index
      const auto &index_col = index_schema.GetColumn(index_meta.cols_[index_col_idx].name_);
      uint16_t index_offset = index->GetKeyOidToOffsetMap().at(index_col.Oid());
      // Check null and write bytes.
      if (index_col.Nullable() && table_pr->IsNull(table_offset)) {
        index_pr->SetNull(index_offset);
      } else {
        byte *index_data = index_pr->AccessForceNotNull(index_offset);
        auto type_size = storage::AttrSizeBytes(type::TypeUtil::GetTypeSize(index_col.Type()));
        std::memcpy(index_data, table_pr->AccessForceNotNull(table_offset), type_size);
      }
    }
    // Insert tuple into the index
    index->Insert(exec_ctx_->GetTxn(), *index_pr, slot);
    num_inserted++;
  }
  // Cleanup
  delete[] table_buffer;
  delete[] index_buffer;
  // EXECUTION_LOG_INFO("Wrote {} tuples into index {}.", num_inserted, index_meta.index_name_);
}

std::vector<TableGenerator::TableInsertMeta> TableGenerator::GenerateMiniRunnerTableMetas() {
  std::vector<TableInsertMeta> table_metas;
  std::vector<uint32_t> row_nums = {1,    3,    5,     7,     10,    50,     100,    500,    1000,
                                    2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000};
  std::vector<type::TypeId> types = {type::TypeId::INTEGER};
  for (int col_num = 31; col_num <= 31; col_num++) {
    for (uint32_t row_num : row_nums) {
      // Cardinality of the last column
      std::vector<uint32_t> cardinalities;
      // Generate different cardinalities exponentially
      for (uint32_t i = 1; i < row_num; i *= 2) cardinalities.emplace_back(i);
      cardinalities.emplace_back(row_num);

      for (uint32_t cardinality : cardinalities) {
        for (type::TypeId type : types) {
          std::vector<ColumnInsertMeta> col_metas;
          for (int j = 1; j <= col_num; j++) {
            std::stringstream col_name;
            col_name << "col" << j;
            if (j == 1) {
              // The first column would be serial
              col_metas.emplace_back(col_name.str(), type, false, Dist::Serial, 0, 0);
            } else if (j == 15) {
              // The 15th column is related to the cardinality
              col_metas.emplace_back(col_name.str(), type, false, Dist::Rotate, 0, cardinality);
            } else if (j > 15) {
              // Columns after the 15th column duplicate the 15th column
              col_metas.emplace_back(col_metas[14], col_name.str(), 14);
            } else {
              // All the rest of the columns are uniformly distributed
              col_metas.emplace_back(col_name.str(), type, false, Dist::Uniform, 0, row_num - 1);
            }
          }
          table_metas.emplace_back(GenerateTableName(type, col_num, row_num, cardinality), row_num, col_metas);
        }
      }
    }
  }
  return table_metas;
}

void TableGenerator::InitTestIndexes() {
  /**
   * This array configures indexes. To add an index, modify this array
   */
  std::vector<IndexInsertMeta> index_metas = {
      // The empty table
      {"index_empty", "empty_table", {{"index_colA", type::TypeId::INTEGER, false, "colA"}}},

      // Table 1
      {"index_1", "test_1", {{"index_colA", type::TypeId::INTEGER, false, "colA"}}},

      // Table 2: one col
      {"index_2", "test_2", {{"index_col1", type::TypeId::SMALLINT, false, "col1"}}},

      // Table 2: two cols
      {"index_2_multi",
       "test_2",
       {{"index_col1", type::TypeId::SMALLINT, false, "col1"}, {"index_col2", type::TypeId::INTEGER, true, "col2"}}},

      // Index on a varchar
      {"varchar_index", "all_types_empty_table", {{"index_varchar_col", type::TypeId::VARCHAR, false, "varchar_col"}}}};

  for (auto &index_meta : index_metas) {
    CreateIndex(&index_meta);
  }
}
}  // namespace terrier::execution::sql
