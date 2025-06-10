const std = @import("std");
const fe = @import("fe.zig");
const internal = @import("libpq-int.zig");
const connect = @import("fe-connect.zig");
const pqexpbuffer = @import("pqexpbuffer.zig");
const protocol = @import("protocol.zig");
const encoding = @import("encoding.zig");

// Re-export from fe-connect.zig
pub const connectStart = connect.connectStart;
pub const connectStartParams = connect.connectStartParams;
pub const connectPoll = connect.connectPoll;
pub const connectdb = connect.connectdb;
pub const connectdbParams = connect.connectdbParams;
pub const setdbLogin = connect.setdbLogin;
pub const finish = connect.finish;
pub const conndefaults = connect.conndefaults;
pub const conninfoParse = connect.conninfoParse;
pub const conninfo = connect.conninfo;
pub const conninfoFree = connect.conninfoFree;
pub const setGlobalAllocator = connect.setGlobalAllocator;
pub const getGlobalAllocator = connect.getGlobalAllocator;

// Helper to get internal connection
fn getInternalConn(conn: *fe.PGconn) *internal.pg_conn_internal {
    return @ptrCast(@alignCast(conn));
}

// Helper to get internal result
fn getInternalResult(res: *fe.PGresult) *internal.pg_result_internal {
    return @ptrCast(@alignCast(res));
}

// Connection reset functions
pub fn resetStart(conn: *fe.PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Close existing connection
    if (conn_internal.sock) |sock| {
        std.os.close(sock);
        conn_internal.sock = null;
    }
    
    // Reset state
    conn_internal.status = .CONNECTION_NEEDED;
    conn_internal.async_status = .PGASYNC_IDLE;
    
    return 1;
}

pub fn resetPoll(conn: *fe.PGconn) fe.PostgresPollingStatusType {
    return connectPoll(conn);
}

pub fn reset(conn: *fe.PGconn) void {
    _ = resetStart(conn);
    while (true) {
        const status = resetPoll(conn);
        if (status == .PGRES_POLLING_OK or status == .PGRES_POLLING_FAILED) {
            break;
        }
        // In blocking mode, we would wait here
        std.time.sleep(10 * std.time.ns_per_ms);
    }
}

// Cancel functions
pub fn cancelCreate(conn: *fe.PGconn) ?*fe.PGcancelConn {
    _ = conn;
    // TODO: Implement
    return null;
}

pub fn cancelStart(cancelConn: *fe.PGcancelConn) c_int {
    _ = cancelConn;
    // TODO: Implement
    return 0;
}

pub fn cancelBlocking(cancelConn: *fe.PGcancelConn) c_int {
    _ = cancelConn;
    // TODO: Implement
    return 0;
}

pub fn cancelPoll(cancelConn: *fe.PGcancelConn) fe.PostgresPollingStatusType {
    _ = cancelConn;
    // TODO: Implement
    return .PGRES_POLLING_FAILED;
}

pub fn cancelStatus(cancelConn: *const fe.PGcancelConn) fe.ConnStatusType {
    _ = cancelConn;
    // TODO: Implement
    return .CONNECTION_BAD;
}

pub fn cancelSocket(cancelConn: *const fe.PGcancelConn) c_int {
    _ = cancelConn;
    // TODO: Implement
    return -1;
}

pub fn cancelErrorMessage(cancelConn: *const fe.PGcancelConn) [*:0]u8 {
    _ = cancelConn;
    // TODO: Implement
    return @constCast("not implemented");
}

pub fn cancelReset(cancelConn: *fe.PGcancelConn) void {
    _ = cancelConn;
    // TODO: Implement
}

pub fn cancelFinish(cancelConn: *fe.PGcancelConn) void {
    _ = cancelConn;
    // TODO: Implement
}

pub fn getCancel(conn: *fe.PGconn) ?*fe.PGcancel {
    _ = conn;
    // TODO: Implement
    return null;
}

pub fn freeCancel(cancel_: *fe.PGcancel) void {
    _ = cancel_;
    // TODO: Implement
}

pub fn cancel(cancel_: *fe.PGcancel, errbuf: [*]u8, errbufsize: c_int) c_int {
    _ = cancel_;
    _ = errbuf;
    _ = errbufsize;
    // TODO: Implement
    return 0;
}

pub fn requestCancel(conn: *fe.PGconn) c_int {
    _ = conn;
    // TODO: Implement
    return 0;
}

// Connection accessor functions
pub fn db(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const dbname = conn_internal.params.getValue("dbname") orelse "";
    return @constCast(@ptrCast(dbname.ptr));
}

pub fn service(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const svc = conn_internal.params.getValue("service") orelse "";
    return @constCast(@ptrCast(svc.ptr));
}

pub fn user(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const usr = conn_internal.params.getValue("user") orelse "";
    return @constCast(@ptrCast(usr.ptr));
}

pub fn pass(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const pwd = conn_internal.params.getValue("password") orelse "";
    return @constCast(@ptrCast(pwd.ptr));
}

pub fn host(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const h = conn_internal.params.getValue("host") orelse "";
    return @constCast(@ptrCast(h.ptr));
}

pub fn hostaddr(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const ha = conn_internal.params.getValue("hostaddr") orelse "";
    return @constCast(@ptrCast(ha.ptr));
}

pub fn port(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const p = conn_internal.params.getValue("port") orelse "";
    return @constCast(@ptrCast(p.ptr));
}

pub fn tty(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const t = conn_internal.params.getValue("tty") orelse "";
    return @constCast(@ptrCast(t.ptr));
}

pub fn options(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const opt = conn_internal.params.getValue("options") orelse "";
    return @constCast(@ptrCast(opt.ptr));
}

pub fn status(conn: *const fe.PGconn) fe.ConnStatusType {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.status;
}

pub fn transactionStatus(conn: *const fe.PGconn) fe.PGTransactionStatusType {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.trans_status;
}

pub fn parameterStatus(conn: *const fe.PGconn, paramName: [*:0]const u8) ?[*:0]const u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    const value = conn_internal.server_params.get(std.mem.span(paramName)) orelse return null;
    return @ptrCast(value.ptr);
}

pub fn protocolVersion(conn: *const fe.PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return @intCast(pqcomm.PG_PROTOCOL_MAJOR(conn_internal.protocol_version));
}

pub fn fullProtocolVersion(conn: *const fe.PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return @intCast(pqcomm.PG_PROTOCOL_FULL(conn_internal.protocol_version));
}

pub fn serverVersion(conn: *const fe.PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.server_version;
}

pub fn errorMessage(conn: *const fe.PGconn) [*:0]u8 {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.error_message.data;
}

pub fn socket(conn: *const fe.PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return if (conn_internal.sock) |sock| @intCast(sock) else -1;
}

pub fn backendPID(conn: *const fe.PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.backend_pid;
}

pub fn pipelineStatus(conn: *const fe.PGconn) fe.PGpipelineStatus {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.pipeline_status;
}

pub fn connectionNeedsPassword(conn: *const fe.PGconn) c_int {
    _ = conn;
    // TODO: Implement properly
    return 0;
}

pub fn connectionUsedPassword(conn: *const fe.PGconn) c_int {
    _ = conn;
    // TODO: Implement properly
    return 0;
}

pub fn connectionUsedGSSAPI(conn: *const fe.PGconn) c_int {
    _ = conn;
    // TODO: Implement properly
    return 0;
}

pub fn clientEncoding(conn: *const fe.PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return conn_internal.client_encoding;
}

pub fn setClientEncoding(conn: *fe.PGconn, enc: [*:0]const u8) c_int {
    const conn_internal = getInternalConn(conn);
    const encoding_id = encoding.pg_char_to_encoding(std.mem.span(enc));
    if (encoding_id < 0) {
        return -1;
    }
    conn_internal.client_encoding = encoding_id;
    return 0;
}

// SSL functions
pub fn sslInUse(conn: *fe.PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    return if (conn_internal.tls_client != null) 1 else 0;
}

pub fn sslStruct(conn: *fe.PGconn, struct_name: [*:0]const u8) ?*anyopaque {
    _ = conn;
    _ = struct_name;
    // We don't expose internal TLS structures
    return null;
}

pub fn sslAttribute(conn: *fe.PGconn, attribute_name: [*:0]const u8) ?[*:0]const u8 {
    _ = conn;
    _ = attribute_name;
    // TODO: Implement SSL attributes
    return null;
}

pub fn sslAttributeNames(conn: *fe.PGconn) ?[*]const [*:0]const u8 {
    _ = conn;
    // TODO: Implement
    return null;
}

pub fn getssl(conn: *fe.PGconn) ?*anyopaque {
    _ = conn;
    // We use Zig's TLS, not OpenSSL
    return null;
}

pub fn initSSL(do_init: c_int) void {
    _ = do_init;
    // No-op for Zig implementation
}

pub fn initOpenSSL(do_ssl: c_int, do_crypto: c_int) void {
    _ = do_ssl;
    _ = do_crypto;
    // No-op for Zig implementation
}

// GSSAPI functions
pub fn gssEncInUse(conn: *fe.PGconn) c_int {
    _ = conn;
    // GSSAPI not supported in this implementation
    return 0;
}

pub fn getgssctx(conn: *fe.PGconn) ?*anyopaque {
    _ = conn;
    // GSSAPI not supported
    return null;
}

// Error handling
pub fn setErrorVerbosity(conn: *fe.PGconn, verbosity: fe.PGVerbosity) fe.PGVerbosity {
    const conn_internal = getInternalConn(conn);
    const old = conn_internal.verbosity;
    conn_internal.verbosity = verbosity;
    return old;
}

pub fn setErrorContextVisibility(conn: *fe.PGconn, show_context: fe.PGContextVisibility) fe.PGContextVisibility {
    const conn_internal = getInternalConn(conn);
    const old = conn_internal.show_context;
    conn_internal.show_context = show_context;
    return old;
}

pub fn setNoticeReceiver(conn: *fe.PGconn, proc: fe.PQnoticeReceiver, arg: ?*anyopaque) fe.PQnoticeReceiver {
    const conn_internal = getInternalConn(conn);
    const old = conn_internal.notice_hooks.noticeRec;
    conn_internal.notice_hooks.noticeRec = proc;
    conn_internal.notice_hooks.noticeRecArg = arg;
    return old.?;
}

pub fn setNoticeProcessor(conn: *fe.PGconn, proc: fe.PQnoticeProcessor, arg: ?*anyopaque) fe.PQnoticeProcessor {
    const conn_internal = getInternalConn(conn);
    const old = conn_internal.notice_hooks.noticeProc;
    conn_internal.notice_hooks.noticeProc = proc;
    conn_internal.notice_hooks.noticeProcArg = arg;
    return old.?;
}

pub fn registerThreadLock(newhandler: fe.pgthreadlock_t) fe.pgthreadlock_t {
    _ = newhandler;
    // TODO: Implement thread locking
    return defaultThreadLock;
}

fn defaultThreadLock(acquire: c_int) callconv(.C) void {
    _ = acquire;
    // Default implementation - no-op
}

// Tracing
pub fn trace(conn: *fe.PGconn, debug_port: *std.c.FILE) void {
    _ = conn;
    _ = debug_port;
    // TODO: Implement tracing
}

pub fn untrace(conn: *fe.PGconn) void {
    _ = conn;
    // TODO: Implement
}

pub fn setTraceFlags(conn: *fe.PGconn, flags: c_int) void {
    _ = conn;
    _ = flags;
    // TODO: Implement
}

// Version information
pub fn libVersion() c_int {
    // Return a version number compatible with libpq
    // Format: major * 10000 + minor * 100 + revision
    return 150000; // Version 15.0.0
}

// Memory management
pub fn freemem(ptr: *anyopaque) void {
    const allocator = getGlobalAllocator();
    // We need to know the size, which is problematic
    // For now, we'll use a simple approach
    _ = allocator;
    _ = ptr;
    // TODO: Implement proper memory tracking
}

// Encoding functions
pub fn char_to_encoding(name: [*:0]const u8) c_int {
    return encoding.pg_char_to_encoding(std.mem.span(name));
}

pub fn encoding_to_char(enc: c_int) [*:0]const u8 {
    const name = encoding.pg_encoding_to_char(enc) orelse "UNKNOWN";
    return @ptrCast(name.ptr);
}

pub fn valid_server_encoding_id(enc: c_int) c_int {
    return if (encoding.pg_valid_server_encoding_id(enc)) 1 else 0;
}