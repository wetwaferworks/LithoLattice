// jsonrpc.cpp: JSON-RPC 2.0 framing, written clean-room from the specification.
#include "litholattice/mcp/jsonrpc.h"

namespace litholattice::mcp {

std::string rpcErrorMessage(int code) {
  switch (code) {
    case kRpcParseError:
      return "Parse error";
    case kRpcInvalidRequest:
      return "Invalid Request";
    case kRpcMethodNotFound:
      return "Method not found";
    case kRpcInvalidParams:
      return "Invalid params";
    case kRpcInternalError:
      return "Internal error";
    case kRpcUnsupportedProtocolVersion:
      return "Unsupported protocol version";
    default:
      return "Server error";
  }
}

cJsonValue buildSuccessResponse(const cJsonValue& id, const cJsonValue& result) {
  cJsonValue::Object o;
  o.emplace_back("jsonrpc", cJsonValue::string(kJsonRpcVersion));
  o.emplace_back("result", result);
  o.emplace_back("id", id);
  return cJsonValue::object(std::move(o));
}

cJsonValue buildErrorResponse(const cJsonValue& id, int code, std::string_view message,
                             const cJsonValue& data) {
  cJsonValue::Object err;
  err.emplace_back("code", cJsonValue::integer(code));
  err.emplace_back("message", cJsonValue::string(message));
  if (!data.isNull()) {
    err.emplace_back("data", data);
  }
  cJsonValue::Object o;
  o.emplace_back("jsonrpc", cJsonValue::string(kJsonRpcVersion));
  o.emplace_back("error", cJsonValue::object(std::move(err)));
  o.emplace_back("id", id);
  return cJsonValue::object(std::move(o));
}

RpcParseOutcome parseRpcRequest(std::string_view text) {
  RpcParseOutcome out;
  ParseResult pr = parseJson(text);
  if (!pr.ok) {
    out.ok = false;
    out.errorCode = kRpcParseError;
    out.errorMessage = rpcErrorMessage(kRpcParseError);
    out.errorId = cJsonValue::null();
    return out;
  }
  const cJsonValue& v = pr.value;
  // v1 decision (documented in jsonrpc.h): batch arrays are rejected.
  if (!v.isObject()) {
    out.ok = false;
    out.errorCode = kRpcInvalidRequest;
    out.errorMessage = rpcErrorMessage(kRpcInvalidRequest);
    out.errorId = cJsonValue::null();
    return out;
  }
  const cJsonValue* version = v.find("jsonrpc");
  if (!version || !version->isString() || version->asString() != kJsonRpcVersion) {
    out.ok = false;
    out.errorCode = kRpcInvalidRequest;
    out.errorMessage = rpcErrorMessage(kRpcInvalidRequest);
    out.errorId = cJsonValue::null();
    return out;
  }
  const cJsonValue* method = v.find("method");
  if (!method || !method->isString()) {
    out.ok = false;
    out.errorCode = kRpcInvalidRequest;
    out.errorMessage = rpcErrorMessage(kRpcInvalidRequest);
    out.errorId = cJsonValue::null();
    return out;
  }
  const cJsonValue* id = v.find("id");
  const cJsonValue* params = v.find("params");
  if (params && !params->isNull() && !params->isObject() && !params->isArray()) {
    // Per spec, params MUST be Array or Object when present; a wrong type is
    // the handler's call, surfaced as Invalid params.
    out.ok = true;
    out.request.isNotification = (id == nullptr);
    out.request.id = id ? *id : cJsonValue::null();
    out.request.method = method->asString();
    out.request.params = *params;
    return out;
  }
  out.ok = true;
  out.request.isNotification = (id == nullptr);
  out.request.id = id ? *id : cJsonValue::null();
  out.request.method = method->asString();
  out.request.params = params ? *params : cJsonValue::null();
  return out;
}

void cRpcRouter::registerHandler(std::string method, RpcHandler handler) {
  for (auto& [name, h] : handlers_) {
    if (name == method) {
      h = std::move(handler);
      return;
    }
  }
  handlers_.emplace_back(std::move(method), std::move(handler));
}

bool cRpcRouter::hasHandler(std::string_view method) const {
  for (const auto& [name, h] : handlers_) {
    if (name == method) return true;
  }
  return false;
}

std::optional<cJsonValue> cRpcRouter::dispatch(const RpcRequest& request) {
  if (request.isNotification) {
    // Notifications still invoke the handler (fire and forget) but never
    // produce a response. Handler errors on a notification are swallowed:
    // there is nowhere to send them.
    for (const auto& [name, h] : handlers_) {
      if (name == request.method) {
        try {
          h(request.params);
        } catch (...) {
        }
        return std::nullopt;
      }
    }
    return std::nullopt;
  }
  for (const auto& [name, h] : handlers_) {
    if (name == request.method) {
      try {
        return buildSuccessResponse(request.id, h(request.params));
      } catch (const cRpcError&) {
        throw;
      } catch (...) {
        throw cRpcError(kRpcInternalError, rpcErrorMessage(kRpcInternalError));
      }
    }
  }
  throw cRpcError(kRpcMethodNotFound, rpcErrorMessage(kRpcMethodNotFound),
                 cJsonValue::string(request.method));
}

std::optional<cJsonValue> handleParsedRequest(const RpcRequest& req, cRpcRouter& router) {
  // Invalid params type (present but neither Array nor Object): -32602.
  if (!req.params.isNull() && !req.params.isObject() && !req.params.isArray()) {
    if (req.isNotification) return std::nullopt;  // notifications: no response, ever
    return buildErrorResponse(req.id, kRpcInvalidParams, rpcErrorMessage(kRpcInvalidParams));
  }
  try {
    return router.dispatch(req);  // nullopt for notifications
  } catch (const cRpcError& e) {
    return buildErrorResponse(req.id, e.code(), e.message(), e.data());
  }
}

std::optional<std::string> handleRpcMessage(std::string_view text, cRpcRouter& router) {
  RpcParseOutcome outcome = parseRpcRequest(text);
  if (!outcome.ok) {
    return buildErrorResponse(outcome.errorId, outcome.errorCode, outcome.errorMessage)
        .serialize();
  }
  std::optional<cJsonValue> resp = handleParsedRequest(outcome.request, router);
  if (!resp) return std::nullopt;  // notification
  return resp->serialize();
}

}  // namespace litholattice::mcp
