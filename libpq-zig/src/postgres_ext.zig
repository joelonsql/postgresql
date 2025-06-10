const std = @import("std");

// Object ID is a fundamental type in Postgres.
pub const Oid = u32;

pub const InvalidOid: Oid = 0;

pub const OID_MAX = std.math.maxInt(u32);

pub fn atooid(x: []const u8) Oid {
    return std.fmt.parseInt(Oid, x, 10) catch InvalidOid;
}

// Identifiers of error message fields. Kept here to keep common
// between frontend and backend, and also to export them to libpq
// applications.
pub const PG_DIAG_SEVERITY: u8 = 'S';
pub const PG_DIAG_SEVERITY_NONLOCALIZED: u8 = 'V';
pub const PG_DIAG_SQLSTATE: u8 = 'C';
pub const PG_DIAG_MESSAGE_PRIMARY: u8 = 'M';
pub const PG_DIAG_MESSAGE_DETAIL: u8 = 'D';
pub const PG_DIAG_MESSAGE_HINT: u8 = 'H';
pub const PG_DIAG_STATEMENT_POSITION: u8 = 'P';
pub const PG_DIAG_INTERNAL_POSITION: u8 = 'p';
pub const PG_DIAG_INTERNAL_QUERY: u8 = 'q';
pub const PG_DIAG_CONTEXT: u8 = 'W';
pub const PG_DIAG_SCHEMA_NAME: u8 = 's';
pub const PG_DIAG_TABLE_NAME: u8 = 't';
pub const PG_DIAG_COLUMN_NAME: u8 = 'c';
pub const PG_DIAG_DATATYPE_NAME: u8 = 'd';
pub const PG_DIAG_CONSTRAINT_NAME: u8 = 'n';
pub const PG_DIAG_SOURCE_FILE: u8 = 'F';
pub const PG_DIAG_SOURCE_LINE: u8 = 'L';
pub const PG_DIAG_SOURCE_FUNCTION: u8 = 'R';