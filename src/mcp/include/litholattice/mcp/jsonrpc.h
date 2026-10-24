// jsonrpc.h: JSON-RPC 2.0 framing for the LithoLattice MCP stack (Phase 0).
//
// Written clean-room from the JSON-RPC 2.0 specification.
//
// Locked design decisions:
//   - Single requests only in v1. A top-level JSON array (batch) is rejected
//     with -32600 Invalid Request. Rationale: both planned transports are
//     single-message oriented (newline-delimited stdio frames, Streamable
//     HTTP single POST bodies where batching is an edge case), and engine
//     fail-fast policy favors rejecting unneeded surface now; batching can
//     be added later without breaking single-request behavior.
//   - Notification = request object with NO "id" member. A null "id" is a
//     real request (id null round-trips in the response).
//   - Errors are reported two ways: protocol-level failures become JSON-RPC
//     error responses; a handler signals a parameter/action problem by
//     throwing cRpcError (e.g. -32602), and unexpected exceptions become
//     -32603 Internal error. Model-actionable tool failures in later phases
//     will use result payloads (isError), never protocol errors.
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>

#include "litholattice/mcp/json.h"

namespace litholattice::mcp {

/// JSON-RPC 2.0 version marker, required on every request and response.
inline constexpr const char* kJsonRpcVersion = "2.0";

/// Standard JSON-RPC 2.0 error codes.
inline constexpr int kRpcParseError = -32700;      // invalid JSON
inline constexpr int kRpcInvalidRequest = -32600;   // not a valid request object
inline constexpr int kRpcMethodNotFound = -32601;  // unknown method
inline constexpr int kRpcInvalidParams = -32602;   // bad params
inline constexpr int kRpcInternalError = -32603;   // unexpected handler failure
/// MCP-reserved server error: client asked for an unsupported protocol
/// version. Defined now (used by the Phase 1/3 handshake work).
inline constexpr int kRpcUnsupportedProtocolVersion = -32022;
/// MCP-reserved server error (2026-07-28, SEP-2243): a Streamable HTTP
/// header disagreed with the JSON-RPC body it mirrors (MCP-Protocol-Version,
/// Mcp-Method, Mcp-Name), or a required one was absent.
inline constexpr int kRpcHeaderMismatch = -32020;

/// Default messages for the standard codes, per the JSON-RPC 2.0 spec.
std::string rpcErrorMessage(int code);

/// Exception a method handler throws to produce a JSON-RPC error response.
/// Carries the numeric code, message, and optional structured data.
class cRpcError : public std::exception {
 public:
  cRpcError(int code, std::string message, cJsonValue data = cJsonValue::null())
      : code_(code), message_(std::move(message)), data_(std::move(data)) {}
  int code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  const cJsonValue& data() const noexcept { return data_; }
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  int code_;
  std::string message_;
  cJsonValue data_;
};

/// A validated JSON-RPC 2.0 request (or notification).
struct RpcRequest {
  /// True when the request object had no "id" member (no response is sent).
  bool isNotification = false;
  /// The request id: string, integer, or null. Null-value cJsonValue when this
  /// is a notification.
  cJsonValue id;
  std::string method;
  /// Params as given; null when the member was absent.
  cJsonValue params;
};

/// Parse + validate one JSON-RPC request/notification from text. Never throws
/// for protocol problems: on failure returns ok=false with code/message for
/// the error response (id is null unless a valid id could be recovered).
struct RpcParseOutcome {
  bool ok = false;
  RpcRequest request;
  int errorCode = kRpcInvalidRequest;
  std::string errorMessage;
  /// Recovered id for the error response (null when none recoverable).
  cJsonValue errorId;
};
RpcParseOutcome parseRpcRequest(std::string_view text);

/// Builds a JSON-RPC 2.0 response object value (not yet serialized).
cJsonValue buildSuccessResponse(const cJsonValue& id, const cJsonValue& result);
cJsonValue buildErrorResponse(const cJsonValue& id, int code, std::string_view message,
                             const cJsonValue& data = cJsonValue::null());

/// Method handler: receives params (null when absent), returns the result
/// value, or throws cRpcError for a protocol error.
using RpcHandler = std::function<cJsonValue(const cJsonValue& params)>;

/// Routes method names to handlers. Unknown method -> cRpcError(-32601).
class cRpcRouter {
 public:
  /// Registers (or replaces) the handler for a method name.
  void registerHandler(std::string method, RpcHandler handler);
  bool hasHandler(std::string_view method) const;

  /// Dispatches one validated request. Returns nullopt for notifications
  /// (no response is ever sent for them). Throws cRpcError for unknown
  /// methods; a handler's cRpcError propagates; any other exception becomes
  /// cRpcError(-32603). Never throws anything else.
  std::optional<cJsonValue> dispatch(const RpcRequest& request);

 private:
  std::vector<std::pair<std::string, RpcHandler>> handlers_;
};

/// One-shot pipeline: parse text, validate, dispatch through the router, and
/// serialize the response. Returns nullopt when no response is owed
/// (notification) or, per spec, when the request was a notification that
/// failed validation (a notification that fails to parse gets no response).
/// This function never throws for protocol-level problems.
std::optional<std::string> handleRpcMessage(std::string_view text, cRpcRouter& router);

/// Dispatches an already-parsed request: the invalid-params check,
/// notification swallowing, and cRpcError conversion of handleRpcMessage,
/// minus the parse — it returns the response VALUE (success or error
/// envelope), or nullopt for a notification. Callers that already hold the
/// parsed RpcRequest (cMcpServer::handleMessage, dispatchStateless) use this
/// to avoid parsing the body a second time; dispatchStateless additionally
/// merges its envelope into the value BEFORE serializing (no
/// serialize-then-re-parse round trip).
std::optional<cJsonValue> handleParsedRequest(const RpcRequest& req, cRpcRouter& router);

}  // namespace litholattice::mcp
