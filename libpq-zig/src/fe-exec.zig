const std = @import("std");
const fe = @import("fe.zig");
const internal = @import("libpq-int.zig");
const connect = @import("fe-connect.zig");
const pqexpbuffer = @import("pqexpbuffer.zig");
const protocol = @import("protocol.zig");
const events = @import("events.zig");

const PGconn = fe.PGconn;
const PGresult = fe.PGresult;
const ExecStatusType = fe.ExecStatusType;
const Oid = fe.Oid;

// Helper to get internal connection
fn getInternalConn(conn: *PGconn) *internal.pg_conn_internal {
    return @ptrCast(@alignCast(conn));
}

// Helper to get internal result
fn getInternalResult(res: *PGresult) *internal.pg_result_internal {
    return @ptrCast(@alignCast(res));
}

fn getInternalResultConst(res: *const PGresult) *const internal.pg_result_internal {
    return @ptrCast(@alignCast(res));
}

// Simple synchronous query
pub fn exec(conn: *PGconn, query: [*:0]const u8) ?*PGresult {
    if (sendQuery(conn, query) == 0) {
        return null;
    }
    
    return getResultSync(conn);
}

pub fn execParams(conn: *PGconn, command: [*:0]const u8, nParams: c_int, paramTypes: ?[*]const Oid, paramValues: ?[*]const ?[*]const u8, paramLengths: ?[*]const c_int, paramFormats: ?[*]const c_int, resultFormat: c_int) ?*PGresult {
    if (sendQueryParams(conn, command, nParams, paramTypes, paramValues, paramLengths, paramFormats, resultFormat) == 0) {
        return null;
    }
    
    return getResultSync(conn);
}

pub fn prepare(conn: *PGconn, stmtName: [*:0]const u8, query: [*:0]const u8, nParams: c_int, paramTypes: ?[*]const Oid) ?*PGresult {
    if (sendPrepare(conn, stmtName, query, nParams, paramTypes) == 0) {
        return null;
    }
    
    return getResultSync(conn);
}

pub fn execPrepared(conn: *PGconn, stmtName: [*:0]const u8, nParams: c_int, paramValues: ?[*]const ?[*]const u8, paramLengths: ?[*]const c_int, paramFormats: ?[*]const c_int, resultFormat: c_int) ?*PGresult {
    if (sendQueryPrepared(conn, stmtName, nParams, paramValues, paramLengths, paramFormats, resultFormat) == 0) {
        return null;
    }
    
    return getResultSync(conn);
}

// Interface for multiple-result or asynchronous queries
pub fn sendQuery(conn: *PGconn, query: [*:0]const u8) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Check connection state
    if (conn_internal.status != .CONNECTION_OK) {
        conn_internal.setErrorMessage("connection not ready for query", .{});
        return 0;
    }
    
    // Clear any pending results
    clearPendingResults(conn_internal);
    
    // Build Query message
    var packet = std.ArrayList(u8).init(conn_internal.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Query) catch {
        conn_internal.setErrorMessage("out of memory", .{});
        return 0;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn_internal.setErrorMessage("out of memory", .{});
        return 0;
    };
    
    // Query string
    const query_span = std.mem.span(query);
    packet.appendSlice(query_span) catch {
        conn_internal.setErrorMessage("out of memory", .{});
        return 0;
    };
    packet.append(0) catch {
        conn_internal.setErrorMessage("out of memory", .{});
        return 0;
    };
    
    // Write length
    const len = @as(u32, @intCast(packet.items.len - 1));
    std.mem.writeIntBig(u32, packet.items[1..5], len);
    
    // Send packet
    if (!sendPacket(conn_internal, packet.items)) {
        return 0;
    }
    
    // Update state
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_SIMPLE;
    
    return 1;
}

pub fn sendQueryParams(conn: *PGconn, command: [*:0]const u8, nParams: c_int, paramTypes: ?[*]const Oid, paramValues: ?[*]const ?[*]const u8, paramLengths: ?[*]const c_int, paramFormats: ?[*]const c_int, resultFormat: c_int) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Check connection state
    if (conn_internal.status != .CONNECTION_OK) {
        conn_internal.setErrorMessage("connection not ready for query", .{});
        return 0;
    }
    
    // Check parameter count
    if (nParams < 0 or nParams > fe.PQ_QUERY_PARAM_MAX_LIMIT) {
        conn_internal.setErrorMessage("invalid parameter count", .{});
        return 0;
    }
    
    // Clear any pending results
    clearPendingResults(conn_internal);
    
    // Send Parse message
    if (!sendParseMessage(conn_internal, "", command, nParams, paramTypes)) {
        return 0;
    }
    
    // Send Bind message
    if (!sendBindMessage(conn_internal, "", "", nParams, paramValues, paramLengths, paramFormats, resultFormat)) {
        return 0;
    }
    
    // Send Describe message
    if (!sendDescribeMessage(conn_internal, 'P', "")) {
        return 0;
    }
    
    // Send Execute message
    if (!sendExecuteMessage(conn_internal, "", 0)) {
        return 0;
    }
    
    // Send Sync message
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    // Update state
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_EXTENDED;
    
    return 1;
}

pub fn sendPrepare(conn: *PGconn, stmtName: [*:0]const u8, query: [*:0]const u8, nParams: c_int, paramTypes: ?[*]const Oid) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Check connection state
    if (conn_internal.status != .CONNECTION_OK) {
        conn_internal.setErrorMessage("connection not ready for query", .{});
        return 0;
    }
    
    // Clear any pending results
    clearPendingResults(conn_internal);
    
    // Send Parse message
    if (!sendParseMessage(conn_internal, stmtName, query, nParams, paramTypes)) {
        return 0;
    }
    
    // Send Sync message
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    // Update state
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_PREPARE;
    
    return 1;
}

pub fn sendQueryPrepared(conn: *PGconn, stmtName: [*:0]const u8, nParams: c_int, paramValues: ?[*]const ?[*]const u8, paramLengths: ?[*]const c_int, paramFormats: ?[*]const c_int, resultFormat: c_int) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Check connection state
    if (conn_internal.status != .CONNECTION_OK) {
        conn_internal.setErrorMessage("connection not ready for query", .{});
        return 0;
    }
    
    // Clear any pending results
    clearPendingResults(conn_internal);
    
    // Send Bind message
    if (!sendBindMessage(conn_internal, "", stmtName, nParams, paramValues, paramLengths, paramFormats, resultFormat)) {
        return 0;
    }
    
    // Send Execute message
    if (!sendExecuteMessage(conn_internal, "", 0)) {
        return 0;
    }
    
    // Send Sync message
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    // Update state
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_EXTENDED;
    
    return 1;
}

pub fn setSingleRowMode(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Can only be called after sending query
    if (conn_internal.async_status != .PGASYNC_BUSY) {
        return 0;
    }
    
    // TODO: Implement single row mode
    return 0;
}

pub fn setChunkedRowsMode(conn: *PGconn, chunkSize: c_int) c_int {
    _ = chunkSize;
    const conn_internal = getInternalConn(conn);
    
    // Can only be called after sending query
    if (conn_internal.async_status != .PGASYNC_BUSY) {
        return 0;
    }
    
    // TODO: Implement chunked rows mode
    return 0;
}

pub fn getResult(conn: *PGconn) ?*PGresult {
    const conn_internal = getInternalConn(conn);
    
    // Check if we have a result ready
    if (conn_internal.result) |res| {
        conn_internal.result = conn_internal.next_result;
        conn_internal.next_result = null;
        return @ptrCast(res);
    }
    
    // If nothing ready and not busy, we're done
    if (conn_internal.async_status != .PGASYNC_BUSY) {
        return null;
    }
    
    // Try to read more data
    if (consumeInput(conn) == 0) {
        return null;
    }
    
    // Process messages
    while (processMessages(conn_internal)) {
        if (conn_internal.result != null) {
            break;
        }
    }
    
    // Return result if we have one
    if (conn_internal.result) |res| {
        conn_internal.result = conn_internal.next_result;
        conn_internal.next_result = null;
        return @ptrCast(res);
    }
    
    return null;
}

// Routines for managing an asynchronous query
pub fn isBusy(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    return if (conn_internal.async_status == .PGASYNC_BUSY) 1 else 0;
}

pub fn consumeInput(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.sock == null) {
        return 0;
    }
    
    // Read available data
    var buffer: [8192]u8 = undefined;
    const n = std.os.recv(conn_internal.sock.?, &buffer, std.os.MSG.DONTWAIT) catch |err| {
        switch (err) {
            error.WouldBlock => return 1, // No data available, but that's OK
            else => {
                conn_internal.setErrorMessage("recv failed: {}", .{err});
                return 0;
            },
        }
    };
    
    if (n == 0) {
        conn_internal.setErrorMessage("server closed the connection unexpectedly", .{});
        conn_internal.status = .CONNECTION_BAD;
        return 0;
    }
    
    // Add to input buffer
    pqexpbuffer.appendBinaryPQExpBuffer(&conn_internal.in_buffer, buffer[0..n].ptr, n, conn_internal.allocator);
    
    return 1;
}

// Helper functions

fn clearPendingResults(conn: *internal.pg_conn_internal) void {
    if (conn.result) |res| {
        clearResult(res, conn.allocator);
        conn.result = null;
    }
    if (conn.next_result) |res| {
        clearResult(res, conn.allocator);
        conn.next_result = null;
    }
}

fn sendPacket(conn: *internal.pg_conn_internal, data: []const u8) bool {
    if (conn.sock == null) {
        conn.setErrorMessage("no connection to server", .{});
        return false;
    }
    
    // For now, do a blocking send
    _ = std.os.send(conn.sock.?, data, 0) catch |err| {
        conn.setErrorMessage("send failed: {}", .{err});
        conn.status = .CONNECTION_BAD;
        return false;
    };
    
    return true;
}

fn sendParseMessage(conn: *internal.pg_conn_internal, stmtName: [*:0]const u8, query: [*:0]const u8, nParams: c_int, paramTypes: ?[*]const Oid) bool {
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Parse) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Statement name
    packet.appendSlice(std.mem.span(stmtName)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Query string
    packet.appendSlice(std.mem.span(query)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Number of parameter types
    packet.writer().writeIntBig(i16, @intCast(nParams)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Parameter types
    if (nParams > 0 and paramTypes != null) {
        var i: usize = 0;
        while (i < nParams) : (i += 1) {
            packet.writer().writeIntBig(u32, paramTypes.?[i]) catch {
                conn.setErrorMessage("out of memory", .{});
                return false;
            };
        }
    } else {
        // Send zeros for unspecified types
        var i: usize = 0;
        while (i < nParams) : (i += 1) {
            packet.writer().writeIntBig(u32, 0) catch {
                conn.setErrorMessage("out of memory", .{});
                return false;
            };
        }
    }
    
    // Write length
    const len = @as(u32, @intCast(packet.items.len - 1));
    std.mem.writeIntBig(u32, packet.items[1..5], len);
    
    return sendPacket(conn, packet.items);
}

fn sendBindMessage(conn: *internal.pg_conn_internal, portalName: [*:0]const u8, stmtName: [*:0]const u8, nParams: c_int, paramValues: ?[*]const ?[*]const u8, paramLengths: ?[*]const c_int, paramFormats: ?[*]const c_int, resultFormat: c_int) bool {
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Bind) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Portal name
    packet.appendSlice(std.mem.span(portalName)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Statement name
    packet.appendSlice(std.mem.span(stmtName)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Number of parameter format codes
    if (paramFormats != null) {
        packet.writer().writeIntBig(i16, @intCast(nParams)) catch {
            conn.setErrorMessage("out of memory", .{});
            return false;
        };
        var i: usize = 0;
        while (i < nParams) : (i += 1) {
            packet.writer().writeIntBig(i16, @intCast(paramFormats.?[i])) catch {
                conn.setErrorMessage("out of memory", .{});
                return false;
            };
        }
    } else {
        packet.writer().writeIntBig(i16, 0) catch {
            conn.setErrorMessage("out of memory", .{});
            return false;
        };
    }
    
    // Number of parameter values
    packet.writer().writeIntBig(i16, @intCast(nParams)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Parameter values
    var i: usize = 0;
    while (i < nParams) : (i += 1) {
        if (paramValues != null and paramValues.?[i] != null) {
            const len = if (paramLengths != null) paramLengths.?[i] else @intCast(std.mem.len(paramValues.?[i].?));
            packet.writer().writeIntBig(i32, @intCast(len)) catch {
                conn.setErrorMessage("out of memory", .{});
                return false;
            };
            packet.appendSlice(paramValues.?[i].?[0..@intCast(len)]) catch {
                conn.setErrorMessage("out of memory", .{});
                return false;
            };
        } else {
            // NULL parameter
            packet.writer().writeIntBig(i32, -1) catch {
                conn.setErrorMessage("out of memory", .{});
                return false;
            };
        }
    }
    
    // Result format code
    packet.writer().writeIntBig(i16, 1) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.writer().writeIntBig(i16, @intCast(resultFormat)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Write length
    const len = @as(u32, @intCast(packet.items.len - 1));
    std.mem.writeIntBig(u32, packet.items[1..5], len);
    
    return sendPacket(conn, packet.items);
}

fn sendDescribeMessage(conn: *internal.pg_conn_internal, what: u8, name: [*:0]const u8) bool {
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Describe) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // What to describe ('S' for statement, 'P' for portal)
    packet.append(what) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Name
    packet.appendSlice(std.mem.span(name)) catch {
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
    
    return sendPacket(conn, packet.items);
}

fn sendExecuteMessage(conn: *internal.pg_conn_internal, portalName: [*:0]const u8, maxRows: u32) bool {
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Execute) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Portal name
    packet.appendSlice(std.mem.span(portalName)) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    packet.append(0) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Maximum rows to return (0 = no limit)
    packet.writer().writeIntBig(u32, maxRows) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Write length
    const len = @as(u32, @intCast(packet.items.len - 1));
    std.mem.writeIntBig(u32, packet.items[1..5], len);
    
    return sendPacket(conn, packet.items);
}

fn sendSyncMessage(conn: *internal.pg_conn_internal) bool {
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Sync) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Length (always 4 for Sync)
    packet.writer().writeIntBig(u32, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    return sendPacket(conn, packet.items);
}

fn processMessages(conn: *internal.pg_conn_internal) bool {
    while (conn.in_cursor < conn.in_buffer.len) {
        // Need at least 5 bytes for message header
        if (conn.in_buffer.len - conn.in_cursor < 5) {
            return false;
        }
        
        const msg_type = conn.in_buffer.data[conn.in_cursor];
        const msg_len = std.mem.readIntBig(u32, conn.in_buffer.data[conn.in_cursor + 1 ..][0..4]);
        
        // Check if we have the complete message
        if (conn.in_buffer.len - conn.in_cursor < msg_len + 1) {
            return false;
        }
        
        // Process message
        switch (msg_type) {
            protocol.PqMsg_ErrorResponse => {
                processErrorResponse(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
                // Create error result
                conn.result = makeErrorResult(conn);
                conn.async_status = .PGASYNC_READY;
            },
            protocol.PqMsg_NoticeResponse => {
                processNoticeResponse(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            protocol.PqMsg_CommandComplete => {
                processCommandComplete(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            protocol.PqMsg_EmptyQueryResponse => {
                // Create empty result
                conn.result = makeEmptyResult(conn, .PGRES_EMPTY_QUERY);
            },
            protocol.PqMsg_RowDescription => {
                processRowDescription(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            protocol.PqMsg_DataRow => {
                processDataRow(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            protocol.PqMsg_ReadyForQuery => {
                processReadyForQuery(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
                conn.async_status = .PGASYNC_IDLE;
            },
            protocol.PqMsg_ParseComplete => {
                // Create result for successful prepare
                if (conn.query_class == .PGQUERY_PREPARE) {
                    conn.result = makeEmptyResult(conn, .PGRES_COMMAND_OK);
                }
            },
            protocol.PqMsg_BindComplete => {
                // Nothing to do
            },
            protocol.PqMsg_ParameterStatus => {
                processParameterStatus(conn, conn.in_buffer.data[conn.in_cursor + 5 ..][0 .. msg_len - 4]);
            },
            else => {
                // Unknown message type, skip it
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
    
    return true;
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
            },
            'D' => { // Detail
                msg.appendSlice("\nDETAIL: ") catch {};
                msg.appendSlice(field_value) catch {};
            },
            'H' => { // Hint
                msg.appendSlice("\nHINT: ") catch {};
                msg.appendSlice(field_value) catch {};
            },
            else => {},
        }
        
        i = j + 1;
    }
    
    conn.setErrorMessage("{s}", .{msg.items});
}

fn processNoticeResponse(conn: *internal.pg_conn_internal, data: []const u8) void {
    // TODO: Implement notice handling
    _ = conn;
    _ = data;
}

fn processCommandComplete(conn: *internal.pg_conn_internal, data: []const u8) void {
    // Find null terminator
    var i: usize = 0;
    while (i < data.len and data[i] != 0) : (i += 1) {}
    
    if (conn.result == null) {
        conn.result = makeEmptyResult(conn, .PGRES_COMMAND_OK);
    }
    
    if (conn.result) |res| {
        // Copy command status
        const status_len = @min(i, internal.CMDSTATUS_LEN - 1);
        @memcpy(res.cmdStatus[0..status_len], data[0..status_len]);
        res.cmdStatus[status_len] = 0;
    }
}

fn processRowDescription(conn: *internal.pg_conn_internal, data: []const u8) void {
    if (data.len < 2) return;
    
    const nfields = std.mem.readIntBig(i16, data[0..2]);
    if (nfields < 0) return;
    
    // Create result if needed
    if (conn.result == null) {
        conn.result = makeEmptyResult(conn, .PGRES_TUPLES_OK);
    }
    
    const res = conn.result orelse return;
    
    // Allocate attribute descriptors
    res.numAttributes = nfields;
    res.attDescs = conn.allocator.alloc(fe.PGresAttDesc, @intCast(nfields)) catch {
        conn.setErrorMessage("out of memory", .{});
        return;
    };
    
    var pos: usize = 2;
    var field: usize = 0;
    while (field < nfields) : (field += 1) {
        // Field name
        var name_end = pos;
        while (name_end < data.len and data[name_end] != 0) : (name_end += 1) {}
        if (name_end >= data.len) break;
        
        res.attDescs.?[field].name = conn.allocator.dupeZ(u8, data[pos..name_end]) catch {
            conn.setErrorMessage("out of memory", .{});
            return;
        };
        pos = name_end + 1;
        
        // Need at least 18 more bytes for the rest of the fields
        if (pos + 18 > data.len) break;
        
        // Table OID
        res.attDescs.?[field].tableid = std.mem.readIntBig(u32, data[pos..][0..4]);
        pos += 4;
        
        // Column number
        res.attDescs.?[field].columnid = std.mem.readIntBig(i16, data[pos..][0..2]);
        pos += 2;
        
        // Type OID
        res.attDescs.?[field].typid = std.mem.readIntBig(u32, data[pos..][0..4]);
        pos += 4;
        
        // Type size
        res.attDescs.?[field].typlen = std.mem.readIntBig(i16, data[pos..][0..2]);
        pos += 2;
        
        // Type modifier
        res.attDescs.?[field].atttypmod = std.mem.readIntBig(i32, data[pos..][0..4]);
        pos += 4;
        
        // Format code
        res.attDescs.?[field].format = std.mem.readIntBig(i16, data[pos..][0..2]);
        pos += 2;
    }
}

fn processDataRow(conn: *internal.pg_conn_internal, data: []const u8) void {
    const res = conn.result orelse return;
    
    if (data.len < 2) return;
    
    const nfields = std.mem.readIntBig(i16, data[0..2]);
    if (nfields != res.numAttributes) return;
    
    // Expand tuple array if needed
    if (res.ntups >= res.tupArrSize) {
        const new_size = if (res.tupArrSize == 0) 128 else res.tupArrSize * 2;
        const new_tuples = conn.allocator.realloc(
            if (res.tuples) |t| t[0..@intCast(res.tupArrSize)] else &[_][*]internal.PGresAttValue{},
            new_size
        ) catch {
            conn.setErrorMessage("out of memory", .{});
            return;
        };
        res.tuples = new_tuples.ptr;
        res.tupArrSize = @intCast(new_size);
    }
    
    // Allocate tuple
    const tuple = conn.allocator.alloc(internal.PGresAttValue, @intCast(nfields)) catch {
        conn.setErrorMessage("out of memory", .{});
        return;
    };
    res.tuples.?[@intCast(res.ntups)] = tuple.ptr;
    
    var pos: usize = 2;
    var field: usize = 0;
    while (field < nfields) : (field += 1) {
        if (pos + 4 > data.len) break;
        
        const len = std.mem.readIntBig(i32, data[pos..][0..4]);
        pos += 4;
        
        if (len < 0) {
            // NULL value
            tuple[field].len = internal.NULL_LEN;
            tuple[field].value = @ptrCast(&res.null_field);
        } else {
            const ulen: usize = @intCast(len);
            if (pos + ulen > data.len) break;
            
            // Allocate space for value plus null terminator
            const value = conn.allocator.alloc(u8, ulen + 1) catch {
                conn.setErrorMessage("out of memory", .{});
                return;
            };
            @memcpy(value[0..ulen], data[pos..pos + ulen]);
            value[ulen] = 0;
            
            tuple[field].len = len;
            tuple[field].value = value.ptr;
            
            pos += ulen;
        }
    }
    
    res.ntups += 1;
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
    }
}

fn makeEmptyResult(conn: *internal.pg_conn_internal, status: ExecStatusType) ?*internal.pg_result_internal {
    const res = conn.allocator.create(internal.pg_result_internal) catch {
        conn.setErrorMessage("out of memory", .{});
        return null;
    };
    
    res.* = .{
        .ntups = 0,
        .numAttributes = 0,
        .attDescs = null,
        .tuples = null,
        .tupArrSize = 0,
        .numParameters = 0,
        .paramDescs = null,
        .resultStatus = status,
        .cmdStatus = std.mem.zeroes([internal.CMDSTATUS_LEN]u8),
        .binary = 0,
        .noticeHooks = conn.notice_hooks,
        .events = null,
        .nEvents = 0,
        .client_encoding = conn.client_encoding,
        .errMsg = null,
        .errFields = null,
        .errQuery = null,
        .null_field = [_]u8{0},
        .curBlock = null,
        .curOffset = 0,
        .spaceLeft = 0,
        .memorySize = @sizeOf(internal.pg_result_internal),
    };
    
    // Fire result create events
    _ = events.fireResultCreateEvents(@ptrCast(conn), @ptrCast(res));
    
    return res;
}

fn makeErrorResult(conn: *internal.pg_conn_internal) ?*internal.pg_result_internal {
    const res = makeEmptyResult(conn, .PGRES_FATAL_ERROR) orelse return null;
    
    // Copy error message
    if (conn.error_message.len > 0) {
        res.errMsg = conn.allocator.dupeZ(u8, conn.error_message.data[0..conn.error_message.len]) catch {
            return res;
        };
    }
    
    return res;
}

fn clearResult(res: *internal.pg_result_internal, allocator: std.mem.Allocator) void {
    // This is implemented in libpq-int.zig
    internal.clearResult(res, allocator);
}

fn getResultSync(conn: *PGconn) ?*PGresult {
    const conn_internal = getInternalConn(conn);
    
    // In blocking mode, wait for complete result
    while (true) {
        const res = getResult(conn);
        if (res != null) {
            return res;
        }
        
        if (conn_internal.async_status == .PGASYNC_IDLE) {
            break;
        }
        
        // Wait for socket to be readable
        if (conn_internal.sock) |sock| {
            var fds = [_]std.os.pollfd{.{
                .fd = sock,
                .events = std.os.POLL.IN,
                .revents = 0,
            }};
            _ = std.os.poll(&fds, -1) catch {
                return null;
            };
        }
    }
    
    return null;
}

// Additional query execution functions needed by fe-impl.zig

pub fn describePrepared(conn: *PGconn, stmt: [*:0]const u8) ?*PGresult {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return null;
    }
    
    clearPendingResults(conn_internal);
    
    if (!sendDescribeMessage(conn_internal, 'S', stmt)) {
        return null;
    }
    
    if (!sendSyncMessage(conn_internal)) {
        return null;
    }
    
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_DESCRIBE;
    
    return getResultSync(conn);
}

pub fn describePortal(conn: *PGconn, portal: [*:0]const u8) ?*PGresult {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return null;
    }
    
    clearPendingResults(conn_internal);
    
    if (!sendDescribeMessage(conn_internal, 'P', portal)) {
        return null;
    }
    
    if (!sendSyncMessage(conn_internal)) {
        return null;
    }
    
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_DESCRIBE;
    
    return getResultSync(conn);
}

pub fn sendDescribePrepared(conn: *PGconn, stmt: [*:0]const u8) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return 0;
    }
    
    clearPendingResults(conn_internal);
    
    if (!sendDescribeMessage(conn_internal, 'S', stmt)) {
        return 0;
    }
    
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_DESCRIBE;
    
    return 1;
}

pub fn sendDescribePortal(conn: *PGconn, portal: [*:0]const u8) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return 0;
    }
    
    clearPendingResults(conn_internal);
    
    if (!sendDescribeMessage(conn_internal, 'P', portal)) {
        return 0;
    }
    
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_DESCRIBE;
    
    return 1;
}

pub fn closePrepared(conn: *PGconn, stmt: [*:0]const u8) ?*PGresult {
    if (sendClosePrepared(conn, stmt) == 0) {
        return null;
    }
    
    return getResultSync(conn);
}

pub fn closePortal(conn: *PGconn, portal: [*:0]const u8) ?*PGresult {
    if (sendClosePortal(conn, portal) == 0) {
        return null;
    }
    
    return getResultSync(conn);
}

pub fn sendClosePrepared(conn: *PGconn, stmt: [*:0]const u8) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return 0;
    }
    
    clearPendingResults(conn_internal);
    
    if (!sendCloseMessage(conn_internal, 'S', stmt)) {
        return 0;
    }
    
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_CLOSE;
    
    return 1;
}

pub fn sendClosePortal(conn: *PGconn, portal: [*:0]const u8) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return 0;
    }
    
    clearPendingResults(conn_internal);
    
    if (!sendCloseMessage(conn_internal, 'P', portal)) {
        return 0;
    }
    
    if (!sendSyncMessage(conn_internal)) {
        return 0;
    }
    
    conn_internal.async_status = .PGASYNC_BUSY;
    conn_internal.query_class = .PGQUERY_CLOSE;
    
    return 1;
}

fn sendCloseMessage(conn: *internal.pg_conn_internal, what: u8, name: [*:0]const u8) bool {
    var packet = std.ArrayList(u8).init(conn.allocator);
    defer packet.deinit();
    
    // Message type
    packet.append(protocol.PqMsg_Close) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Reserve space for length
    packet.appendNTimes(0, 4) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // What to close ('S' for statement, 'P' for portal)
    packet.append(what) catch {
        conn.setErrorMessage("out of memory", .{});
        return false;
    };
    
    // Name
    packet.appendSlice(std.mem.span(name)) catch {
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
    
    return sendPacket(conn, packet.items);
}

// Pipeline mode functions
pub fn enterPipelineMode(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.status != .CONNECTION_OK) {
        return 0;
    }
    
    if (conn_internal.pipeline_status != .PQ_PIPELINE_OFF) {
        return 0;
    }
    
    if (conn_internal.async_status != .PGASYNC_IDLE) {
        return 0;
    }
    
    conn_internal.pipeline_status = .PQ_PIPELINE_ON;
    return 1;
}

pub fn exitPipelineMode(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.pipeline_status == .PQ_PIPELINE_OFF) {
        return 1;
    }
    
    if (conn_internal.async_status != .PGASYNC_IDLE) {
        return 0;
    }
    
    conn_internal.pipeline_status = .PQ_PIPELINE_OFF;
    return 1;
}

pub fn pipelineSync(conn: *PGconn) c_int {
    return sendPipelineSync(conn);
}

pub fn sendFlushRequest(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    var packet = [_]u8{ protocol.PqMsg_Flush, 0, 0, 0, 4 };
    
    return if (sendPacket(conn_internal, &packet)) 1 else 0;
}

pub fn sendPipelineSync(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.pipeline_status == .PQ_PIPELINE_OFF) {
        return 0;
    }
    
    return if (sendSyncMessage(conn_internal)) 1 else 0;
}

// LISTEN/NOTIFY support
pub fn notifies(conn: *PGconn) ?*fe.PGnotify {
    _ = conn;
    // TODO: Implement NOTIFY support
    return null;
}

// COPY support
pub fn putCopyData(conn: *PGconn, buffer: [*]const u8, nbytes: c_int) c_int {
    _ = conn;
    _ = buffer;
    _ = nbytes;
    // TODO: Implement COPY support
    return -1;
}

pub fn putCopyEnd(conn: *PGconn, errormsg: ?[*:0]const u8) c_int {
    _ = conn;
    _ = errormsg;
    // TODO: Implement COPY support
    return -1;
}

pub fn getCopyData(conn: *PGconn, buffer: *[*]u8, async: c_int) c_int {
    _ = conn;
    _ = buffer;
    _ = async;
    // TODO: Implement COPY support
    return -1;
}

// Deprecated COPY functions
pub fn getline(conn: *PGconn, buffer: [*]u8, length: c_int) c_int {
    _ = conn;
    _ = buffer;
    _ = length;
    return -1;
}

pub fn putline(conn: *PGconn, string: [*:0]const u8) c_int {
    _ = conn;
    _ = string;
    return -1;
}

pub fn getlineAsync(conn: *PGconn, buffer: [*]u8, bufsize: c_int) c_int {
    _ = conn;
    _ = buffer;
    _ = bufsize;
    return -1;
}

pub fn putnbytes(conn: *PGconn, buffer: [*]const u8, nbytes: c_int) c_int {
    _ = conn;
    _ = buffer;
    _ = nbytes;
    return -1;
}

pub fn endcopy(conn: *PGconn) c_int {
    _ = conn;
    return -1;
}

// Non-blocking support
pub fn setnonblocking(conn: *PGconn, arg: c_int) c_int {
    const conn_internal = getInternalConn(conn);
    conn_internal.nonblocking = arg != 0;
    return 0;
}

pub fn isnonblocking(conn: *const PGconn) c_int {
    const conn_internal = @as(*const internal.pg_conn_internal, @ptrCast(@alignCast(conn)));
    return if (conn_internal.nonblocking) 1 else 0;
}

pub fn isthreadsafe() c_int {
    return 1; // Zig is thread-safe
}

pub fn ping(conninfo: [*:0]const u8) fe.PGPing {
    const conn = connect.connectStart(conninfo) orelse return .PQPING_NO_ATTEMPT;
    defer connect.finish(conn);
    
    // Try to connect
    while (true) {
        const status = connect.connectPoll(conn);
        switch (status) {
            .PGRES_POLLING_OK => return .PQPING_OK,
            .PGRES_POLLING_FAILED => {
                const conn_internal = getInternalConn(conn);
                // Check if server rejected connection
                if (conn_internal.status == .CONNECTION_BAD) {
                    // TODO: Better detection of reject vs no response
                    return .PQPING_NO_RESPONSE;
                }
                return .PQPING_NO_RESPONSE;
            },
            .PGRES_POLLING_READING, .PGRES_POLLING_WRITING => {
                // Would block, but we don't want to wait
                return .PQPING_NO_RESPONSE;
            },
            else => return .PQPING_NO_RESPONSE,
        }
    }
}

pub fn pingParams(keywords: [*]const [*:0]const u8, values: [*]const [*:0]const u8, expand_dbname: c_int) fe.PGPing {
    const conn = connect.connectStartParams(keywords, values, expand_dbname) orelse return .PQPING_NO_ATTEMPT;
    defer connect.finish(conn);
    
    // Try to connect
    while (true) {
        const status = connect.connectPoll(conn);
        switch (status) {
            .PGRES_POLLING_OK => return .PQPING_OK,
            .PGRES_POLLING_FAILED => return .PQPING_NO_RESPONSE,
            .PGRES_POLLING_READING, .PGRES_POLLING_WRITING => {
                // Would block
                return .PQPING_NO_RESPONSE;
            },
            else => return .PQPING_NO_RESPONSE,
        }
    }
}

pub fn flush(conn: *PGconn) c_int {
    const conn_internal = getInternalConn(conn);
    
    if (conn_internal.out_buffer.len == 0) {
        return 0; // Nothing to flush
    }
    
    // TODO: Implement output buffer flushing
    return 0;
}

// Fast path interface
pub fn fn_(conn: *PGconn, fnid: c_int, result_buf: *c_int, result_len: *c_int, result_is_int: c_int, args: *const fe.PQArgBlock, nargs: c_int) ?*PGresult {
    _ = conn;
    _ = fnid;
    _ = result_buf;
    _ = result_len;
    _ = result_is_int;
    _ = args;
    _ = nargs;
    // Fast path interface not implemented
    return null;
}