const std = @import("std");
const fe = @import("fe.zig");

// Re-export types from fe.zig that are needed
pub const PGconn = fe.PGconn;
pub const PGresult = fe.PGresult;

// Callback Event Ids
pub const EventId = enum(c_int) {
    PGEVT_REGISTER,
    PGEVT_CONNRESET,
    PGEVT_CONNDESTROY,
    PGEVT_RESULTCREATE,
    PGEVT_RESULTCOPY,
    PGEVT_RESULTDESTROY,
};

pub const EventRegister = extern struct {
    conn: *PGconn,
};

pub const EventConnReset = extern struct {
    conn: *PGconn,
};

pub const EventConnDestroy = extern struct {
    conn: *PGconn,
};

pub const EventResultCreate = extern struct {
    conn: *PGconn,
    result: *PGresult,
};

pub const EventResultCopy = extern struct {
    src: *const PGresult,
    dest: *PGresult,
};

pub const EventResultDestroy = extern struct {
    result: *PGresult,
};

pub const EventProc = *const fn (evtId: EventId, evtInfo: ?*anyopaque, passThrough: ?*anyopaque) callconv(.C) c_int;

// Import implementation
const impl = @import("events-impl.zig");

// Registers an event proc with the given PGconn.
pub const registerEventProc = impl.registerEventProc;

// Sets the PGconn instance data for the provided proc to data.
pub const setInstanceData = impl.setInstanceData;

// Gets the PGconn instance data for the provided proc.
pub const instanceData = impl.instanceData;

// Sets the PGresult instance data for the provided proc to data.
pub const resultSetInstanceData = impl.resultSetInstanceData;

// Gets the PGresult instance data for the provided proc.
pub const resultInstanceData = impl.resultInstanceData;

// Fires RESULTCREATE events for an application-created PGresult.
pub const fireResultCreateEvents = impl.fireResultCreateEvents;