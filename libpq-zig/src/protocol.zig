// Definitions of the request/response codes for the wire protocol.

// These are the request codes sent by the frontend.
pub const PqMsg_Bind: u8 = 'B';
pub const PqMsg_Close: u8 = 'C';
pub const PqMsg_Describe: u8 = 'D';
pub const PqMsg_Execute: u8 = 'E';
pub const PqMsg_FunctionCall: u8 = 'F';
pub const PqMsg_Flush: u8 = 'H';
pub const PqMsg_Parse: u8 = 'P';
pub const PqMsg_Query: u8 = 'Q';
pub const PqMsg_Sync: u8 = 'S';
pub const PqMsg_Terminate: u8 = 'X';
pub const PqMsg_CopyFail: u8 = 'f';
pub const PqMsg_GSSResponse: u8 = 'p';
pub const PqMsg_PasswordMessage: u8 = 'p';
pub const PqMsg_SASLInitialResponse: u8 = 'p';
pub const PqMsg_SASLResponse: u8 = 'p';

// These are the response codes sent by the backend.
pub const PqMsg_ParseComplete: u8 = '1';
pub const PqMsg_BindComplete: u8 = '2';
pub const PqMsg_CloseComplete: u8 = '3';
pub const PqMsg_NotificationResponse: u8 = 'A';
pub const PqMsg_CommandComplete: u8 = 'C';
pub const PqMsg_DataRow: u8 = 'D';
pub const PqMsg_ErrorResponse: u8 = 'E';
pub const PqMsg_CopyInResponse: u8 = 'G';
pub const PqMsg_CopyOutResponse: u8 = 'H';
pub const PqMsg_EmptyQueryResponse: u8 = 'I';
pub const PqMsg_BackendKeyData: u8 = 'K';
pub const PqMsg_NoticeResponse: u8 = 'N';
pub const PqMsg_AuthenticationRequest: u8 = 'R';
pub const PqMsg_ParameterStatus: u8 = 'S';
pub const PqMsg_RowDescription: u8 = 'T';
pub const PqMsg_FunctionCallResponse: u8 = 'V';
pub const PqMsg_CopyBothResponse: u8 = 'W';
pub const PqMsg_ReadyForQuery: u8 = 'Z';
pub const PqMsg_NoData: u8 = 'n';
pub const PqMsg_PortalSuspended: u8 = 's';
pub const PqMsg_ParameterDescription: u8 = 't';
pub const PqMsg_NegotiateProtocolVersion: u8 = 'v';

// These are the codes sent by both the frontend and backend.
pub const PqMsg_CopyDone: u8 = 'c';
pub const PqMsg_CopyData: u8 = 'd';

// These are the codes sent by parallel workers to leader processes.
pub const PqMsg_Progress: u8 = 'P';

// These are the authentication request codes sent by the backend.
pub const AUTH_REQ_OK: u32 = 0; // User is authenticated
pub const AUTH_REQ_KRB4: u32 = 1; // Kerberos V4. Not supported any more.
pub const AUTH_REQ_KRB5: u32 = 2; // Kerberos V5. Not supported any more.
pub const AUTH_REQ_PASSWORD: u32 = 3; // Password
pub const AUTH_REQ_CRYPT: u32 = 4; // crypt password. Not supported any more.
pub const AUTH_REQ_MD5: u32 = 5; // md5 password
// 6 is available. It was used for SCM creds, not supported any more.
pub const AUTH_REQ_GSS: u32 = 7; // GSSAPI without wrap()
pub const AUTH_REQ_GSS_CONT: u32 = 8; // Continue GSS exchanges
pub const AUTH_REQ_SSPI: u32 = 9; // SSPI negotiate without wrap()
pub const AUTH_REQ_SASL: u32 = 10; // Begin SASL authentication
pub const AUTH_REQ_SASL_CONT: u32 = 11; // Continue SASL authentication
pub const AUTH_REQ_SASL_FIN: u32 = 12; // Final SASL message
pub const AUTH_REQ_MAX: u32 = AUTH_REQ_SASL_FIN; // maximum AUTH_REQ_* value