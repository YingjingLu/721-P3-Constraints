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
   * default constructor for constraints other than FK constraint
   * set fk related parameters to zero or false to occupy space
   */
  PG_Constraint(constraint_oid_t con_oid, std::string con_name, namespace_oid_t con_namespace_id,
                ConstraintType con_type, bool con_deferrable, bool con_deferred, bool con_validated,
                col_oid_t con_relid, index_oid_t con_index_id, size_t con_bin, std::string con_src) {
    conoid_ = con_oid;
    conname_ = con_name;
    connamespaceid_ = con_namespace_id;
    contype_ = con_type;
    condeferrable_ = con_deferrable;
    condeferred_ = con_deferred;
    convalidated_ = con_validated;
    conrelid_ = con_relid;
    conindid_ = con_index_id;
    conbin_ = con_bin;
    consrc_ = con_src;
    // void FK attr
    confrelid_ = col_oid_t(0);
    fk_update_cascade_ = false;
    fk_delete_cascade_ = false;
  }

  /**
   * Constructor for FK constraint
   * requires all attribbutes including those for FK to be set
   */
  PG_Constraint(constraint_oid_t con_oid, std::string con_name, namespace_oid_t con_namespace_id,
                ConstraintType con_type, bool con_deferrable, bool con_deferred, bool con_validated,
                col_oid_t con_relid, index_oid_t con_index_id, size_t con_bin, std::string con_src,
                col_oid_t con_fk_rel_id, bool fk_update_cascade, bool fk_delete_cascade) {
    conoid_ = con_oid;
    conname_ = con_name;
    connamespaceid_ = con_namespace_id;
    contype_ = con_type;
    condeferrable_ = con_deferrable;
    condeferred_ = con_deferred;
    convalidated_ = con_validated;
    conrelid_ = con_relid;
    conindid_ = con_index_id;
    conbin_ = con_bin;
    consrc_ = con_src;
    // void FK attr
    confrelid_ = con_fk_rel_id;
    fk_update_cascade_ = fk_update_cascade;
    fk_delete_cascade_ = fk_delete_cascade;
  }

 private:
  friend class DatabaseCatalog;
  constraint_oid_t conoid_;  // oid of the constraint
  std::string conname_;
  namespace_oid_t connamespaceid_; /* OID of namespace containing constraint */
  ConstraintType contype_;         // type of the constraint

  bool condeferrable_; /* deferrable constraint? */
  bool condeferred_;   /* deferred by default? */
  bool convalidated_;  /* constraint has been validated? */
 
  col_oid_t conrelid_; /* constraint relation id */
  index_oid_t conindid_; /* index supporting this constraint */

  /************************ Foreign Key specific ******************************/
  // only applies to foreign keys, set to zero if other types of constraints
  col_oid_t confrelid_;     /* relation id for the foreign key, set to zero if non-FK constraint */
  bool fk_update_cascade_;  // true if cascade on update
  bool fk_delete_cascade_;  // true if cascade on deletion

  std::vector<col_oid_t> con_fk_src_; // the column indcies in the current table for foreign key
  std::vector<col_oid_t> con_fk_ref_; // the column indicies in the parent table that are reference for the foreign key

  /************************ unique key specific *********************************/
  std::vector<col_oid_t> con_unique_oid_; // columns where the uniqueness apply

  /************************ Other undocumented attr ****************************/
  size_t conbin_;
  std::string consrc_;

  friend class Catalog;
  friend class postgres::Builder;
  friend class terrier::TpccPlanTest;
};
}  // namespace terrier::catalog::postgres