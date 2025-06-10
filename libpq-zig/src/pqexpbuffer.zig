const std = @import("std");

// Initial size of the data buffer in a PQExpBuffer.
// NB: this must be large enough to hold error messages that might
// be returned by PQrequestCancel().
pub const INITIAL_EXPBUFFER_SIZE = 256;

// PQExpBufferData holds information about an extensible string.
//   data    is the current buffer for the string (allocated with malloc).
//   len     is the current string length. There is guaranteed to be
//           a terminating '\0' at data[len], although this is not very
//           useful when the string holds binary data rather than text.
//   maxlen  is the allocated size in bytes of 'data', i.e. the maximum
//           string size (including the terminating '\0' char) that we can
//           currently store in 'data' without having to reallocate
//           more space. We must always have maxlen > len.
//
// An exception occurs if we failed to allocate enough memory for the string
// buffer. In that case data points to a statically allocated empty string,
// and len = maxlen = 0.
pub const PQExpBufferData = struct {
    data: [*]u8,
    len: usize,
    maxlen: usize,
};

pub const PQExpBuffer = *PQExpBufferData;

// Static empty string to use when out of memory
var oom_buffer = [_]u8{0};

// Test for a broken (out of memory) PQExpBuffer.
// When a buffer is "broken", all operations except resetting or deleting it
// are no-ops.
pub fn PQExpBufferBroken(str: ?*PQExpBufferData) bool {
    return str == null or str.?.maxlen == 0;
}

// Same, but for use when using a static or local PQExpBufferData struct.
// For that, a null-pointer test is useless and may draw compiler warnings.
pub fn PQExpBufferDataBroken(buf: PQExpBufferData) bool {
    return buf.maxlen == 0;
}

// Create an empty 'PQExpBufferData' & return a pointer to it.
pub fn createPQExpBuffer(allocator: std.mem.Allocator) !*PQExpBufferData {
    const str = try allocator.create(PQExpBufferData);
    errdefer allocator.destroy(str);
    
    initPQExpBuffer(str, allocator);
    if (PQExpBufferBroken(str)) {
        allocator.destroy(str);
        return error.OutOfMemory;
    }
    
    return str;
}

// Initialize a PQExpBufferData struct (with previously undefined contents)
// to describe an empty string.
pub fn initPQExpBuffer(str: *PQExpBufferData, allocator: std.mem.Allocator) void {
    str.data = allocator.alloc(u8, INITIAL_EXPBUFFER_SIZE) catch {
        str.data = &oom_buffer;
        str.maxlen = 0;
        str.len = 0;
        return;
    };
    
    str.maxlen = INITIAL_EXPBUFFER_SIZE;
    str.len = 0;
    str.data[0] = 0;
}

// Free both the data buffer and the PQExpBufferData.
// This is the inverse of createPQExpBuffer().
pub fn destroyPQExpBuffer(str: *PQExpBufferData, allocator: std.mem.Allocator) void {
    if (str.maxlen > 0) {
        allocator.free(str.data[0..str.maxlen]);
    }
    allocator.destroy(str);
}

// Free the data buffer but not the PQExpBufferData itself.
// This is the inverse of initPQExpBuffer().
pub fn termPQExpBuffer(str: *PQExpBufferData, allocator: std.mem.Allocator) void {
    if (str.maxlen > 0) {
        allocator.free(str.data[0..str.maxlen]);
    }
    // Just for safety, make the buffer look empty and broken
    str.data = &oom_buffer;
    str.maxlen = 0;
    str.len = 0;
}

// Reset a PQExpBuffer to empty
// Note: if possible, a "broken" PQExpBuffer is returned to normal.
pub fn resetPQExpBuffer(str: *PQExpBufferData, allocator: std.mem.Allocator) void {
    if (str.maxlen > 0) {
        str.len = 0;
        str.data[0] = 0;
    } else {
        // Try to reinitialize to fix a broken buffer
        initPQExpBuffer(str, allocator);
    }
}

// Make sure there is enough space for 'needed' more bytes in the buffer
// ('needed' does not include the terminating null).
//
// Returns true if OK, false if failed to enlarge buffer. (In the latter case
// the buffer is left in "broken" state.)
pub fn enlargePQExpBuffer(str: *PQExpBufferData, needed: usize, allocator: std.mem.Allocator) bool {
    // Check for overflow
    if (needed >= std.math.maxInt(usize) - str.len - 1) {
        markPQExpBufferBroken(str, allocator);
        return false;
    }
    
    const needed_size = str.len + needed + 1;
    
    // If we already have enough space, we're done
    if (needed_size <= str.maxlen) {
        return true;
    }
    
    // We don't have enough space. If we're not at the end of the current
    // block, enlarge the buffer, keeping the data at the same position.
    var new_size = str.maxlen;
    
    // Double size until we have enough
    while (new_size < needed_size) {
        new_size = new_size * 2;
        
        // Check for overflow
        if (new_size < str.maxlen) {
            markPQExpBufferBroken(str, allocator);
            return false;
        }
    }
    
    // Clamp to a reasonable maximum
    const limit = std.math.maxInt(usize) / 2;
    if (new_size > limit) {
        new_size = limit;
        if (new_size < needed_size) {
            markPQExpBufferBroken(str, allocator);
            return false;
        }
    }
    
    // Realloc
    const new_data = allocator.realloc(str.data[0..str.maxlen], new_size) catch {
        markPQExpBufferBroken(str, allocator);
        return false;
    };
    
    str.data = new_data.ptr;
    str.maxlen = new_size;
    return true;
}

// Mark a PQExpBuffer as broken
fn markPQExpBufferBroken(str: *PQExpBufferData, allocator: std.mem.Allocator) void {
    if (str.maxlen > 0) {
        allocator.free(str.data[0..str.maxlen]);
    }
    str.data = &oom_buffer;
    str.maxlen = 0;
    str.len = 0;
}

// Format text data under the control of fmt (an sprintf-like format string)
// and insert it into str. More space is allocated to str if necessary.
// This is a convenience routine that does the same thing as
// resetPQExpBuffer() followed by appendPQExpBuffer().
pub fn printfPQExpBuffer(str: *PQExpBufferData, allocator: std.mem.Allocator, comptime fmt: []const u8, args: anytype) void {
    resetPQExpBuffer(str, allocator);
    appendPQExpBuffer(str, allocator, fmt, args);
}

// Format text data under the control of fmt (an sprintf-like format string)
// and append it to whatever is already in str. More space is allocated
// to str if necessary. This is sort of like a combination of sprintf and
// strcat.
pub fn appendPQExpBuffer(str: *PQExpBufferData, allocator: std.mem.Allocator, comptime fmt: []const u8, args: anytype) void {
    if (PQExpBufferBroken(str)) {
        return; // already broken
    }
    
    // Try to format into the available space
    const avail = str.maxlen - str.len;
    const result = std.fmt.bufPrint(str.data[str.len..str.maxlen], fmt, args) catch |err| {
        if (err == error.NoSpaceLeft) {
            // Need more space - estimate how much
            const needed = std.fmt.count(fmt, args);
            if (!enlargePQExpBuffer(str, needed, allocator)) {
                return;
            }
            // Try again with more space
            const result2 = std.fmt.bufPrint(str.data[str.len..str.maxlen], fmt, args) catch {
                markPQExpBufferBroken(str, allocator);
                return;
            };
            str.len += result2.len;
            return;
        }
        markPQExpBufferBroken(str, allocator);
        return;
    };
    
    str.len += result.len;
}

// Append the given string to a PQExpBuffer, allocating more space
// if necessary.
pub fn appendPQExpBufferStr(str: *PQExpBufferData, data: []const u8, allocator: std.mem.Allocator) void {
    appendBinaryPQExpBuffer(str, data.ptr, data.len, allocator);
}

// Append a single byte to str.
// Like appendPQExpBuffer(str, "%c", ch) but much faster.
pub fn appendPQExpBufferChar(str: *PQExpBufferData, ch: u8, allocator: std.mem.Allocator) void {
    if (PQExpBufferBroken(str)) {
        return; // already broken
    }
    
    // Make sure there is room for the new character plus null terminator
    if (str.len + 1 >= str.maxlen) {
        if (!enlargePQExpBuffer(str, 1, allocator)) {
            return;
        }
    }
    
    // Append the character
    str.data[str.len] = ch;
    str.len += 1;
    str.data[str.len] = 0;
}

// Append arbitrary binary data to a PQExpBuffer, allocating more space
// if necessary.
pub fn appendBinaryPQExpBuffer(str: *PQExpBufferData, data: [*]const u8, datalen: usize, allocator: std.mem.Allocator) void {
    if (PQExpBufferBroken(str)) {
        return; // already broken
    }
    
    // Make sure there is room
    if (!enlargePQExpBuffer(str, datalen, allocator)) {
        return;
    }
    
    // Append the data
    @memcpy(str.data[str.len..str.len + datalen], data[0..datalen]);
    str.len += datalen;
    str.data[str.len] = 0;
}