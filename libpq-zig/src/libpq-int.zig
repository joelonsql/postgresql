const std = @import("std");
const fe = @import("fe.zig");
const events = @import("events.zig");
const pqexpbuffer = @import("pqexpbuffer.zig");
const pqcomm = @import("pqcomm.zig");
const encoding = @import("encoding.zig");

// Re-export needed types
pub const Oid = fe.Oid;
pub const ExecStatusType = fe.ExecStatusType;
pub const ConnStatusType = fe.ConnStatusType;
pub const PGTransactionStatusType = fe.PGTransactionStatusType;
pub const PGVerbosity = fe.PGVerbosity;
pub const PGContextVisibility = fe.PGContextVisibility;
pub const PGpipelineStatus = fe.PGpipelineStatus;
pub const PQnoticeReceiver = fe.PQnoticeReceiver;
pub const PQnoticeProcessor = fe.PQnoticeProcessor;
pub const PGresAttDesc = fe.PGresAttDesc;
pub const pg_int64 = fe.pg_int64;

// POSTGRES backend dependent Constants
pub const CMDSTATUS_LEN = 64; // should match COMPLETION_TAG_BUFSIZE

// Null length marker for result values
pub const NULL_LEN = -1; // pg_result len for NULL value

// Data about a single parameter of a prepared statement
pub const PGresParamDesc = struct {
    typid: Oid, // type id
};

// Data for a single attribute of a single tuple
pub const PGresAttValue = struct {
    len: c_int, // length in bytes of the value
    value: [*]u8, // actual value, plus terminating zero byte
};

// Typedef for message-field list entries
pub const PGMessageField = struct {
    next: ?*PGMessageField, // list link
    code: u8, // field code
    contents: []u8, // value, nul-terminated
};

// Fields needed for notice handling
pub const PGNoticeHooks = struct {
    noticeRec: ?PQnoticeReceiver, // notice message receiver
    noticeRecArg: ?*anyopaque,
    noticeProc: ?PQnoticeProcessor, // notice message processor
    noticeProcArg: ?*anyopaque,
};

pub const PGEvent = struct {
    proc: events.EventProc, // the function to call on events
    name: []const u8, // used only for error messages
    passThrough: ?*anyopaque, // pointer supplied at registration time
    data: ?*anyopaque, // optional state (instance) data
    resultInitialized: bool, // T if RESULTCREATE/COPY succeeded
};

// Subsidiary-storage management structure for PGresult
pub const PGresult_data = struct {
    next: ?*PGresult_data, // link to next block, or NULL
    space: [1]u8, // dummy for accessing block as bytes
};

// Internal representation of PGresult
pub const pg_result_internal = struct {
    ntups: c_int,
    numAttributes: c_int,
    attDescs: ?[*]PGresAttDesc,
    tuples: ?[*][*]PGresAttValue, // each PGresult tuple is an array of PGresAttValue's
    tupArrSize: c_int, // allocated size of tuples array
    numParameters: c_int,
    paramDescs: ?[*]PGresParamDesc,
    resultStatus: ExecStatusType,
    cmdStatus: [CMDSTATUS_LEN]u8, // cmd status from the query
    binary: c_int, // binary tuple values if binary == 1, otherwise text

    // These fields are copied from the originating PGconn, so that operations
    // on the PGresult don't have to reference the PGconn.
    noticeHooks: PGNoticeHooks,
    events: ?[*]PGEvent,
    nEvents: c_int,
    client_encoding: c_int, // encoding id

    // Error information (all NULL if not an error result). errMsg is the
    // "overall" error message returned by PQresultErrorMessage. If we have
    // per-field info then it is stored in a linked list.
    errMsg: ?[*:0]u8, // error message, or NULL if no error
    errFields: ?*PGMessageField, // message broken into fields
    errQuery: ?[*:0]u8, // text of triggering query, if available

    // All NULL attributes in the query result point to this null string
    null_field: [1]u8,

    // Space management information
    curBlock: ?*PGresult_data, // most recently allocated block
    curOffset: c_int, // start offset of free space in block
    spaceLeft: c_int, // number of free bytes remaining in block
    memorySize: usize, // total space allocated for this PGresult
};

// Protocol versions
pub const PG_PROTOCOL_MAJOR_3 = 3;
pub const PG_PROTOCOL_MINOR_0 = 0;
pub const PG_PROTOCOL_MINOR_2 = 2;

// Socket communication states
pub const PGCommState = enum {
    COMM_IDLE, // No communication in progress
    COMM_BUSY, // Communication in progress
};

// Async states
pub const PGAsyncStatusType = enum {
    PGASYNC_IDLE, // Connection is idle
    PGASYNC_BUSY, // Query in progress
    PGASYNC_READY, // Result ready to be consumed
    PGASYNC_READY_MORE, // More results ready
    PGASYNC_COPY_IN, // Copy In data transfer in progress
    PGASYNC_COPY_OUT, // Copy Out data transfer in progress
    PGASYNC_COPY_BOTH, // Copy Both data transfer in progress
    PGASYNC_PIPELINE_IDLE, // Pipeline idle
};

// Query processing states
pub const PGQueryClass = enum {
    PGQUERY_SIMPLE, // Simple Query protocol
    PGQUERY_EXTENDED, // Extended Query protocol
    PGQUERY_PREPARE, // Prepare statement
    PGQUERY_DESCRIBE, // Describe statement or portal
    PGQUERY_SYNC, // Sync (end of pipeline)
    PGQUERY_CLOSE, // Close statement or portal
};

// Target server type
pub const PGTargetServerType = enum {
    SERVER_TYPE_ANY,
    SERVER_TYPE_READ_WRITE,
    SERVER_TYPE_READ_ONLY,
    SERVER_TYPE_PRIMARY,
    SERVER_TYPE_STANDBY,
    SERVER_TYPE_PREFER_STANDBY,
    SERVER_TYPE_PREFER_STANDBY_PASS2,
};

// Load balance type
pub const PGLoadBalanceType = enum {
    LOAD_BALANCE_DISABLE,
    LOAD_BALANCE_RANDOM,
};

// Options for PQcopyResult
pub const PG_COPYRES_ATTRS = 0x01;
pub const PG_COPYRES_TUPLES = 0x02; // Implies PG_COPYRES_ATTRS
pub const PG_COPYRES_EVENTS = 0x04;
pub const PG_COPYRES_NOTICEHOOKS = 0x08;

// Minimum and maximum protocol version this library supports
pub const PG_PROTOCOL_LIBPQ_MIN = pqcomm.PG_PROTOCOL(3, 0);
pub const PG_PROTOCOL_LIBPQ_MAX = pqcomm.PG_PROTOCOL_LATEST;

// Connection defaults
pub const DefaultHost = "localhost";
pub const DefaultPort = "5432";
pub const DefaultOption = "";
pub const DefaultSSLMode = "prefer";
pub const DefaultSSLCertMode = "allow";
pub const DefaultSSLKeyPassHook = "default";
pub const DefaultGSSMode = "prefer";

// Environment variables
pub const PGHOST_ENV = "PGHOST";
pub const PGPORT_ENV = "PGPORT";
pub const PGDATABASE_ENV = "PGDATABASE";
pub const PGUSER_ENV = "PGUSER";
pub const PGPASSWORD_ENV = "PGPASSWORD";
pub const PGPASSFILE_ENV = "PGPASSFILE";
pub const PGSERVICE_ENV = "PGSERVICE";
pub const PGSERVICEFILE_ENV = "PGSERVICEFILE";
pub const PGOPTIONS_ENV = "PGOPTIONS";
pub const PGAPPNAME_ENV = "PGAPPNAME";
pub const PGSSLMODE_ENV = "PGSSLMODE";
pub const PGREQUIRESSL_ENV = "PGREQUIRESSL"; // deprecated
pub const PGSSLCERT_ENV = "PGSSLCERT";
pub const PGSSLKEY_ENV = "PGSSLKEY";
pub const PGSSLROOTCERT_ENV = "PGSSLROOTCERT";
pub const PGSSLCRL_ENV = "PGSSLCRL";
pub const PGSSLCRLDIR_ENV = "PGSSLCRLDIR";
pub const PGREQUIREPEER_ENV = "PGREQUIREPEER";
pub const PGCONNECT_TIMEOUT_ENV = "PGCONNECT_TIMEOUT";
pub const PGGSSDELEGATION_ENV = "PGGSSDELEGATION";

// Internal connection options structure
pub const PGConnectionOption = struct {
    keyword: []const u8, // Option keyword
    envvar: ?[]const u8, // Environment variable name
    compiled: ?[]const u8, // Compiled-in default value
    val: ?[]u8, // Current value
    label: []const u8, // Label for connect dialog
    dispchar: []const u8, // How to display in dialog
    dispsize: c_int, // Field size in dialog
};

// Structure to hold connection parameters
pub const PGConnParams = struct {
    allocator: std.mem.Allocator,
    options: std.ArrayList(PGConnectionOption),
    
    pub fn init(allocator: std.mem.Allocator) PGConnParams {
        return .{
            .allocator = allocator,
            .options = std.ArrayList(PGConnectionOption).init(allocator),
        };
    }
    
    pub fn deinit(self: *PGConnParams) void {
        for (self.options.items) |*opt| {
            if (opt.val) |val| {
                self.allocator.free(val);
            }
        }
        self.options.deinit();
    }
    
    pub fn getValue(self: *const PGConnParams, keyword: []const u8) ?[]const u8 {
        for (self.options.items) |opt| {
            if (std.mem.eql(u8, opt.keyword, keyword)) {
                return opt.val;
            }
        }
        return null;
    }
    
    pub fn setValue(self: *PGConnParams, keyword: []const u8, value: []const u8) !void {
        for (self.options.items) |*opt| {
            if (std.mem.eql(u8, opt.keyword, keyword)) {
                if (opt.val) |old_val| {
                    self.allocator.free(old_val);
                }
                opt.val = try self.allocator.dupe(u8, value);
                return;
            }
        }
        // Not found, add new option
        try self.options.append(.{
            .keyword = keyword,
            .envvar = null,
            .compiled = null,
            .val = try self.allocator.dupe(u8, value),
            .label = keyword,
            .dispchar = "",
            .dispsize = 20,
        });
    }
};

// Connection state machine states for non-blocking connect
pub const PostgresPollingStatusType = fe.PostgresPollingStatusType;

// Authentication states
pub const PGAuthState = enum {
    AUTH_IDLE,
    AUTH_REQ_WAITING,
    AUTH_REQ_RECEIVED,
    AUTH_RESPONSE_SENT,
    AUTH_COMPLETED,
};

// Main internal connection structure
pub const pg_conn_internal = struct {
    allocator: std.mem.Allocator,
    
    // Connection parameters
    params: PGConnParams,
    
    // Connection state
    status: ConnStatusType,
    trans_status: PGTransactionStatusType,
    nonblocking: bool,
    
    // Socket and network state
    sock: ?std.os.socket_t,
    addr: ?std.net.Address,
    
    // Protocol state
    protocol_version: u32,
    server_version: c_int,
    
    // Authentication state
    auth_state: PGAuthState,
    auth_req_type: u32,
    
    // Error handling
    error_message: pqexpbuffer.PQExpBufferData,
    
    // Notice handling
    notice_hooks: PGNoticeHooks,
    
    // Event handling
    events: std.ArrayList(PGEvent),
    
    // Result handling
    result: ?*pg_result_internal,
    next_result: ?*pg_result_internal,
    
    // Pipeline mode
    pipeline_status: PGpipelineStatus,
    
    // Async query state
    async_status: PGAsyncStatusType,
    query_class: PGQueryClass,
    
    // Input buffer
    in_buffer: pqexpbuffer.PQExpBufferData,
    in_cursor: usize,
    
    // Output buffer
    out_buffer: pqexpbuffer.PQExpBufferData,
    
    // Server parameters
    server_params: std.StringHashMap([]const u8),
    
    // Encoding
    client_encoding: c_int,
    std_strings: bool,
    
    // Cancel info
    backend_pid: c_int,
    backend_key: u32,
    
    // Options
    verbosity: PGVerbosity,
    show_context: PGContextVisibility,
    
    // Target server type
    target_server_type: PGTargetServerType,
    
    // TLS state
    tls_client: ?std.crypto.tls.Client,
    require_ssl: bool,
    
    pub fn init(allocator: std.mem.Allocator) !*pg_conn_internal {
        const conn = try allocator.create(pg_conn_internal);
        errdefer allocator.destroy(conn);
        
        conn.* = .{
            .allocator = allocator,
            .params = PGConnParams.init(allocator),
            .status = .CONNECTION_BAD,
            .trans_status = .PQTRANS_IDLE,
            .nonblocking = false,
            .sock = null,
            .addr = null,
            .protocol_version = 0,
            .server_version = 0,
            .auth_state = .AUTH_IDLE,
            .auth_req_type = 0,
            .error_message = undefined,
            .notice_hooks = .{
                .noticeRec = null,
                .noticeRecArg = null,
                .noticeProc = null,
                .noticeProcArg = null,
            },
            .events = std.ArrayList(PGEvent).init(allocator),
            .result = null,
            .next_result = null,
            .pipeline_status = .PQ_PIPELINE_OFF,
            .async_status = .PGASYNC_IDLE,
            .query_class = .PGQUERY_SIMPLE,
            .in_buffer = undefined,
            .in_cursor = 0,
            .out_buffer = undefined,
            .server_params = std.StringHashMap([]const u8).init(allocator),
            .client_encoding = @intFromEnum(encoding.pg_enc.PG_SQL_ASCII),
            .std_strings = false,
            .backend_pid = 0,
            .backend_key = 0,
            .verbosity = .PQERRORS_DEFAULT,
            .show_context = .PQSHOW_CONTEXT_ERRORS,
            .target_server_type = .SERVER_TYPE_ANY,
            .tls_client = null,
            .require_ssl = false,
        };
        
        pqexpbuffer.initPQExpBuffer(&conn.error_message, allocator);
        pqexpbuffer.initPQExpBuffer(&conn.in_buffer, allocator);
        pqexpbuffer.initPQExpBuffer(&conn.out_buffer, allocator);
        
        return conn;
    }
    
    pub fn deinit(self: *pg_conn_internal) void {
        if (self.sock) |sock| {
            std.os.close(sock);
        }
        
        if (self.tls_client) |*client| {
            _ = client.close();
        }
        
        self.params.deinit();
        
        pqexpbuffer.termPQExpBuffer(&self.error_message, self.allocator);
        pqexpbuffer.termPQExpBuffer(&self.in_buffer, self.allocator);
        pqexpbuffer.termPQExpBuffer(&self.out_buffer, self.allocator);
        
        // Free server parameters
        var iter = self.server_params.iterator();
        while (iter.next()) |entry| {
            self.allocator.free(entry.key_ptr.*);
            self.allocator.free(entry.value_ptr.*);
        }
        self.server_params.deinit();
        
        self.events.deinit();
        
        // Free any results
        if (self.result) |res| {
            clearResult(res, self.allocator);
        }
        if (self.next_result) |res| {
            clearResult(res, self.allocator);
        }
        
        self.allocator.destroy(self);
    }
    
    pub fn setErrorMessage(self: *pg_conn_internal, comptime fmt: []const u8, args: anytype) void {
        pqexpbuffer.printfPQExpBuffer(&self.error_message, self.allocator, fmt, args);
    }
    
    pub fn appendErrorMessage(self: *pg_conn_internal, comptime fmt: []const u8, args: anytype) void {
        pqexpbuffer.appendPQExpBuffer(&self.error_message, self.allocator, fmt, args);
    }
};

// Helper to free a result structure
fn clearResult(result: *pg_result_internal, allocator: std.mem.Allocator) void {
    // Free attribute descriptors
    if (result.attDescs) |descs| {
        for (0..@intCast(result.numAttributes)) |i| {
            allocator.free(std.mem.span(descs[i].name));
        }
        allocator.free(descs[0..@intCast(result.numAttributes)]);
    }
    
    // Free tuples
    if (result.tuples) |tuples| {
        for (0..@intCast(result.ntups)) |i| {
            if (tuples[i]) |tuple| {
                for (0..@intCast(result.numAttributes)) |j| {
                    if (tuple[j].len >= 0) {
                        allocator.free(tuple[j].value[0..@intCast(tuple[j].len + 1)]);
                    }
                }
                allocator.free(tuple[0..@intCast(result.numAttributes)]);
            }
        }
        allocator.free(tuples[0..@intCast(result.tupArrSize)]);
    }
    
    // Free parameter descriptors
    if (result.paramDescs) |descs| {
        allocator.free(descs[0..@intCast(result.numParameters)]);
    }
    
    // Free error message
    if (result.errMsg) |msg| {
        allocator.free(std.mem.span(msg));
    }
    
    // Free error fields
    var field = result.errFields;
    while (field) |f| {
        const next = f.next;
        allocator.free(f.contents);
        allocator.destroy(f);
        field = next;
    }
    
    // Free error query
    if (result.errQuery) |query| {
        allocator.free(std.mem.span(query));
    }
    
    // Free events
    if (result.events) |evts| {
        for (0..@intCast(result.nEvents)) |i| {
            allocator.free(evts[i].name);
        }
        allocator.free(evts[0..@intCast(result.nEvents)]);
    }
    
    // Free result blocks
    var block = result.curBlock;
    while (block) |b| {
        const next = b.next;
        allocator.destroy(b);
        block = next;
    }
    
    allocator.destroy(result);
}