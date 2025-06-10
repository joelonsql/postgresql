const std = @import("std");
const ext = @import("postgres_ext.zig");

// Re-export Oid from postgres_ext.h
pub const Oid = ext.Oid;

// These symbols may be used in compile-time #ifdef tests for the availability
// of v14-and-newer libpq features.
// Features added in PostgreSQL v14:
pub const LIBPQ_HAS_PIPELINING = true;
pub const LIBPQ_HAS_TRACE_FLAGS = true;

// Features added in PostgreSQL v15:
pub const LIBPQ_HAS_SSL_LIBRARY_DETECTION = true;

// Features added in PostgreSQL v17:
pub const LIBPQ_HAS_ASYNC_CANCEL = true;
pub const LIBPQ_HAS_CHANGE_PASSWORD = true;
pub const LIBPQ_HAS_CHUNK_MODE = true;
pub const LIBPQ_HAS_CLOSE_PREPARED = true;
pub const LIBPQ_HAS_SEND_PIPELINE_SYNC = true;
pub const LIBPQ_HAS_SOCKET_POLL = true;

// Features added in PostgreSQL v18:
pub const LIBPQ_HAS_FULL_PROTOCOL_VERSION = true;
pub const LIBPQ_HAS_PROMPT_OAUTH_DEVICE = true;

// Option flags for PQcopyResult
pub const PG_COPYRES_ATTRS: u32 = 0x01;
pub const PG_COPYRES_TUPLES: u32 = 0x02; // Implies PG_COPYRES_ATTRS
pub const PG_COPYRES_EVENTS: u32 = 0x04;
pub const PG_COPYRES_NOTICEHOOKS: u32 = 0x08;

// Application-visible enum types
pub const ConnStatusType = enum(c_int) {
    CONNECTION_OK,
    CONNECTION_BAD,
    // Non-blocking mode only below here
    CONNECTION_STARTED, // Waiting for connection to be made.
    CONNECTION_MADE, // Connection OK; waiting to send.
    CONNECTION_AWAITING_RESPONSE, // Waiting for a response from the postmaster.
    CONNECTION_AUTH_OK, // Received authentication; waiting for backend startup.
    CONNECTION_SETENV, // This state is no longer used.
    CONNECTION_SSL_STARTUP, // Performing SSL handshake.
    CONNECTION_NEEDED, // Internal state: connect() needed.
    CONNECTION_CHECK_WRITABLE, // Checking if session is read-write.
    CONNECTION_CONSUME, // Consuming any extra messages.
    CONNECTION_GSS_STARTUP, // Negotiating GSSAPI.
    CONNECTION_CHECK_TARGET, // Internal state: checking target server properties.
    CONNECTION_CHECK_STANDBY, // Checking if server is in standby mode.
    CONNECTION_ALLOCATED, // Waiting for connection attempt to be started.
    CONNECTION_AUTHENTICATING, // Authentication is in progress with some external system.
};

pub const PostgresPollingStatusType = enum(c_int) {
    PGRES_POLLING_FAILED = 0,
    PGRES_POLLING_READING, // These two indicate that one may use select before polling again.
    PGRES_POLLING_WRITING,
    PGRES_POLLING_OK,
    PGRES_POLLING_ACTIVE, // unused; keep for backwards compatibility
};

pub const ExecStatusType = enum(c_int) {
    PGRES_EMPTY_QUERY = 0, // empty query string was executed
    PGRES_COMMAND_OK, // a query command that doesn't return anything was executed properly by the backend
    PGRES_TUPLES_OK, // a query command that returns tuples was executed properly by the backend, PGresult contains the result tuples
    PGRES_COPY_OUT, // Copy Out data transfer in progress
    PGRES_COPY_IN, // Copy In data transfer in progress
    PGRES_BAD_RESPONSE, // an unexpected response was recv'd from the backend
    PGRES_NONFATAL_ERROR, // notice or warning message
    PGRES_FATAL_ERROR, // query failed
    PGRES_COPY_BOTH, // Copy In/Out data transfer in progress
    PGRES_SINGLE_TUPLE, // single tuple from larger resultset
    PGRES_PIPELINE_SYNC, // pipeline synchronization point
    PGRES_PIPELINE_ABORTED, // Command didn't run because of an abort earlier in a pipeline
    PGRES_TUPLES_CHUNK, // chunk of tuples from larger resultset
};

pub const PGTransactionStatusType = enum(c_int) {
    PQTRANS_IDLE, // connection idle
    PQTRANS_ACTIVE, // command in progress
    PQTRANS_INTRANS, // idle, within transaction block
    PQTRANS_INERROR, // idle, within failed transaction
    PQTRANS_UNKNOWN, // cannot determine status
};

pub const PGVerbosity = enum(c_int) {
    PQERRORS_TERSE, // single-line error messages
    PQERRORS_DEFAULT, // recommended style
    PQERRORS_VERBOSE, // all the facts, ma'am
    PQERRORS_SQLSTATE, // only error severity and SQLSTATE code
};

pub const PGContextVisibility = enum(c_int) {
    PQSHOW_CONTEXT_NEVER, // never show CONTEXT field
    PQSHOW_CONTEXT_ERRORS, // show CONTEXT for errors only (default)
    PQSHOW_CONTEXT_ALWAYS, // always show CONTEXT field
};

// PGPing - The ordering of this enum should not be altered because the
// values are exposed externally via pg_isready.
pub const PGPing = enum(c_int) {
    PQPING_OK, // server is accepting connections
    PQPING_REJECT, // server is alive but rejecting connections
    PQPING_NO_RESPONSE, // could not establish connection
    PQPING_NO_ATTEMPT, // connection not attempted (bad params)
};

// PGpipelineStatus - Current status of pipeline mode
pub const PGpipelineStatus = enum(c_int) {
    PQ_PIPELINE_OFF,
    PQ_PIPELINE_ON,
    PQ_PIPELINE_ABORTED,
};

pub const PGauthData = enum(c_int) {
    PQAUTHDATA_PROMPT_OAUTH_DEVICE, // user must visit a device-authorization URL
    PQAUTHDATA_OAUTH_BEARER_TOKEN, // server requests an OAuth Bearer token
};

// PGconn encapsulates a connection to the backend.
// The contents of this struct are not supposed to be known to applications.
pub const PGconn = opaque {};

// PGcancelConn encapsulates a cancel connection to the backend.
// The contents of this struct are not supposed to be known to applications.
pub const PGcancelConn = opaque {};

// PGresult encapsulates the result of a query (or more precisely, of a single
// SQL command --- a query string given to PQsendQuery can contain multiple
// commands and thus return multiple PGresult objects).
// The contents of this struct are not supposed to be known to applications.
pub const PGresult = opaque {};

// PGcancel encapsulates the information needed to cancel a running
// query on an existing connection.
// The contents of this struct are not supposed to be known to applications.
pub const PGcancel = opaque {};

// PGnotify represents the occurrence of a NOTIFY message.
// Ideally this would be an opaque typedef, but it's so simple that it's
// unlikely to change.
// NOTE: in Postgres 6.4 and later, the be_pid is the notifying backend's,
// whereas in earlier versions it was always your own backend's PID.
pub const PGnotify = extern struct {
    relname: [*:0]u8, // notification condition name
    be_pid: c_int, // process ID of notifying server process
    extra: [*:0]u8, // notification parameter
    // Fields below here are private to libpq; apps should not use 'em
    next: ?*PGnotify, // list link
};

// deprecated name for int64_t
pub const pg_int64 = i64;

// pg_usec_time_t is like time_t, but with microsecond resolution
pub const pg_usec_time_t = i64;

// Function types for notice-handling callbacks
pub const PQnoticeReceiver = *const fn (arg: ?*anyopaque, res: *const PGresult) callconv(.C) void;
pub const PQnoticeProcessor = *const fn (arg: ?*anyopaque, message: [*:0]const u8) callconv(.C) void;

// Print options for PQprint()
pub const pqbool = u8;

pub const PQprintOpt = extern struct {
    header: pqbool, // print output field headings and row count
    align: pqbool, // fill align the fields
    standard: pqbool, // old brain dead format
    html3: pqbool, // output html tables
    expanded: pqbool, // expand tables
    pager: pqbool, // use pager for output if needed
    fieldSep: [*:0]u8, // field separator
    tableOpt: [*:0]u8, // insert to HTML <table ...>
    caption: [*:0]u8, // HTML <caption>
    fieldName: [*][*:0]u8, // null terminated array of replacement field names
};

// Structure for the conninfo parameter definitions returned by PQconndefaults
// or PQconninfoParse.
//
// All fields except "val" point at static strings which must not be altered.
// "val" is either NULL or a malloc'd current-value string. PQconninfoFree()
// will release both the val strings and the PQconninfoOption array itself.
pub const PQconninfoOption = extern struct {
    keyword: [*:0]const u8, // The keyword of the option
    envvar: [*:0]const u8, // Fallback environment variable name
    compiled: [*:0]const u8, // Fallback compiled in default value
    val: ?[*:0]u8, // Option's current value, or NULL
    label: [*:0]const u8, // Label for field in connect dialog
    dispchar: [*:0]const u8, // Indicates how to display this field in a connect dialog
    dispsize: c_int, // Field size in characters for dialog
};

// PQArgBlock -- structure for PQfn() arguments
pub const PQArgBlock = extern struct {
    len: c_int,
    isint: c_int,
    u: extern union {
        ptr: *c_int, // can't use void (dec compiler barfs)
        integer: c_int,
    },
};

// PGresAttDesc -- Data about a single attribute (column) of a query result
pub const PGresAttDesc = extern struct {
    name: [*:0]u8, // column name
    tableid: Oid, // source table, if known
    columnid: c_int, // source column, if known
    format: c_int, // format code for value (text/binary)
    typid: Oid, // type id
    typlen: c_int, // type size
    atttypmod: c_int, // type-specific modifier info
};

// Import implementation
const impl = @import("fe-impl.zig");

// === in fe-connect.c ===

// make a new client connection to the backend
// Asynchronous (non-blocking)
pub const connectStart = impl.connectStart;
pub const connectStartParams = impl.connectStartParams;
pub const connectPoll = impl.connectPoll;

// Synchronous (blocking)
pub const connectdb = impl.connectdb;
pub const connectdbParams = impl.connectdbParams;
pub const setdbLogin = impl.setdbLogin;

pub fn setdb(pghost: ?[*:0]const u8, pgport: ?[*:0]const u8, pgoptions: ?[*:0]const u8, pgtty: ?[*:0]const u8, dbName: ?[*:0]const u8) ?*PGconn {
    return setdbLogin(pghost, pgport, pgoptions, pgtty, dbName, null, null);
}

// close the current connection and free the PGconn data structure
pub const finish = impl.finish;

// get info about connection options known to PQconnectdb
pub const conndefaults = impl.conndefaults;

// parse connection options in same way as PQconnectdb
pub const conninfoParse = impl.conninfoParse;

// return the connection options used by a live connection
pub const conninfo = impl.conninfo;

// free the data structure returned by PQconndefaults() or PQconninfoParse()
pub const conninfoFree = impl.conninfoFree;

// close the current connection and reestablish a new one with the same parameters
// Asynchronous (non-blocking)
pub const resetStart = impl.resetStart;
pub const resetPoll = impl.resetPoll;

// Synchronous (blocking)
pub const reset = impl.reset;

// Create a PGcancelConn that's used to cancel a query on the given PGconn
pub const cancelCreate = impl.cancelCreate;

// issue a cancel request in a non-blocking manner
pub const cancelStart = impl.cancelStart;

// issue a blocking cancel request
pub const cancelBlocking = impl.cancelBlocking;

// poll a non-blocking cancel request
pub const cancelPoll = impl.cancelPoll;
pub const cancelStatus = impl.cancelStatus;
pub const cancelSocket = impl.cancelSocket;
pub const cancelErrorMessage = impl.cancelErrorMessage;
pub const cancelReset = impl.cancelReset;
pub const cancelFinish = impl.cancelFinish;

// request a cancel structure
pub const getCancel = impl.getCancel;

// free a cancel structure
pub const freeCancel = impl.freeCancel;

// deprecated version of PQcancelBlocking, but one which is signal-safe
pub const cancel = impl.cancel;

// deprecated version of PQcancel; not thread-safe
pub const requestCancel = impl.requestCancel;

// Accessor functions for PGconn objects
pub const db = impl.db;
pub const service = impl.service;
pub const user = impl.user;
pub const pass = impl.pass;
pub const host = impl.host;
pub const hostaddr = impl.hostaddr;
pub const port = impl.port;
pub const tty = impl.tty;
pub const options = impl.options;
pub const status = impl.status;
pub const transactionStatus = impl.transactionStatus;
pub const parameterStatus = impl.parameterStatus;
pub const protocolVersion = impl.protocolVersion;
pub const fullProtocolVersion = impl.fullProtocolVersion;
pub const serverVersion = impl.serverVersion;
pub const errorMessage = impl.errorMessage;
pub const socket = impl.socket;
pub const backendPID = impl.backendPID;
pub const pipelineStatus = impl.pipelineStatus;
pub const connectionNeedsPassword = impl.connectionNeedsPassword;
pub const connectionUsedPassword = impl.connectionUsedPassword;
pub const connectionUsedGSSAPI = impl.connectionUsedGSSAPI;
pub const clientEncoding = impl.clientEncoding;
pub const setClientEncoding = impl.setClientEncoding;

// SSL information functions
pub const sslInUse = impl.sslInUse;
pub const sslStruct = impl.sslStruct;
pub const sslAttribute = impl.sslAttribute;
pub const sslAttributeNames = impl.sslAttributeNames;

// Get the OpenSSL structure associated with a connection. Returns NULL for
// unencrypted connections or if any other TLS library is in use.
pub const getssl = impl.getssl;

// Tell libpq whether it needs to initialize OpenSSL
pub const initSSL = impl.initSSL;

// More detailed way to tell libpq whether it needs to initialize OpenSSL
pub const initOpenSSL = impl.initOpenSSL;

// Return true if GSSAPI encryption is in use
pub const gssEncInUse = impl.gssEncInUse;

// Returns GSSAPI context if GSSAPI is in use
pub const getgssctx = impl.getgssctx;

// Set verbosity for PQerrorMessage and PQresultErrorMessage
pub const setErrorVerbosity = impl.setErrorVerbosity;

// Set CONTEXT visibility for PQerrorMessage and PQresultErrorMessage
pub const setErrorContextVisibility = impl.setErrorContextVisibility;

// Override default notice handling routines
pub const setNoticeReceiver = impl.setNoticeReceiver;
pub const setNoticeProcessor = impl.setNoticeProcessor;

// Used to set callback that prevents concurrent access to
// non-thread safe functions that libpq needs.
// The default implementation uses a libpq internal mutex.
// Only required for multithreaded apps that use kerberos
// both within their app and for postgresql connections.
pub const pgthreadlock_t = *const fn (acquire: c_int) callconv(.C) void;

pub const registerThreadLock = impl.registerThreadLock;

// === in fe-trace.c ===
pub const trace = impl.trace;
pub const untrace = impl.untrace;

// flags controlling trace output:
pub const PQTRACE_SUPPRESS_TIMESTAMPS: c_int = 1 << 0; // omit timestamps from each line
pub const PQTRACE_REGRESS_MODE: c_int = 1 << 1; // redact portions of some messages, for testing frameworks
pub const setTraceFlags = impl.setTraceFlags;

// === in fe-exec.c ===

// Simple synchronous query
pub const exec = impl.exec;
pub const execParams = impl.execParams;
pub const prepare = impl.prepare;
pub const execPrepared = impl.execPrepared;

// Interface for multiple-result or asynchronous queries
pub const PQ_QUERY_PARAM_MAX_LIMIT: c_int = 65535;

pub const sendQuery = impl.sendQuery;
pub const sendQueryParams = impl.sendQueryParams;
pub const sendPrepare = impl.sendPrepare;
pub const sendQueryPrepared = impl.sendQueryPrepared;
pub const setSingleRowMode = impl.setSingleRowMode;
pub const setChunkedRowsMode = impl.setChunkedRowsMode;
pub const getResult = impl.getResult;

// Routines for managing an asynchronous query
pub const isBusy = impl.isBusy;
pub const consumeInput = impl.consumeInput;

// Routines for pipeline mode management
pub const enterPipelineMode = impl.enterPipelineMode;
pub const exitPipelineMode = impl.exitPipelineMode;
pub const pipelineSync = impl.pipelineSync;
pub const sendFlushRequest = impl.sendFlushRequest;
pub const sendPipelineSync = impl.sendPipelineSync;

// LISTEN/NOTIFY support
pub const notifies = impl.notifies;

// Routines for copy in/out
pub const putCopyData = impl.putCopyData;
pub const putCopyEnd = impl.putCopyEnd;
pub const getCopyData = impl.getCopyData;

// Deprecated routines for copy in/out
pub const getline = impl.getline;
pub const putline = impl.putline;
pub const getlineAsync = impl.getlineAsync;
pub const putnbytes = impl.putnbytes;
pub const endcopy = impl.endcopy;

// Set blocking/nonblocking connection to the backend
pub const setnonblocking = impl.setnonblocking;
pub const isnonblocking = impl.isnonblocking;
pub const isthreadsafe = impl.isthreadsafe;
pub const ping = impl.ping;
pub const pingParams = impl.pingParams;

// Force the write buffer to be written (or at least try)
pub const flush = impl.flush;

// "Fast path" interface --- not really recommended for application use
pub const fn_ = impl.fn_;

// Accessor functions for PGresult objects
pub const resultStatus = impl.resultStatus;
pub const resStatus = impl.resStatus;
pub const resultErrorMessage = impl.resultErrorMessage;
pub const resultVerboseErrorMessage = impl.resultVerboseErrorMessage;
pub const resultErrorField = impl.resultErrorField;
pub const ntuples = impl.ntuples;
pub const nfields = impl.nfields;
pub const binaryTuples = impl.binaryTuples;
pub const fname = impl.fname;
pub const fnumber = impl.fnumber;
pub const ftable = impl.ftable;
pub const ftablecol = impl.ftablecol;
pub const fformat = impl.fformat;
pub const ftype = impl.ftype;
pub const fsize = impl.fsize;
pub const fmod = impl.fmod;
pub const cmdStatus = impl.cmdStatus;
pub const oidStatus = impl.oidStatus; // old and ugly
pub const oidValue = impl.oidValue; // new and improved
pub const cmdTuples = impl.cmdTuples;
pub const getvalue = impl.getvalue;
pub const getlength = impl.getlength;
pub const getisnull = impl.getisnull;
pub const nparams = impl.nparams;
pub const paramtype = impl.paramtype;

// Describe prepared statements and portals
pub const describePrepared = impl.describePrepared;
pub const describePortal = impl.describePortal;
pub const sendDescribePrepared = impl.sendDescribePrepared;
pub const sendDescribePortal = impl.sendDescribePortal;

// Close prepared statements and portals
pub const closePrepared = impl.closePrepared;
pub const closePortal = impl.closePortal;
pub const sendClosePrepared = impl.sendClosePrepared;
pub const sendClosePortal = impl.sendClosePortal;

// Delete a PGresult
pub const clear = impl.clear;

// For freeing other alloc'd results, such as PGnotify structs
pub const freemem = impl.freemem;

// Exists for backward compatibility. bjm 2003-03-24
pub fn freeNotify(ptr: *anyopaque) void {
    freemem(ptr);
}

// Error when no password was given.
// Note: depending on this is deprecated; use PQconnectionNeedsPassword().
pub const noPasswordSupplied = "fe_sendauth: no password supplied\n";

// Create and manipulate PGresults
pub const makeEmptyPGresult = impl.makeEmptyPGresult;
pub const copyResult = impl.copyResult;
pub const setResultAttrs = impl.setResultAttrs;
pub const resultAlloc = impl.resultAlloc;
pub const resultMemorySize = impl.resultMemorySize;
pub const setvalue = impl.setvalue;

// Quoting strings before inclusion in queries.
pub const escapeStringConn = impl.escapeStringConn;
pub const escapeLiteral = impl.escapeLiteral;
pub const escapeIdentifier = impl.escapeIdentifier;
pub const escapeByteaConn = impl.escapeByteaConn;
pub const unescapeBytea = impl.unescapeBytea;

// These forms are deprecated!
pub const escapeString = impl.escapeString;
pub const escapeBytea = impl.escapeBytea;

// === in fe-print.c ===

pub const print = impl.print;

// really old printing routines
pub const displayTuples = impl.displayTuples;

pub const printTuples = impl.printTuples;

// === in fe-lobj.c ===

// Large-object access routines
pub const lo_open = impl.lo_open;
pub const lo_close = impl.lo_close;
pub const lo_read = impl.lo_read;
pub const lo_write = impl.lo_write;
pub const lo_lseek = impl.lo_lseek;
pub const lo_lseek64 = impl.lo_lseek64;
pub const lo_creat = impl.lo_creat;
pub const lo_create = impl.lo_create;
pub const lo_tell = impl.lo_tell;
pub const lo_tell64 = impl.lo_tell64;
pub const lo_truncate = impl.lo_truncate;
pub const lo_truncate64 = impl.lo_truncate64;
pub const lo_unlink = impl.lo_unlink;
pub const lo_import = impl.lo_import;
pub const lo_import_with_oid = impl.lo_import_with_oid;
pub const lo_export = impl.lo_export;

// === in fe-misc.c ===

// Get the version of the libpq library in use
pub const libVersion = impl.libVersion;

// Poll a socket for reading and/or writing with an optional timeout
pub const socketPoll = impl.socketPoll;

// Get current time in the form PQsocketPoll wants
pub const getCurrentTimeUSec = impl.getCurrentTimeUSec;

// Determine length of multibyte encoded char at *s
pub const mblen = impl.mblen;

// Same, but not more than the distance to the end of string s
pub const mblenBounded = impl.mblenBounded;

// Determine display length of multibyte encoded char at *s
pub const dsplen = impl.dsplen;

// Get encoding id from environment variable PGCLIENTENCODING
pub const env2encoding = impl.env2encoding;

// === in fe-auth.c ===

pub const PGpromptOAuthDevice = extern struct {
    verification_uri: [*:0]const u8, // verification URI to visit
    user_code: [*:0]const u8, // user code to enter
    verification_uri_complete: ?[*:0]const u8, // optional combination of URI and code, or NULL
    expires_in: c_int, // seconds until user code expires
};

// for PGoauthBearerRequest.async()
pub const SOCKTYPE = if (@import("builtin").os.tag == .windows) usize else c_int;

pub const PGoauthBearerRequest = extern struct {
    // Hook inputs (constant across all calls)
    openid_configuration: [*:0]const u8, // OIDC discovery URI
    scope: ?[*:0]const u8, // required scope(s), or NULL

    // Hook outputs

    // Callback implementing a custom asynchronous OAuth flow.
    async: ?*const fn (conn: *PGconn, request: *PGoauthBearerRequest, altsock: *SOCKTYPE) callconv(.C) PostgresPollingStatusType,

    // Callback to clean up custom allocations. A hook implementation may use
    // this to free request.token and any resources in request.user.
    cleanup: ?*const fn (conn: *PGconn, request: *PGoauthBearerRequest) callconv(.C) void,

    // The hook should set this to the Bearer token contents for the
    // connection, once the flow is completed. The token contents must remain
    // available to libpq until the hook's cleanup callback is called.
    token: ?[*:0]u8,

    // Hook-defined data. libpq will not modify this pointer across calls to
    // the async callback, so it can be used to keep track of
    // application-specific state. Resources allocated here should be freed by
    // the cleanup callback.
    user: ?*anyopaque,
};

pub const encryptPassword = impl.encryptPassword;
pub const encryptPasswordConn = impl.encryptPasswordConn;
pub const changePassword = impl.changePassword;

pub const PQauthDataHook_type = *const fn (type: PGauthData, conn: *PGconn, data: ?*anyopaque) callconv(.C) c_int;
pub const setAuthDataHook = impl.setAuthDataHook;
pub const getAuthDataHook = impl.getAuthDataHook;
pub const defaultAuthDataHook = impl.defaultAuthDataHook;

// === in encnames.c ===

pub const char_to_encoding = impl.char_to_encoding;
pub const encoding_to_char = impl.encoding_to_char;
pub const valid_server_encoding_id = impl.valid_server_encoding_id;

// === in fe-secure-openssl.c ===

// Support for overriding sslpassword handling with a callback
pub const PQsslKeyPassHook_OpenSSL_type = *const fn (buf: [*]u8, size: c_int, conn: *PGconn) callconv(.C) c_int;
pub const getSSLKeyPassHook_OpenSSL = impl.getSSLKeyPassHook_OpenSSL;
pub const setSSLKeyPassHook_OpenSSL = impl.setSSLKeyPassHook_OpenSSL;
pub const defaultSSLKeyPassHook_OpenSSL = impl.defaultSSLKeyPassHook_OpenSSL;