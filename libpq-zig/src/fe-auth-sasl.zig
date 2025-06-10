const std = @import("std");
const fe = @import("fe.zig");

pub const PGconn = fe.PGconn;

// Possible states for the SASL exchange
pub const SASLStatus = enum(c_int) {
    SASL_COMPLETE = 0,
    SASL_FAILED,
    SASL_CONTINUE,
    SASL_ASYNC,
};

// Frontend SASL mechanism callbacks.
//
// To implement a frontend mechanism, declare a pg_fe_sasl_mech struct with
// appropriate callback implementations, then hook it into conn->sasl during
// pg_SASL_init()'s mechanism negotiation.
pub const pg_fe_sasl_mech = struct {
    // Initializes mechanism-specific state for a connection. This
    // callback must return a pointer to its allocated state, which will
    // be passed as-is as the first argument to the other callbacks.
    // the free() callback is called to release any state resources.
    //
    // If state allocation fails, the implementation should return NULL to
    // fail the authentication exchange.
    //
    // Input parameters:
    //   conn:     The connection to the server
    //   password: The user's supplied password for the current connection
    //   mech:     The mechanism name in use, for implementations that may
    //             advertise more than one name (such as *-PLUS variants).
    init: *const fn (conn: *PGconn, password: [*:0]const u8, mech: [*:0]const u8) callconv(.C) ?*anyopaque,

    // Produces a client response to a server challenge. As a special case
    // for client-first SASL mechanisms, exchange() is called with a NULL
    // server response once at the start of the authentication exchange to
    // generate an initial response. Returns a SASLStatus indicating the
    // state and status of the exchange.
    //
    // Input parameters:
    //   state:     The opaque mechanism state returned by init()
    //   final:     true if the server has sent a final exchange outcome
    //   input:     The challenge data sent by the server, or NULL when
    //              generating a client-first initial response (that is, when
    //              the server expects the client to send a message to start
    //              the exchange). This is guaranteed to be null-terminated
    //              for safety, but SASL allows embedded nulls in challenges,
    //              so mechanisms must be careful to check inputlen.
    //   inputlen:  The length of the challenge data sent by the server, or -1
    //              during client-first initial response generation.
    //
    // Output parameters, to be set by the callback function:
    //   output:    A malloc'd buffer containing the client's response to
    //              the server (can be empty), or NULL if the exchange should
    //              be aborted. (The callback should return SASL_FAILED in the
    //              latter case.)
    //   outputlen: The length (0 or higher) of the client response buffer,
    //              ignored if output is NULL.
    //
    // Return value:
    //   SASL_CONTINUE: The output buffer is filled with a client response.
    //                  Additional server challenge is expected
    //   SASL_ASYNC:    Some asynchronous processing external to the
    //                  connection needs to be done before a response can be
    //                  generated. The mechanism is responsible for setting up
    //                  conn->async_auth/cleanup_async_auth appropriately
    //                  before returning.
    //   SASL_COMPLETE: The SASL exchange has completed successfully.
    //   SASL_FAILED:   The exchange has failed and the connection should be
    //                  dropped.
    exchange: *const fn (state: ?*anyopaque, final: bool, input: ?[*]u8, inputlen: c_int, output: *?[*]u8, outputlen: *c_int) callconv(.C) SASLStatus,

    // Returns true if the connection has an established channel binding. A
    // mechanism implementation must ensure that a SASL exchange has actually
    // been completed, in addition to checking that channel binding is in use.
    //
    // Mechanisms that do not implement channel binding may simply return
    // false.
    //
    // Input parameters:
    //   state:    The opaque mechanism state returned by init()
    channel_bound: *const fn (state: ?*anyopaque) callconv(.C) bool,

    // Frees the state allocated by init(). This is called when the connection
    // is dropped, not when the exchange is completed.
    //
    // Input parameters:
    //   state:    The opaque mechanism state returned by init()
    free: *const fn (state: ?*anyopaque) callconv(.C) void,
};