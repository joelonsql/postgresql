const std = @import("std");

// PostgreSQL encoding identifiers
//
// WARNING: If you add some encoding don't forget to update
// the pg_enc2name_tbl[] array, the pg_enc2gettext_tbl[] array
// and the pg_wchar_table[] array.
//
// PG_SQL_ASCII is default encoding and must be = 0.
pub const pg_enc = enum(c_int) {
    PG_SQL_ASCII = 0, // SQL/ASCII
    PG_EUC_JP, // EUC for Japanese
    PG_EUC_CN, // EUC for Chinese
    PG_EUC_KR, // EUC for Korean
    PG_EUC_TW, // EUC for Taiwan
    PG_EUC_JIS_2004, // EUC-JIS-2004
    PG_UTF8, // Unicode UTF8
    PG_MULE_INTERNAL, // Mule internal code
    PG_LATIN1, // ISO-8859-1 Latin 1
    PG_LATIN2, // ISO-8859-2 Latin 2
    PG_LATIN3, // ISO-8859-3 Latin 3
    PG_LATIN4, // ISO-8859-4 Latin 4
    PG_LATIN5, // ISO-8859-9 Latin 5
    PG_LATIN6, // ISO-8859-10 Latin6
    PG_LATIN7, // ISO-8859-13 Latin7
    PG_LATIN8, // ISO-8859-14 Latin8
    PG_LATIN9, // ISO-8859-15 Latin9
    PG_LATIN10, // ISO-8859-16 Latin10
    PG_WIN1256, // windows-1256
    PG_WIN1258, // Windows-1258
    PG_WIN866, // (MS-DOS CP866)
    PG_WIN874, // windows-874
    PG_KOI8R, // KOI8-R
    PG_WIN1251, // windows-1251
    PG_WIN1252, // windows-1252
    PG_ISO_8859_5, // ISO-8859-5
    PG_ISO_8859_6, // ISO-8859-6
    PG_ISO_8859_7, // ISO-8859-7
    PG_ISO_8859_8, // ISO-8859-8
    PG_WIN1250, // windows-1250
    PG_WIN1253, // windows-1253
    PG_WIN1254, // windows-1254
    PG_WIN1255, // windows-1255
    PG_WIN1257, // windows-1257
    PG_KOI8U, // KOI8-U
    // PG_ENCODING_BE_LAST points to the above entry

    // followings are for client encoding only
    PG_SJIS, // Shift JIS (Windows-932)
    PG_BIG5, // Big5 (Windows-950)
    PG_GBK, // GBK (Windows-936)
    PG_UHC, // UHC (Windows-949)
    PG_GB18030, // GB18030
    PG_JOHAB, // EUC for Korean JOHAB
    PG_SHIFT_JIS_2004, // Shift-JIS-2004
    _PG_LAST_ENCODING_, // mark only
};

pub const PG_ENCODING_BE_LAST = pg_enc.PG_KOI8U;

// Please use these tests before access to pg_enc2name_tbl[]
// or to other places...
pub fn PG_VALID_BE_ENCODING(enc: c_int) bool {
    return enc >= 0 and enc <= @intFromEnum(PG_ENCODING_BE_LAST);
}

pub fn PG_ENCODING_IS_CLIENT_ONLY(enc: c_int) bool {
    return enc > @intFromEnum(PG_ENCODING_BE_LAST) and enc < @intFromEnum(pg_enc._PG_LAST_ENCODING_);
}

pub fn PG_VALID_ENCODING(enc: c_int) bool {
    return enc >= 0 and enc < @intFromEnum(pg_enc._PG_LAST_ENCODING_);
}

// On FE are possible all encodings
pub fn PG_VALID_FE_ENCODING(enc: c_int) bool {
    return PG_VALID_ENCODING(enc);
}

// The pg_wchar type
pub const pg_wchar = u32;

// Maximum byte length of multibyte characters in any backend encoding
pub const MAX_MULTIBYTE_CHAR_LEN = 4;

// When converting strings between different encodings, we assume that space
// for converted result is 4-to-1 growth in the worst case.
pub const MAX_CONVERSION_GROWTH = 4;

// Maximum byte length of a string that's required in any encoding to convert
// at least one character to any other encoding.
pub const MAX_CONVERSION_INPUT_LENGTH = 16;

// Maximum byte length of the string equivalent to any one Unicode code point,
// in any backend encoding.
pub const MAX_UNICODE_EQUIVALENT_STRING = 16;

// Encoding name/ID mapping structures
pub const pg_enc_map = struct {
    name: []const u8,
    encoding: pg_enc,
};

// This is a simplified version - the actual implementation would need the full table
pub const encoding_map = [_]pg_enc_map{
    .{ .name = "SQL_ASCII", .encoding = .PG_SQL_ASCII },
    .{ .name = "EUC_JP", .encoding = .PG_EUC_JP },
    .{ .name = "EUC_CN", .encoding = .PG_EUC_CN },
    .{ .name = "EUC_KR", .encoding = .PG_EUC_KR },
    .{ .name = "EUC_TW", .encoding = .PG_EUC_TW },
    .{ .name = "EUC_JIS_2004", .encoding = .PG_EUC_JIS_2004 },
    .{ .name = "UTF8", .encoding = .PG_UTF8 },
    .{ .name = "MULE_INTERNAL", .encoding = .PG_MULE_INTERNAL },
    .{ .name = "LATIN1", .encoding = .PG_LATIN1 },
    .{ .name = "LATIN2", .encoding = .PG_LATIN2 },
    .{ .name = "LATIN3", .encoding = .PG_LATIN3 },
    .{ .name = "LATIN4", .encoding = .PG_LATIN4 },
    .{ .name = "LATIN5", .encoding = .PG_LATIN5 },
    .{ .name = "LATIN6", .encoding = .PG_LATIN6 },
    .{ .name = "LATIN7", .encoding = .PG_LATIN7 },
    .{ .name = "LATIN8", .encoding = .PG_LATIN8 },
    .{ .name = "LATIN9", .encoding = .PG_LATIN9 },
    .{ .name = "LATIN10", .encoding = .PG_LATIN10 },
    .{ .name = "WIN1256", .encoding = .PG_WIN1256 },
    .{ .name = "WIN1258", .encoding = .PG_WIN1258 },
    .{ .name = "WIN866", .encoding = .PG_WIN866 },
    .{ .name = "WIN874", .encoding = .PG_WIN874 },
    .{ .name = "KOI8R", .encoding = .PG_KOI8R },
    .{ .name = "WIN1251", .encoding = .PG_WIN1251 },
    .{ .name = "WIN1252", .encoding = .PG_WIN1252 },
    .{ .name = "ISO_8859_5", .encoding = .PG_ISO_8859_5 },
    .{ .name = "ISO_8859_6", .encoding = .PG_ISO_8859_6 },
    .{ .name = "ISO_8859_7", .encoding = .PG_ISO_8859_7 },
    .{ .name = "ISO_8859_8", .encoding = .PG_ISO_8859_8 },
    .{ .name = "WIN1250", .encoding = .PG_WIN1250 },
    .{ .name = "WIN1253", .encoding = .PG_WIN1253 },
    .{ .name = "WIN1254", .encoding = .PG_WIN1254 },
    .{ .name = "WIN1255", .encoding = .PG_WIN1255 },
    .{ .name = "WIN1257", .encoding = .PG_WIN1257 },
    .{ .name = "KOI8U", .encoding = .PG_KOI8U },
    .{ .name = "SJIS", .encoding = .PG_SJIS },
    .{ .name = "BIG5", .encoding = .PG_BIG5 },
    .{ .name = "GBK", .encoding = .PG_GBK },
    .{ .name = "UHC", .encoding = .PG_UHC },
    .{ .name = "GB18030", .encoding = .PG_GB18030 },
    .{ .name = "JOHAB", .encoding = .PG_JOHAB },
    .{ .name = "SHIFT_JIS_2004", .encoding = .PG_SHIFT_JIS_2004 },
};

pub fn pg_char_to_encoding(name: []const u8) c_int {
    for (encoding_map) |entry| {
        if (std.ascii.eqlIgnoreCase(name, entry.name)) {
            return @intFromEnum(entry.encoding);
        }
    }
    return -1;
}

pub fn pg_encoding_to_char(encoding: c_int) ?[]const u8 {
    if (!PG_VALID_ENCODING(encoding)) {
        return null;
    }
    
    for (encoding_map) |entry| {
        if (@intFromEnum(entry.encoding) == encoding) {
            return entry.name;
        }
    }
    return null;
}

pub fn pg_valid_server_encoding_id(encoding: c_int) bool {
    return PG_VALID_BE_ENCODING(encoding);
}