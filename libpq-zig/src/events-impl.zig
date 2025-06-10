const std = @import("std");
const events = @import("events.zig");
const fe = @import("fe.zig");
const internal = @import("libpq-int.zig");

const PGconn = events.PGconn;
const PGresult = events.PGresult;
const EventProc = events.EventProc;

// Helper to get internal connection
fn getInternalConn(conn: *PGconn) *internal.pg_conn_internal {
    return @ptrCast(@alignCast(conn));
}

fn getInternalConnConst(conn: *const PGconn) *const internal.pg_conn_internal {
    return @ptrCast(@alignCast(conn));
}

// Helper to get internal result
fn getInternalResult(res: *PGresult) *internal.pg_result_internal {
    return @ptrCast(@alignCast(res));
}

fn getInternalResultConst(res: *const PGresult) *const internal.pg_result_internal {
    return @ptrCast(@alignCast(res));
}

// Registers an event proc with the given PGconn.
pub fn registerEventProc(conn: *PGconn, proc: EventProc, name: [*:0]const u8, passThrough: ?*anyopaque) c_int {
    const conn_internal = getInternalConn(conn);
    
    // Check if already registered
    for (conn_internal.events.items) |event| {
        if (event.proc == proc) {
            return 0; // Already registered
        }
    }
    
    // Create new event
    const event = internal.PGEvent{
        .proc = proc,
        .name = conn_internal.allocator.dupe(u8, std.mem.span(name)) catch {
            return 0;
        },
        .passThrough = passThrough,
        .data = null,
        .resultInitialized = false,
    };
    
    // Add to events list
    conn_internal.events.append(event) catch {
        conn_internal.allocator.free(event.name);
        return 0;
    };
    
    // Fire PGEVT_REGISTER event
    const reg_data = events.EventRegister{ .conn = conn };
    if (proc(.PGEVT_REGISTER, &reg_data, passThrough) == 0) {
        // Registration failed, remove from list
        _ = conn_internal.events.pop();
        conn_internal.allocator.free(event.name);
        return 0;
    }
    
    return 1;
}

// Sets the PGconn instance data for the provided proc to data.
pub fn setInstanceData(conn: *PGconn, proc: EventProc, data: ?*anyopaque) c_int {
    const conn_internal = getInternalConn(conn);
    
    for (conn_internal.events.items) |*event| {
        if (event.proc == proc) {
            event.data = data;
            return 1;
        }
    }
    
    return 0; // Event proc not found
}

// Gets the PGconn instance data for the provided proc.
pub fn instanceData(conn: *const PGconn, proc: EventProc) ?*anyopaque {
    const conn_internal = getInternalConnConst(conn);
    
    for (conn_internal.events.items) |event| {
        if (event.proc == proc) {
            return event.data;
        }
    }
    
    return null;
}

// Sets the PGresult instance data for the provided proc to data.
pub fn resultSetInstanceData(result: *PGresult, proc: EventProc, data: ?*anyopaque) c_int {
    const res_internal = getInternalResult(result);
    
    if (res_internal.events) |events_list| {
        var i: usize = 0;
        while (i < res_internal.nEvents) : (i += 1) {
            if (events_list[i].proc == proc) {
                events_list[i].data = data;
                return 1;
            }
        }
    }
    
    return 0; // Event proc not found
}

// Gets the PGresult instance data for the provided proc.
pub fn resultInstanceData(result: *const PGresult, proc: EventProc) ?*anyopaque {
    const res_internal = getInternalResultConst(result);
    
    if (res_internal.events) |events_list| {
        var i: usize = 0;
        while (i < res_internal.nEvents) : (i += 1) {
            if (events_list[i].proc == proc) {
                return events_list[i].data;
            }
        }
    }
    
    return null;
}

// Fires RESULTCREATE events for an application-created PGresult.
pub fn fireResultCreateEvents(conn: *PGconn, res: *PGresult) c_int {
    const conn_internal = getInternalConn(conn);
    const res_internal = getInternalResult(res);
    
    // Copy events from connection to result
    if (conn_internal.events.items.len > 0) {
        const allocator = conn_internal.allocator;
        res_internal.events = allocator.alloc(internal.PGEvent, conn_internal.events.items.len) catch {
            return 0;
        };
        res_internal.nEvents = @intCast(conn_internal.events.items.len);
        
        // Copy events and fire RESULTCREATE
        for (conn_internal.events.items, 0..) |conn_event, i| {
            res_internal.events.?[i] = .{
                .proc = conn_event.proc,
                .name = allocator.dupe(u8, conn_event.name) catch {
                    // Clean up on failure
                    for (0..i) |j| {
                        allocator.free(res_internal.events.?[j].name);
                    }
                    allocator.free(res_internal.events.?[0..conn_internal.events.items.len]);
                    res_internal.events = null;
                    res_internal.nEvents = 0;
                    return 0;
                },
                .passThrough = conn_event.passThrough,
                .data = null,
                .resultInitialized = false,
            };
            
            // Fire RESULTCREATE event
            const create_data = events.EventResultCreate{ .conn = conn, .result = res };
            if (conn_event.proc(.PGEVT_RESULTCREATE, &create_data, conn_event.passThrough) != 0) {
                res_internal.events.?[i].resultInitialized = true;
            }
        }
    }
    
    return 1;
}