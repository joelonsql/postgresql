const std = @import("std");
const fe = @import("fe.zig");
const internal = @import("libpq-int.zig");
const pqexpbuffer = @import("pqexpbuffer.zig");
const pqcomm = @import("pqcomm.zig");
const protocol = @import("protocol.zig");
const encoding = @import("encoding.zig");

const PGconn = fe.PGconn;
const ConnStatusType = fe.ConnStatusType;
const PostgresPollingStatusType = fe.PostgresPollingStatusType;
const PQconninfoOption = fe.PQconninfoOption;

// Default connection parameters
const default_options = [_]internal.PGConnectionOption{
    .{ .keyword = "host", .envvar = internal.PGHOST_ENV, .compiled = internal.DefaultHost, .val = null, .label = "Database Server", .dispchar = "", .dispsize = 40 },
    .{ .keyword = "hostaddr", .envvar = null, .compiled = null, .val = null, .label = "Database Server IP Address", .dispchar = "", .dispsize = 40 },
    .{ .keyword = "port", .envvar = internal.PGPORT_ENV, .compiled = internal.DefaultPort, .val = null, .label = "Database Server Port", .dispchar = "", .dispsize = 6 },
    .{ .keyword = "dbname", .envvar = internal.PGDATABASE_ENV, .compiled = null, .val = null, .label = "Database Name", .dispchar = "", .dispsize = 20 },
    .{ .keyword = "user", .envvar = internal.PGUSER_ENV, .compiled = null, .val = null, .label = "Database User", .dispchar = "", .dispsize = 20 },
    .{ .keyword = "password", .envvar = internal.PGPASSWORD_ENV, .compiled = null, .val = null, .label = "Database Password", .dispchar = "*", .dispsize = 20 },
    .{ .keyword = "options", .envvar = internal.PGOPTIONS_ENV, .compiled = internal.DefaultOption, .val = null, .label = "Backend Options", .dispchar = "", .dispsize = 40 },
    .{ .keyword = "application_name", .envvar = internal.PGAPPNAME_ENV, .compiled = null, .val = null, .label = "Application Name", .dispchar = "", .dispsize = 64 },
    .{ .keyword = "sslmode", .envvar = internal.PGSSLMODE_ENV, .compiled = internal.DefaultSSLMode, .val = null, .label = "SSL Mode", .dispchar = "", .dispsize = 12 },
    .{ .keyword = "connect_timeout", .envvar = internal.PGCONNECT_TIMEOUT_ENV, .compiled = null, .val = null, .label = "Connect Timeout", .dispchar = "", .dispsize = 10 },
    // Add more options as needed
};

var global_allocator: ?std.mem.Allocator = null;

pub fn getGlobalAllocator() std.mem.Allocator {
    if (global_allocator == null) {
        // Use page allocator as default
        global_allocator = std.heap.page_allocator;
    }
    return global_allocator.?;
}

pub fn setGlobalAllocator(allocator: std.mem.Allocator) void {
    global_allocator = allocator;
}

// Make a new client connection to the backend
// Asynchronous (non-blocking)
pub fn connectStart(conninfo: [*:0]const u8) ?*PGconn {
    return connectStartParams(&conninfo, &conninfo, 0);
}

pub fn connectStartParams(keywords: [*]const [*:0]const u8, values: [*]const [*:0]const u8, expand_dbname: c_int) ?*PGconn {
    const allocator = getGlobalAllocator();
    
    // Create internal connection structure
    const conn_internal = internal.pg_conn_internal.init(allocator) catch {
        return null;
    };
    
    // Parse connection parameters
    if (!parseConnectionParams(conn_internal, keywords, values, expand_dbname)) {
        conn_internal.deinit();
        return null;
    }
    
    // Set initial state
    conn_internal.status = .CONNECTION_NEEDED;
    
    // Return opaque pointer
    return @ptrCast(conn_internal);
}

pub fn connectPoll(conn: *PGconn) PostgresPollingStatusType {
    const conn_internal = getInternalConn(conn);
    
    switch (conn_internal.status) {
        .CONNECTION_OK => return .PGRES_POLLING_OK,
        .CONNECTION_BAD => return .PGRES_POLLING_FAILED,
        
        .CONNECTION_NEEDED => {
            // Start connection attempt
            if (!connectDBStart(conn_internal)) {
                return .PGRES_POLLING_FAILED;
            }
            return .PGRES_POLLING_WRITING;
        },
        
        .CONNECTION_STARTED => {
            // Check if connect() completed
            return pollConnectionEstablishment(conn_internal);
        },
        
        .CONNECTION_MADE => {
            // Send startup packet
            if (!sendStartupPacket(conn_internal)) {
                return .PGRES_POLLING_FAILED;
            }
            conn_internal.status = .CONNECTION_AWAITING_RESPONSE;
            return .PGRES_POLLING_READING;
        },
        
        .CONNECTION_AWAITING_RESPONSE => {
            // Read server response
            return processServerResponse(conn_internal);
        },
        
        // Add more states as needed
        else => return .PGRES_POLLING_FAILED,
    }
}

// Synchronous (blocking)
pub fn connectdb(conninfo: [*:0]const u8) ?*PGconn {
    const conn = connectStart(conninfo) orelse return null;
    
    if (PQconnectPoll(conn)) {
        return conn;
    }
    
    return conn;
}

pub fn connectdbParams(keywords: [*]const [*:0]const u8, values: [*]const [*:0]const u8, expand_dbname: c_int) ?*PGconn {
    const conn = connectStartParams(keywords, values, expand_dbname) orelse return null;
    
    if (PQconnectPoll(conn)) {
        return conn;
    }
    
    return conn;
}

pub fn setdbLogin(pghost: ?[*:0]const u8, pgport: ?[*:0]const u8, pgoptions: ?[*:0]const u8, pgtty: ?[*:0]const u8, dbName: ?[*:0]const u8, login: ?[*:0]const u8, pwd: ?[*:0]const u8) ?*PGconn {
    var keywords = [_]?[*:0]const u8{ "host", "port", "options", "tty", "dbname", "user", "password", null };
    var values = [_]?[*:0]const u8{ pghost, pgport, pgoptions, pgtty, dbName, login, pwd, null };
    
    return connectdbParams(@ptrCast(&keywords), @ptrCast(&values), 0);
}

// Close the current connection and free the PGconn data structure
pub fn finish(conn: *PGconn) void {
    const conn_internal = getInternalConn(conn);
    conn_internal.deinit();
}

// Get info about connection options known to PQconnectdb
pub fn conndefaults() ?[*]PQconninfoOption {
    const allocator = getGlobalAllocator();
    
    const options = allocator.alloc(PQconninfoOption, default_options.len + 1) catch {
        return null;
    };
    
    for (default_options, 0..) |def_opt, i| {
        options[i] = .{
            .keyword = allocator.dupeZ(u8, def_opt.keyword) catch {
                // Clean up on failure
                for (0..i) |j| {
                    allocator.free(std.mem.span(options[j].keyword));
                    if (options[j].envvar) |env| {
                        allocator.free(std.mem.span(env));
                    }
                    if (options[j].compiled) |comp| {
                        allocator.free(std.mem.span(comp));
                    }
                    if (options[j].val) |val| {
                        allocator.free(std.mem.span(val));
                    }
                    allocator.free(std.mem.span(options[j].label));
                    allocator.free(std.mem.span(options[j].dispchar));
                }
                allocator.free(options);
                return null;
            },
            .envvar = if (def_opt.envvar) |env| allocator.dupeZ(u8, env) catch {
                allocator.free(std.mem.span(options[i].keyword));
                // Clean up previous entries...
                allocator.free(options);
                return null;
            } else null,
            .compiled = if (def_opt.compiled) |comp| allocator.dupeZ(u8, comp) catch {
                // Clean up...
                return null;
            } else null,
            .val = null,
            .label = allocator.dupeZ(u8, def_opt.label) catch {
                // Clean up...
                return null;
            },
            .dispchar = allocator.dupeZ(u8, def_opt.dispchar) catch {
                // Clean up...
                return null;
            },
            .dispsize = def_opt.dispsize,
        };
    }
    
    // Null terminator
    options[default_options.len] = .{
        .keyword = null,
        .envvar = null,
        .compiled = null,
        .val = null,
        .label = null,
        .dispchar = null,
        .dispsize = 0,
    };
    
    return options.ptr;
}

// Parse connection options in same way as PQconnectdb
pub fn conninfoParse(conninfo: [*:0]const u8, errmsg: *?[*:0]u8) ?[*]PQconninfoOption {
    const allocator = getGlobalAllocator();
    errmsg.* = null;
    
    // Get defaults first
    const options = conndefaults() orelse {
        errmsg.* = allocator.dupeZ(u8, "out of memory") catch null;
        return null;
    };
    
    // Parse the connection string
    if (!parseConnectionString(std.mem.span(conninfo), options, allocator, errmsg)) {
        conninfoFree(options);
        return null;
    }
    
    return options;
}

// Return the connection options used by a live connection
pub fn conninfo(conn: *PGconn) ?[*]PQconninfoOption {
    const conn_internal = getInternalConn(conn);
    const allocator = getGlobalAllocator();
    
    const num_opts = conn_internal.params.options.items.len;
    const options = allocator.alloc(PQconninfoOption, num_opts + 1) catch {
        return null;
    };
    
    for (conn_internal.params.options.items, 0..) |opt, i| {
        options[i] = .{
            .keyword = allocator.dupeZ(u8, opt.keyword) catch {
                // Clean up on failure
                for (0..i) |j| {
                    allocator.free(std.mem.span(options[j].keyword));
                    if (options[j].val) |val| {
                        allocator.free(std.mem.span(val));
                    }
                    // ... clean up other fields
                }
                allocator.free(options);
                return null;
            },
            .envvar = if (opt.envvar) |env| allocator.dupeZ(u8, env) catch {
                // Clean up...
                return null;
            } else null,
            .compiled = if (opt.compiled) |comp| allocator.dupeZ(u8, comp) catch {
                // Clean up...
                return null;
            } else null,
            .val = if (opt.val) |val| allocator.dupeZ(u8, val) catch {
                // Clean up...
                return null;
            } else null,
            .label = allocator.dupeZ(u8, opt.label) catch {
                // Clean up...
                return null;
            },
            .dispchar = allocator.dupeZ(u8, opt.dispchar) catch {
                // Clean up...
                return null;
            },
            .dispsize = opt.dispsize,
        };
    }
    
    // Null terminator
    options[num_opts] = .{
        .keyword = null,
        .envvar = null,
        .compiled = null,
        .val = null,
        .label = null,
        .dispchar = null,
        .dispsize = 0,
    };
    
    return options.ptr;
}

// Free the data structure returned by PQconndefaults() or PQconninfoParse()
pub fn conninfoFree(connOptions: [*]PQconninfoOption) void {
    const allocator = getGlobalAllocator();
    
    var i: usize = 0;
    while (connOptions[i].keyword != null) : (i += 1) {
        allocator.free(std.mem.span(connOptions[i].keyword.?));
        if (connOptions[i].envvar) |env| {
            allocator.free(std.mem.span(env));
        }
        if (connOptions[i].compiled) |comp| {
            allocator.free(std.mem.span(comp));
        }
        if (connOptions[i].val) |val| {
            allocator.free(std.mem.span(val));
        }
        if (connOptions[i].label) |label| {
            allocator.free(std.mem.span(label));
        }
        if (connOptions[i].dispchar) |disp| {
            allocator.free(std.mem.span(disp));
        }
    }
    
    allocator.free(connOptions[0..i + 1]);
}

// Helper functions

fn getInternalConn(conn: *PGconn) *internal.pg_conn_internal {
    return @ptrCast(@alignCast(conn));
}

fn parseConnectionParams(conn: *internal.pg_conn_internal, keywords: [*]const [*:0]const u8, values: [*]const [*:0]const u8, expand_dbname: c_int) bool {
    _ = expand_dbname; // TODO: Implement dbname expansion
    
    var i: usize = 0;
    while (keywords[i] != null) : (i += 1) {
        const keyword = std.mem.span(keywords[i]);
        const value = if (values[i] != null) std.mem.span(values[i]) else "";
        
        conn.params.setValue(keyword, value) catch {
            conn.setErrorMessage("out of memory", .{});
            return false;
        };
    }
    
    // Apply defaults and environment variables
    applyDefaults(conn) catch {
        return false;
    };
    
    return true;
}

fn applyDefaults(conn: *internal.pg_conn_internal) !void {
    for (default_options) |def_opt| {
        if (conn.params.getValue(def_opt.keyword) == null) {
            // Check environment variable
            if (def_opt.envvar) |env| {
                if (std.os.getenv(env)) |env_val| {
                    try conn.params.setValue(def_opt.keyword, env_val);
                    continue;
                }
            }
            
            // Use compiled default
            if (def_opt.compiled) |comp| {
                try conn.params.setValue(def_opt.keyword, comp);
            }
        }
    }
}

fn parseConnectionString(conninfo: []const u8, options: [*]PQconninfoOption, allocator: std.mem.Allocator, errmsg: *?[*:0]u8) bool {
    // Check if this is a URI
    if (std.mem.startsWith(u8, conninfo, "postgresql://") or 
        std.mem.startsWith(u8, conninfo, "postgres://")) {
        return parseURI(conninfo, options, allocator, errmsg);
    }
    
    // Otherwise parse as key=value pairs
    return parseKeyValueString(conninfo, options, allocator, errmsg);
}

fn parseURI(uri: []const u8, options: [*]PQconninfoOption, allocator: std.mem.Allocator, errmsg: *?[*:0]u8) bool {
    // Basic URI parsing - this is a simplified version
    _ = options;
    _ = allocator;
    
    // Check scheme
    if (!std.mem.startsWith(u8, uri, "postgresql://")) {
        errmsg.* = allocator.dupeZ(u8, "invalid URI scheme") catch null;
        return false;
    }
    
    // TODO: Implement full URI parsing
    errmsg.* = allocator.dupeZ(u8, "URI parsing not fully implemented") catch null;
    return false;
}

fn parseKeyValueString(str: []const u8, options: [*]PQconninfoOption, allocator: std.mem.Allocator, errmsg: *?[*:0]u8) bool {
    _ = str;
    _ = options;
    _ = allocator;
    
    // TODO: Implement key=value parsing
    errmsg.* = allocator.dupeZ(u8, "key=value parsing not implemented") catch null;
    return false;
}

fn connectDBStart(conn: *internal.pg_conn_internal) bool {
    // Get connection parameters
    const host = conn.params.getValue("host") orelse "localhost";
    const port_str = conn.params.getValue("port") orelse "5432";
    const port = std.fmt.parseInt(u16, port_str, 10) catch {
        conn.setErrorMessage("invalid port number: {s}", .{port_str});
        return false;
    };
    
    // Create socket
    const sock = std.os.socket(std.os.AF.INET, std.os.SOCK.STREAM, 0) catch |err| {
        conn.setErrorMessage("socket creation failed: {}", .{err});
        return false;
    };
    conn.sock = sock;
    
    // Set non-blocking if needed
    if (conn.nonblocking) {
        const flags = std.os.fcntl(sock, std.os.F.GETFL, 0) catch |err| {
            conn.setErrorMessage("fcntl F_GETFL failed: {}", .{err});
            std.os.close(sock);
            conn.sock = null;
            return false;
        };
        
        _ = std.os.fcntl(sock, std.os.F.SETFL, flags | std.os.O.NONBLOCK) catch |err| {
            conn.setErrorMessage("fcntl F_SETFL failed: {}", .{err});
            std.os.close(sock);
            conn.sock = null;
            return false;
        };
    }
    
    // Resolve host
    const addr = std.net.Address.resolveIp(host, port) catch |err| {
        conn.setErrorMessage("host resolution failed: {}", .{err});
        std.os.close(sock);
        conn.sock = null;
        return false;
    };
    conn.addr = addr;
    
    // Start connection
    conn.status = .CONNECTION_STARTED;
    
    // Attempt to connect
    std.os.connect(sock, &addr.any, addr.getOsSockLen()) catch |err| {
        switch (err) {
            error.WouldBlock => {
                // This is expected for non-blocking connects
                return true;
            },
            else => {
                conn.setErrorMessage("connect failed: {}", .{err});
                std.os.close(sock);
                conn.sock = null;
                return false;
            },
        }
    };
    
    // If we get here, connection succeeded immediately
    conn.status = .CONNECTION_MADE;
    return true;
}

fn pollConnectionEstablishment(conn: *internal.pg_conn_internal) PostgresPollingStatusType {
    if (conn.sock == null) {
        return .PGRES_POLLING_FAILED;
    }
    
    // Check if the connection completed
    var err: c_int = 0;
    var err_len: std.os.socklen_t = @sizeOf(c_int);
    
    std.os.getsockopt(conn.sock.?, std.os.SOL.SOCKET, std.os.SO.ERROR, std.mem.asBytes(&err)[0..], &err_len) catch |e| {
        conn.setErrorMessage("getsockopt failed: {}", .{e});
        return .PGRES_POLLING_FAILED;
    };
    
    if (err != 0) {
        conn.setErrorMessage("connect failed: {}", .{std.os.errno(err)});
        return .PGRES_POLLING_FAILED;
    }
    
    // Connection established
    conn.status = .CONNECTION_MADE;
    return .PGRES_POLLING_WRITING;
}

fn sendStartupPacket(conn: *internal.pg_conn_internal) bool {
    // Build startup packet
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Protocol version
    const protocol_version = pqcomm.PG_PROTOCOL(3, 0);
    packet.writer().writeIntBig(u32, protocol_version) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Add parameters
    const user = conn.params.getValue("user") orelse {
        conn.setErrorMessage("user name not specified", .{});
        return false;
    };
    
    packet.appendSlice("user") catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.appendSlice(user) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    const database = conn.params.getValue("dbname") orelse user;
    packet.appendSlice("database") catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.appendSlice(database) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Add other parameters as needed
    
    // Null terminator
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Write length
    const len = @as(u32, @intCast(packet.items.len));
    std.mem.writeIntBig(u32, packet.items[0..4], len);
    
    // Send packet
    _ = std.os.send(conn.sock.?, packet.items, 0) catch |err| {
        conn.setErrorMessage("send failed: {}", .{err});
        return false;
    };
    
    return true;
}

fn processServerResponse(conn: *internal.pg_conn_internal) PostgresPollingStatusType {
    // Read server response
    var buffer: [8192]u8 = undefined;
    const n = std.os.recv(conn.sock.?, &buffer, 0) catch |err| {
        switch (err) {
            error.WouldBlock => return .PGRES_POLLING_READING,
            else => {
                conn.setErrorMessage("recv failed: {}", .{err});
                return .PGRES_POLLING_FAILED;
            },
        }
    };
    
    if (n == 0) {
        conn.setErrorMessage("server closed the connection unexpectedly", .{});
        return .PGRES_POLLING_FAILED;
    }
    
    // Add to input buffer
    pqexpbuffer.appendBinaryPQExpBuffer(&conn.in_buffer, buffer[0..n].ptr, n, conn.allocator);
    
    // Process messages
    while (conn.in_cursor < conn.in_buffer.len) {
        // Need at least 5 bytes for message header
        if (conn.in_buffer.len - conn.in_cursor < 5) {
            return .PGRES_POLLING_READING;
        }
        
        const msg_type = conn.in_buffer.data[conn.in_cursor];
        const msg_len = std.mem.readIntBig(u32, conn.in_buffer.data[conn.in_cursor + 1 ..][0..4]);
        
        // Check if we have the complete message
        if (conn.in_buffer.len - conn.in_cursor < msg_len + 1) {
            return .PGRES_POLLING_READING;
        }
        
        // Process message
        switch (msg_type) {
            protocol.PqMsg_AuthenticationRequest => {
                if (!processAuthenticationRequest(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4])) {
                    return .PGRES_POLLING_FAILED;
                }
            },
            protocol.PqMsg_ErrorResponse => {
                processErrorResponse(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
                return .PGRES_POLLING_FAILED;
            },
            protocol.PqMsg_BackendKeyData => {
                processBackendKeyData(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            protocol.PqMsg_ParameterStatus => {
                processParameterStatus(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            protocol.PqMsg_ReadyForQuery => {
                processReadyForQuery(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
                conn.status = .CONNECTION_OK;
                return .PGRES_POLLING_OK;
            },
            else => {
                conn.setErrorMessage("unexpected message type 0x{x}", .{msg_type});
                return .PGRES_POLLING_FAILED;
            },
        }
        
        // Move cursor
        conn.in_cursor += msg_len + 1;
    }
    
    // Reset buffer if consumed
    if (conn.in_cursor >= conn.in_buffer.len) {
        pqexpbuffer.resetPQExpBuffer(&conn.in_buffer, conn.allocator);
        conn.in_cursor = 0;
    }
    
    return .PGRES_POLLING_READING;
}

fn processAuthenticationRequest(conn: *internal.pg_conn_internal, data: []const u8) bool {
    if (data.len < 4) {
        conn.setErrorMessage("invalid authentication request", .{});
        return false;
    }
    
    const auth_type = std.mem.readIntBig(u32, data[0..4]);
    
    switch (auth_type) {
        protocol.AUTH_REQ_OK => {
            // Authentication successful
            conn.auth_state = .AUTH_COMPLETED;
            return true;
        },
        protocol.AUTH_REQ_PASSWORD => {
            // Clear text password required
            return sendPasswordPacket(conn, false);
        },
        protocol.AUTH_REQ_MD5 => {
            // MD5 password required
            if (data.len < 8) {
                conn.setErrorMessage("invalid MD5 authentication request", .{});
                return false;
            }
            return sendMD5Password(conn, data[4..8]);
        },
        protocol.AUTH_REQ_SASL => {
            // SASL authentication
            return processSASLAuth(conn, data[4..]);
        },
        else => {
            conn.setErrorMessage("unsupported authentication method: {}", .{auth_type});
            return false;
        },
    }
}

fn sendPasswordPacket(conn: *internal.pg_conn_internal, encrypted: bool) bool {
    _ = encrypted;
    
    const password = conn.params.getValue("password") orelse {
        conn.setErrorMessage("password required", .{});
        return false;
    };
    
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_PasswordMessage) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Password
    packet.appendSlice(password) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Write length
    const len = @as(u32, @intCast(packet.items.len - 1));
    std.mem.writeIntBig(u32, packet.items[1..5], len);
    
    // Send packet
    _ = std.os.send(conn.sock.?, packet.items, 0) catch |err| {
        conn.setErrorMessage("send failed: {}", .{err});
        return false;
    };
    
    return true;
}

fn sendMD5Password(conn: *internal.pg_conn_internal, salt: []const u8) bool {
    _ = salt;
    
    // TODO: Implement MD5 password authentication
    conn.setErrorMessage("MD5 authentication not implemented", .{});
    return false;
}

fn processSASLAuth(conn: *internal.pg_conn_internal, data: []const u8) bool {
    _ = data;
    
    // TODO: Implement SASL authentication
    conn.setErrorMessage("SASL authentication not implemented", .{});
    return false;
}

fn processErrorResponse(conn: *internal.pg_conn_internal, data: []const u8) void {
    var msg = std.ArrayList(u8).init(conn.allocator);
    defer msg.deinit();
    
    var i: usize = 0;
    while (i < data.len) {
        const field_type = data[i];
        i += 1;
        
        if (field_type == 0) break;
        
        // Find null terminator
        var j = i;
        while (j < data.len and data[j] != 0) : (j += 1) {}
        
        if (j >= data.len) break;
        
        const field_value = data[i..j];
        
        switch (field_type) {
            'M' => { // Primary message
                msg.appendSlice(field_value) catch {};
                msg.append(' ') catch {};
            },
            'D' => { // Detail
                msg.appendSlice("DETAIL: ") catch {};
                msg.appendSlice(field_value) catch {};
                msg.append(' ') catch {};
            },
            'H' => { // Hint
                msg.appendSlice("HINT: ") catch {};
                msg.appendSlice(field_value) catch {};
                msg.append(' ') catch {};
            },
            else => {},
        }
        
        i = j + 1;
    }
    
    conn.setErrorMessage("{s}", .{msg.items});
}

fn processBackendKeyData(conn: *internal.pg_conn_internal, data: []const u8) void {
    if (data.len >= 8) {
        conn.backend_pid = @intCast(std.mem.readIntBig(u32, data[0..4]));
        conn.backend_key = std.mem.readIntBig(u32, data[4..8]);
    }
}

fn processParameterStatus(conn: *internal.pg_conn_internal, data: []const u8) void {
    // Find first null terminator
    var i: usize = 0;
    while (i < data.len and data[i] != 0) : (i += 1) {}
    
    if (i >= data.len) return;
    
    const param_name = data[0..i];
    const value_start = i + 1;
    
    // Find second null terminator
    i = value_start;
    while (i < data.len and data[i] != 0) : (i += 1) {}
    
    if (i > value_start) {
        const param_value = data[value_start..i];
        
        // Store parameter
        const name_copy = conn.allocator.dupe(u8, param_name) catch return;
        const value_copy = conn.allocator.dupe(u8, param_value) catch {
            conn.allocator.free(name_copy);
            return;
        };
        
        // Free old value if exists
        if (conn.server_params.fetchPut(name_copy, value_copy)) |old| {
            conn.allocator.free(old.key);
            conn.allocator.free(old.value);
        }
        
        // Special handling for certain parameters
        if (std.mem.eql(u8, param_name, "client_encoding")) {
            conn.client_encoding = encoding.pg_char_to_encoding(param_value);
        } else if (std.mem.eql(u8, param_name, "standard_conforming_strings")) {
            conn.std_strings = std.mem.eql(u8, param_value, "on");
        } else if (std.mem.eql(u8, param_name, "server_version")) {
            // Parse server version
            parseServerVersion(conn, param_value);
        }
    }
}

fn processReadyForQuery(conn: *internal.pg_conn_internal, data: []const u8) void {
    if (data.len >= 1) {
        switch (data[0]) {
            'I' => conn.trans_status = .PQTRANS_IDLE,
            'T' => conn.trans_status = .PQTRANS_INTRANS,
            'E' => conn.trans_status = .PQTRANS_INERROR,
            else => conn.trans_status = .PQTRANS_UNKNOWN,
        }
    }
}

fn parseServerVersion(conn: *internal.pg_conn_internal, version_str: []const u8) void {
    // Parse version string like "14.2" or "14.2 (Ubuntu 14.2-1.pgdg20.04+1)"
    var major: u32 = 0;
    var minor: u32 = 0;
    
    var i: usize = 0;
    
    // Parse major version
    while (i < version_str.len and std.ascii.isDigit(version_str[i])) : (i += 1) {
        major = major * 10 + (version_str[i] - '0');
    }
    
    // Skip dot
    if (i < version_str.len and version_str[i] == '.') {
        i += 1;
    }
    
    // Parse minor version
    while (i < version_str.len and std.ascii.isDigit(version_str[i])) : (i += 1) {
        minor = minor * 10 + (version_str[i] - '0');
    }
    
    conn.server_version = @intCast(major * 10000 + minor * 100);
}

// Blocking helper for synchronous connections
fn PQconnectPoll(conn: *PGconn) bool {
    while (true) {
        const status = connectPoll(conn);
        switch (status) {
            .PGRES_POLLING_OK => return true,
            .PGRES_POLLING_FAILED => return false,
            .PGRES_POLLING_READING => {
                // Wait for socket to be readable
                const conn_internal = getInternalConn(conn);
                if (conn_internal.sock) |sock| {
                    var fds = [_]std.os.pollfd{.{
                        .fd = sock,
                        .events = std.os.POLL.IN,
                        .revents = 0,
                    }};
                    _ = std.os.poll(&fds, -1) catch {
                        return false;
                    };
                }
            },
            .PGRES_POLLING_WRITING => {
                // Wait for socket to be writable
                const conn_internal = getInternalConn(conn);
                if (conn_internal.sock) |sock| {
                    var fds = [_]std.os.pollfd{.{
                        .fd = sock,
                        .events = std.os.POLL.OUT,
                        .revents = 0,
                    }};
                    _ = std.os.poll(&fds, -1) catch {
                        return false;
                    };
                }
            },
            else => return false,
        }
    }
}