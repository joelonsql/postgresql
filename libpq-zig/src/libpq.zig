const std = @import("std");

// Re-export all public APIs
pub const fe = @import("fe.zig");
pub const events = @import("events.zig");
pub const ext = @import("postgres_ext.zig");

// Re-export commonly used types at the top level
pub const Oid = ext.Oid;
pub const InvalidOid = ext.InvalidOid;
pub const OID_MAX = ext.OID_MAX;

// Connection management
pub const PGconn = fe.PGconn;
pub const PGcancelConn = fe.PGcancelConn;
pub const PGcancel = fe.PGcancel;
pub const PGresult = fe.PGresult;
pub const PGnotify = fe.PGnotify;

// Enums
pub const ConnStatusType = fe.ConnStatusType;
pub const PostgresPollingStatusType = fe.PostgresPollingStatusType;
pub const ExecStatusType = fe.ExecStatusType;
pub const PGTransactionStatusType = fe.PGTransactionStatusType;
pub const PGVerbosity = fe.PGVerbosity;
pub const PGContextVisibility = fe.PGContextVisibility;
pub const PGPing = fe.PGPing;
pub const PGpipelineStatus = fe.PGpipelineStatus;
pub const PGauthData = fe.PGauthData;

// Structs
pub const PQconninfoOption = fe.PQconninfoOption;
pub const PQArgBlock = fe.PQArgBlock;
pub const PGresAttDesc = fe.PGresAttDesc;
pub const PQprintOpt = fe.PQprintOpt;
pub const PGpromptOAuthDevice = fe.PGpromptOAuthDevice;
pub const PGoauthBearerRequest = fe.PGoauthBearerRequest;

// Function types
pub const PQnoticeReceiver = fe.PQnoticeReceiver;
pub const PQnoticeProcessor = fe.PQnoticeProcessor;
pub const pgthreadlock_t = fe.pgthreadlock_t;
pub const PQauthDataHook_type = fe.PQauthDataHook_type;

// Connection functions
pub const connectStart = fe.connectStart;
pub const connectStartParams = fe.connectStartParams;
pub const connectPoll = fe.connectPoll;
pub const connectdb = fe.connectdb;
pub const connectdbParams = fe.connectdbParams;
pub const setdbLogin = fe.setdbLogin;
pub const finish = fe.finish;
pub const conndefaults = fe.conndefaults;
pub const conninfoParse = fe.conninfoParse;
pub const conninfo = fe.conninfo;
pub const conninfoFree = fe.conninfoFree;
pub const resetStart = fe.resetStart;
pub const resetPoll = fe.resetPoll;
pub const reset = fe.reset;

// Cancel functions
pub const cancelCreate = fe.cancelCreate;
pub const cancelStart = fe.cancelStart;
pub const cancelBlocking = fe.cancelBlocking;
pub const cancelPoll = fe.cancelPoll;
pub const cancelStatus = fe.cancelStatus;
pub const cancelSocket = fe.cancelSocket;
pub const cancelErrorMessage = fe.cancelErrorMessage;
pub const cancelReset = fe.cancelReset;
pub const cancelFinish = fe.cancelFinish;
pub const getCancel = fe.getCancel;
pub const freeCancel = fe.freeCancel;
pub const cancel = fe.cancel;
pub const requestCancel = fe.requestCancel;

// Connection accessor functions
pub const db = fe.db;
pub const service = fe.service;
pub const user = fe.user;
pub const pass = fe.pass;
pub const host = fe.host;
pub const hostaddr = fe.hostaddr;
pub const port = fe.port;
pub const tty = fe.tty;
pub const options = fe.options;
pub const status = fe.status;
pub const transactionStatus = fe.transactionStatus;
pub const parameterStatus = fe.parameterStatus;
pub const protocolVersion = fe.protocolVersion;
pub const fullProtocolVersion = fe.fullProtocolVersion;
pub const serverVersion = fe.serverVersion;
pub const errorMessage = fe.errorMessage;
pub const socket = fe.socket;
pub const backendPID = fe.backendPID;
pub const pipelineStatus = fe.pipelineStatus;
pub const connectionNeedsPassword = fe.connectionNeedsPassword;
pub const connectionUsedPassword = fe.connectionUsedPassword;
pub const connectionUsedGSSAPI = fe.connectionUsedGSSAPI;
pub const clientEncoding = fe.clientEncoding;
pub const setClientEncoding = fe.setClientEncoding;

// SSL functions
pub const sslInUse = fe.sslInUse;
pub const sslStruct = fe.sslStruct;
pub const sslAttribute = fe.sslAttribute;
pub const sslAttributeNames = fe.sslAttributeNames;
pub const getssl = fe.getssl;
pub const initSSL = fe.initSSL;
pub const initOpenSSL = fe.initOpenSSL;

// GSSAPI functions
pub const gssEncInUse = fe.gssEncInUse;
pub const getgssctx = fe.getgssctx;

// Error handling
pub const setErrorVerbosity = fe.setErrorVerbosity;
pub const setErrorContextVisibility = fe.setErrorContextVisibility;
pub const setNoticeReceiver = fe.setNoticeReceiver;
pub const setNoticeProcessor = fe.setNoticeProcessor;
pub const registerThreadLock = fe.registerThreadLock;

// Tracing
pub const trace = fe.trace;
pub const untrace = fe.untrace;
pub const setTraceFlags = fe.setTraceFlags;

// Query execution
pub const exec = fe.exec;
pub const execParams = fe.execParams;
pub const prepare = fe.prepare;
pub const execPrepared = fe.execPrepared;
pub const sendQuery = fe.sendQuery;
pub const sendQueryParams = fe.sendQueryParams;
pub const sendPrepare = fe.sendPrepare;
pub const sendQueryPrepared = fe.sendQueryPrepared;
pub const setSingleRowMode = fe.setSingleRowMode;
pub const setChunkedRowsMode = fe.setChunkedRowsMode;
pub const getResult = fe.getResult;
pub const isBusy = fe.isBusy;
pub const consumeInput = fe.consumeInput;

// Pipeline mode
pub const enterPipelineMode = fe.enterPipelineMode;
pub const exitPipelineMode = fe.exitPipelineMode;
pub const pipelineSync = fe.pipelineSync;
pub const sendFlushRequest = fe.sendFlushRequest;
pub const sendPipelineSync = fe.sendPipelineSync;

// LISTEN/NOTIFY
pub const notifies = fe.notifies;

// COPY support
pub const putCopyData = fe.putCopyData;
pub const putCopyEnd = fe.putCopyEnd;
pub const getCopyData = fe.getCopyData;

// Deprecated COPY functions
pub const getline = fe.getline;
pub const putline = fe.putline;
pub const getlineAsync = fe.getlineAsync;
pub const putnbytes = fe.putnbytes;
pub const endcopy = fe.endcopy;

// Async support
pub const setnonblocking = fe.setnonblocking;
pub const isnonblocking = fe.isnonblocking;
pub const isthreadsafe = fe.isthreadsafe;
pub const ping = fe.ping;
pub const pingParams = fe.pingParams;
pub const flush = fe.flush;

// Fast path interface
pub const fn_ = fe.fn_;

// Result accessors
pub const resultStatus = fe.resultStatus;
pub const resStatus = fe.resStatus;
pub const resultErrorMessage = fe.resultErrorMessage;
pub const resultVerboseErrorMessage = fe.resultVerboseErrorMessage;
pub const resultErrorField = fe.resultErrorField;
pub const ntuples = fe.ntuples;
pub const nfields = fe.nfields;
pub const binaryTuples = fe.binaryTuples;
pub const fname = fe.fname;
pub const fnumber = fe.fnumber;
pub const ftable = fe.ftable;
pub const ftablecol = fe.ftablecol;
pub const fformat = fe.fformat;
pub const ftype = fe.ftype;
pub const fsize = fe.fsize;
pub const fmod = fe.fmod;
pub const cmdStatus = fe.cmdStatus;
pub const oidStatus = fe.oidStatus;
pub const oidValue = fe.oidValue;
pub const cmdTuples = fe.cmdTuples;
pub const getvalue = fe.getvalue;
pub const getlength = fe.getlength;
pub const getisnull = fe.getisnull;
pub const nparams = fe.nparams;
pub const paramtype = fe.paramtype;

// Describe functions
pub const describePrepared = fe.describePrepared;
pub const describePortal = fe.describePortal;
pub const sendDescribePrepared = fe.sendDescribePrepared;
pub const sendDescribePortal = fe.sendDescribePortal;

// Close functions
pub const closePrepared = fe.closePrepared;
pub const closePortal = fe.closePortal;
pub const sendClosePrepared = fe.sendClosePrepared;
pub const sendClosePortal = fe.sendClosePortal;

// Memory management
pub const clear = fe.clear;
pub const freemem = fe.freemem;

// Result creation/manipulation
pub const makeEmptyPGresult = fe.makeEmptyPGresult;
pub const copyResult = fe.copyResult;
pub const setResultAttrs = fe.setResultAttrs;
pub const resultAlloc = fe.resultAlloc;
pub const resultMemorySize = fe.resultMemorySize;
pub const setvalue = fe.setvalue;

// String escaping
pub const escapeStringConn = fe.escapeStringConn;
pub const escapeLiteral = fe.escapeLiteral;
pub const escapeIdentifier = fe.escapeIdentifier;
pub const escapeByteaConn = fe.escapeByteaConn;
pub const unescapeBytea = fe.unescapeBytea;
pub const escapeString = fe.escapeString;
pub const escapeBytea = fe.escapeBytea;

// Printing
pub const print = fe.print;
pub const displayTuples = fe.displayTuples;
pub const printTuples = fe.printTuples;

// Large objects
pub const lo_open = fe.lo_open;
pub const lo_close = fe.lo_close;
pub const lo_read = fe.lo_read;
pub const lo_write = fe.lo_write;
pub const lo_lseek = fe.lo_lseek;
pub const lo_lseek64 = fe.lo_lseek64;
pub const lo_creat = fe.lo_creat;
pub const lo_create = fe.lo_create;
pub const lo_tell = fe.lo_tell;
pub const lo_tell64 = fe.lo_tell64;
pub const lo_truncate = fe.lo_truncate;
pub const lo_truncate64 = fe.lo_truncate64;
pub const lo_unlink = fe.lo_unlink;
pub const lo_import = fe.lo_import;
pub const lo_import_with_oid = fe.lo_import_with_oid;
pub const lo_export = fe.lo_export;

// Misc functions
pub const libVersion = fe.libVersion;
pub const socketPoll = fe.socketPoll;
pub const getCurrentTimeUSec = fe.getCurrentTimeUSec;
pub const mblen = fe.mblen;
pub const mblenBounded = fe.mblenBounded;
pub const dsplen = fe.dsplen;
pub const env2encoding = fe.env2encoding;

// Auth functions
pub const encryptPassword = fe.encryptPassword;
pub const encryptPasswordConn = fe.encryptPasswordConn;
pub const changePassword = fe.changePassword;
pub const setAuthDataHook = fe.setAuthDataHook;
pub const getAuthDataHook = fe.getAuthDataHook;
pub const defaultAuthDataHook = fe.defaultAuthDataHook;

// Encoding functions
pub const char_to_encoding = fe.char_to_encoding;
pub const encoding_to_char = fe.encoding_to_char;
pub const valid_server_encoding_id = fe.valid_server_encoding_id;

// Events API
pub const EventId = events.EventId;
pub const EventRegister = events.EventRegister;
pub const EventConnReset = events.EventConnReset;
pub const EventConnDestroy = events.EventConnDestroy;
pub const EventResultCreate = events.EventResultCreate;
pub const EventResultCopy = events.EventResultCopy;
pub const EventResultDestroy = events.EventResultDestroy;
pub const EventProc = events.EventProc;
pub const registerEventProc = events.registerEventProc;
pub const setInstanceData = events.setInstanceData;
pub const instanceData = events.instanceData;
pub const resultSetInstanceData = events.resultSetInstanceData;
pub const resultInstanceData = events.resultInstanceData;
pub const fireResultCreateEvents = events.fireResultCreateEvents;

// Constants
pub const PG_COPYRES_ATTRS = fe.PG_COPYRES_ATTRS;
pub const PG_COPYRES_TUPLES = fe.PG_COPYRES_TUPLES;
pub const PG_COPYRES_EVENTS = fe.PG_COPYRES_EVENTS;
pub const PG_COPYRES_NOTICEHOOKS = fe.PG_COPYRES_NOTICEHOOKS;
pub const PQ_QUERY_PARAM_MAX_LIMIT = fe.PQ_QUERY_PARAM_MAX_LIMIT;
pub const PQTRACE_SUPPRESS_TIMESTAMPS = fe.PQTRACE_SUPPRESS_TIMESTAMPS;
pub const PQTRACE_REGRESS_MODE = fe.PQTRACE_REGRESS_MODE;
pub const noPasswordSupplied = fe.noPasswordSupplied;

// Feature detection constants
pub const LIBPQ_HAS_PIPELINING = fe.LIBPQ_HAS_PIPELINING;
pub const LIBPQ_HAS_TRACE_FLAGS = fe.LIBPQ_HAS_TRACE_FLAGS;
pub const LIBPQ_HAS_SSL_LIBRARY_DETECTION = fe.LIBPQ_HAS_SSL_LIBRARY_DETECTION;
pub const LIBPQ_HAS_ASYNC_CANCEL = fe.LIBPQ_HAS_ASYNC_CANCEL;
pub const LIBPQ_HAS_CHANGE_PASSWORD = fe.LIBPQ_HAS_CHANGE_PASSWORD;
pub const LIBPQ_HAS_CHUNK_MODE = fe.LIBPQ_HAS_CHUNK_MODE;
pub const LIBPQ_HAS_CLOSE_PREPARED = fe.LIBPQ_HAS_CLOSE_PREPARED;
pub const LIBPQ_HAS_SEND_PIPELINE_SYNC = fe.LIBPQ_HAS_SEND_PIPELINE_SYNC;
pub const LIBPQ_HAS_SOCKET_POLL = fe.LIBPQ_HAS_SOCKET_POLL;
pub const LIBPQ_HAS_FULL_PROTOCOL_VERSION = fe.LIBPQ_HAS_FULL_PROTOCOL_VERSION;
pub const LIBPQ_HAS_PROMPT_OAUTH_DEVICE = fe.LIBPQ_HAS_PROMPT_OAUTH_DEVICE;

// Error field identifiers from postgres_ext.h
pub const PG_DIAG_SEVERITY = ext.PG_DIAG_SEVERITY;
pub const PG_DIAG_SEVERITY_NONLOCALIZED = ext.PG_DIAG_SEVERITY_NONLOCALIZED;
pub const PG_DIAG_SQLSTATE = ext.PG_DIAG_SQLSTATE;
pub const PG_DIAG_MESSAGE_PRIMARY = ext.PG_DIAG_MESSAGE_PRIMARY;
pub const PG_DIAG_MESSAGE_DETAIL = ext.PG_DIAG_MESSAGE_DETAIL;
pub const PG_DIAG_MESSAGE_HINT = ext.PG_DIAG_MESSAGE_HINT;
pub const PG_DIAG_STATEMENT_POSITION = ext.PG_DIAG_STATEMENT_POSITION;
pub const PG_DIAG_INTERNAL_POSITION = ext.PG_DIAG_INTERNAL_POSITION;
pub const PG_DIAG_INTERNAL_QUERY = ext.PG_DIAG_INTERNAL_QUERY;
pub const PG_DIAG_CONTEXT = ext.PG_DIAG_CONTEXT;
pub const PG_DIAG_SCHEMA_NAME = ext.PG_DIAG_SCHEMA_NAME;
pub const PG_DIAG_TABLE_NAME = ext.PG_DIAG_TABLE_NAME;
pub const PG_DIAG_COLUMN_NAME = ext.PG_DIAG_COLUMN_NAME;
pub const PG_DIAG_DATATYPE_NAME = ext.PG_DIAG_DATATYPE_NAME;
pub const PG_DIAG_CONSTRAINT_NAME = ext.PG_DIAG_CONSTRAINT_NAME;
pub const PG_DIAG_SOURCE_FILE = ext.PG_DIAG_SOURCE_FILE;
pub const PG_DIAG_SOURCE_LINE = ext.PG_DIAG_SOURCE_LINE;
pub const PG_DIAG_SOURCE_FUNCTION = ext.PG_DIAG_SOURCE_FUNCTION;