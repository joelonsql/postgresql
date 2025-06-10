const std = @import("std");
const builtin = @import("builtin");

// Import protocol definitions
pub const protocol = @import("protocol.zig");

pub const SockAddr = struct {
    addr: std.net.Address,
};

pub const AddrInfo = struct {
    family: i32,
    addr: SockAddr,
};

// Configure the UNIX socket location for the well known port.
pub fn UNIXSOCK_PATH(buffer: []u8, port: u16, sockdir: []const u8) ![]u8 {
    std.debug.assert(sockdir.len > 0);
    return std.fmt.bufPrint(buffer, "{s}/.s.PGSQL.{d}", .{ sockdir, port });
}

// The maximum workable length of a socket path is what will fit into
// struct sockaddr_un. This is usually only 100 or so bytes :-(.
pub const UNIXSOCK_PATH_BUFLEN = if (builtin.os.tag == .windows) 0 else std.os.linux.sockaddr.un.sun_path.len;

// A host that looks either like an absolute path or starts with @ is
// interpreted as a Unix-domain socket address.
pub fn is_unixsock_path(path: []const u8) bool {
    return is_absolute_path(path) or (path.len > 0 and path[0] == '@');
}

pub fn is_absolute_path(path: []const u8) bool {
    if (path.len == 0) return false;
    
    if (builtin.os.tag == .windows) {
        // Windows absolute paths: C:\ or \\server\share
        if (path.len >= 3 and std.ascii.isAlphabetic(path[0]) and path[1] == ':' and (path[2] == '\\' or path[2] == '/')) {
            return true;
        }
        if (path.len >= 2 and ((path[0] == '\\' and path[1] == '\\') or (path[0] == '/' and path[1] == '/'))) {
            return true;
        }
        return false;
    } else {
        // Unix absolute paths start with /
        return path[0] == '/';
    }
}

// These manipulate the frontend/backend protocol version number.
//
// The major number should be incremented for incompatible changes. The minor
// number should be incremented for compatible changes (eg. additional
// functionality).
//
// If a backend supports version m.n of the protocol it must actually support
// versions m.[0..n]. Backend support for version m-1 can be dropped after a
// `reasonable' length of time.
//
// A frontend isn't required to support anything other than the current
// version.

pub fn PG_PROTOCOL_MAJOR(v: u32) u16 {
    return @intCast(v >> 16);
}

pub fn PG_PROTOCOL_MINOR(v: u32) u16 {
    return @intCast(v & 0x0000ffff);
}

pub fn PG_PROTOCOL_FULL(v: u32) u32 {
    return PG_PROTOCOL_MAJOR(v) * 10000 + PG_PROTOCOL_MINOR(v);
}

pub fn PG_PROTOCOL(m: u16, n: u16) u32 {
    return (@as(u32, m) << 16) | n;
}

// The earliest and latest frontend/backend protocol version supported.
pub const PG_PROTOCOL_EARLIEST = PG_PROTOCOL(3, 0);
pub const PG_PROTOCOL_LATEST = PG_PROTOCOL(3, 2);

pub const ProtocolVersion = u32; // FE/BE protocol version number
pub const MsgType = ProtocolVersion;

// Packet lengths are 4 bytes in network byte order.
// The initial length is omitted from the packet layouts appearing below.
pub const PacketLen = u32;

// In protocol 3.0 and later, the startup packet length is not fixed, but
// we set an arbitrary limit on it anyway. This is just to prevent simple
// denial-of-service attacks via sending enough data to run the server
// out of memory.
pub const MAX_STARTUP_PACKET_LENGTH = 10000;

pub const AuthRequest = u32;

// A client can also send a cancel-current-operation request to the postmaster.
// This is uglier than sending it directly to the client's backend, but it
// avoids depending on out-of-band communication facilities.
//
// The cancel request code must not match any protocol version number
// we're ever likely to use. This random choice should do.
//
// Before PostgreSQL v18 and the protocol version bump from 3.0 to 3.2, the
// cancel key was always 4 bytes. With protocol version 3.2, it's variable
// length.

pub const CANCEL_REQUEST_CODE = PG_PROTOCOL(1234, 5678);

pub const CancelRequestPacket = extern struct {
    // Note that each field is stored in network byte order!
    cancelRequestCode: MsgType, // code to identify a cancel request
    backendPID: u32, // PID of client's backend
    // cancelAuthCode follows as variable length data
};