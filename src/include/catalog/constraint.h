#include "catalog/postgres/check_constraint.h"
#include "catalog/postgres/exclusion_constraint.h"
#include "catalog/postgres/fk_constraint.h"
#include "catalog/postgres/pg_constraint.h"
#include "catalog/schema.h"
#include "parser/expression/abstract_expression.h"
#include "storage/projected_row.h"
#include "storage/sql_table.h"
#include "storage/storage_defs.h"
#include "transaction/transaction_context.h"

namespace terrier {
class StorageTestUtil;
class TpccPlanTest;
}  // namespace terrier

namespace terrier::catalog::postgres {

// A UNION of metadata structure for each of the constraints
using PGConstraintMetadata = union PGConstraintMetadata {
  struct UNIQUEMetadata;
  struct FKMetadata;
  struct CHECKMetadata;
  struct EXCLUSIONMetadata;
};

struct UNIQUEMetadata {};
struct FKMetadata {
  /************************ Foreign Key specific ******************************/
  // only applies to foreign keys, set to zero if other types of constraints
  col_oid_t confrelid_; /* relation id for the foreign key, set to zero if non-FK constraint */

  std::vector<col_oid_t> con_fk_src_;  // the column indcies in the current table for foreign key
  std::vector<col_oid_t> con_fk_ref_;  // the column indicies in the parent table that are reference for the foreign key

  /************************ unique key specific *********************************/
  std::vector<col_oid_t> con_unique_oid_;  // columns where the uniqueness apply

  /************************ Other undocumented attr ****************************/
  size_t conbin_;
  std::string consrc_;
};

struct CHECKMetadata {};

struct EXCLUSIONMetadata {};

/**
 * The class datastructure for the pg_constraint
 * Including the attribute for characterizing a constraint on a table
 * Currently support NOT NULL, FOREIGN KEY, UNIQUE
 *
 ********************* Nulti Column setup *********************
 * This claos includes support for multi column senario:
 * CREATE TABLE example (
    a integer,
    b integer,
    c integer,
    UNIQUE (a, c)
);

CREATE TABLE t1 (
  a integer PRIMARY KEY,
  b integer,
  c integer,
  FOREIGN KEY (b, c) REFERENCES other_table (c1, c2)
);
 */
class PG_Constraint {
 public:
  /**
   * Constructor for FK constraint
   * requires all attribbutes including those for FK to be set
   */
  PG_Constraint(constraint_oid_t con_oid, std::string con_name, namespace_oid_t con_namespace_id,
                ConstraintType con_type, bool con_deferrable, bool con_deferred, bool con_validated,
                table_oid_t con_relid, index_oid_t con_index_id, std::string con_frelid_varchar = "",
                std::string con_unique_col_varchar = "", constraint_oid_t check_id = INVALID_CONSTRAINT_OID,
                constraint_oid_t exclusion_id = INVALID_CONSTRAINT_OID) {
    conoid_ = con_oid;
    conname_ = con_name;
    connamespaceid_ = con_namespace_id;
    contype_ = con_type;
    condeferrable_ = con_deferrable;
    condeferred_ = con_deferred;
    convalidated_ = con_validated;
    conrelid_ = con_relid;
    conindid_ = con_index_id;
    InitializeMetaData(con_frelid_varchar, con_unique_col_varchar, check_id, exclusion_id);
  }

 private:
  friend class DatabaseCatalog;
  constraint_oid_t conoid_;  // oid of the constraint
  std::string conname_;
  namespace_oid_t connamespaceid_; /* OID of namespace containing constraint */
  ConstraintType contype_;         // type of the constraint

  bool condeferrable_;    /* deferrable constraint? */
  bool condeferred_;      /* deferred by default? */
  bool convalidated_;     /* Has the constraint been validated? Currently, can only be false for foreign keys */
  table_oid_t conrelid_;  // table this constraint applies to
  index_oid_t conindid_;  /* index supporting this constraint */

  PGConstraintMetadata metadata_;  // pther metadata depending on the constraint type

  friend class Catalog;
  friend class postgres::Builder;
  friend class terrier::TpccPlanTest;

  void InitializeMetaData(std::string con_frelid_varchar, std::string con_unique_col_varchar, constraint_oid_t check_id,
                          constraint_oid_t exclusion_id) {
    switch (ConstraintType type = this->contype_)
    {
    case (ConstraintType::CHECK):
      /* code */
      break;
    case (ConstraintType::FOREIGN_KEY):
      /* code */
      break;
    case (ConstraintType::PRIMARY_KEY):
      /* code */
      break;
    case (ConstraintType::UNIQUE):
      /* code */
      break;
    case (ConstraintType::TRIGGER):
      /* code */
      break;
    case (ConstraintType::EXCLUSION):
      /* code */
      break;
    case (ConstraintType::NOTNULL):
      /* code */
      break;
    default:
      break;
    }
  }
};
}  // namespace terrier::catalog::postgres