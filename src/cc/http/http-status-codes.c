/*
 * http-status-codes.c
 *
 *  Created on: Jul 29, 2case HTTP_016: return "*      Author: amyznikov";
 */

#include "http-status-codes.h"

const char * http_status_message(int http_status_code)
{
  switch ( http_status_code ) {

  case HTTP_100_Continue: return "Continue";
  case HTTP_101_SwitchingProtocols: return "Switching Protocols";
  case HTTP_102_Processing: return "Processing";

  case HTTP_200_OK: return "OK";
  case HTTP_201_Created: return "Created";
  case HTTP_202_Accepted: return "Accepted";
  case HTTP_203_NonAuthoritativeInformation: return "Non-Authoritative Information";
  case HTTP_204_NoContent: return "No Content";
  case HTTP_205_ResetContent: return "Reset Content";
  case HTTP_206_PartialContent: return "Partial Content";
  case HTTP_207_MultiStatus: return "Multi-Status";
  case HTTP_208_AlreadyReported: return "Already Reported";

  case HTTP_226_IM_Used: return "IM Used";

  case HTTP_300_MultipleChoices: return "Multiple Choices";
  case HTTP_301_MovedPermanently: return "Moved Permanently";
  case HTTP_302_Found: return "Found";
  case HTTP_303_SeeOther: return "See Other";
  case HTTP_304_NotModified: return "Not Modified";
  case HTTP_305_UseProxy: return "Use Proxy";
  case HTTP_306_Unused: return "(Unused)";
  case HTTP_307_TemporaryRedirect: return "Temporary Redirect";
  case HTTP_308_PermanentRedirect: return "Permanent Redirect";

  case HTTP_400_BadRequest: return "Bad Request";
  case HTTP_401_Unauthorized: return "Unauthorized";
  case HTTP_402_PaymentRequired: return "Payment Required";
  case HTTP_403_Forbidden: return "Forbidden";
  case HTTP_404_NotFound: return "Not Found";
  case HTTP_405_MethodNotAllowed: return "Method Not Allowed";
  case HTTP_406_NotAcceptable: return "Not Acceptable";
  case HTTP_407_ProxyAuthenticationRequired: return "Proxy Authentication Required";
  case HTTP_408_RequestTimeout: return "Request Timeout";
  case HTTP_409_Conflict: return "Conflict";
  case HTTP_410_Gone: return "Gone";
  case HTTP_411_LengthRequired: return "Length Required";
  case HTTP_412_PreconditionFailed: return "Precondition Failed";
  case HTTP_413_PayloadTooLarge: return "Payload Too Large";
  case HTTP_414_UriTooLong: return "URI Too Long";
  case HTTP_415_UnsupportedMediaType: return "Unsupported Media Type";
  case HTTP_416_RangeNotSatisfiable: return "Range Not Satisfiable";
  case HTTP_417_ExpectationFailed: return "Expectation Failed";

  case HTTP_421_MisdirectedRequest: return "Misdirected Request";
  case HTTP_422_UnprocessableEntity: return "Unprocessable Entity";
  case HTTP_423_Locked: return "Locked";
  case HTTP_424_FailedDependency: return "Failed Dependency";

  case HTTP_426_UpgradeRequired: return "Upgrade Required";
  case HTTP_427_Unassigned: return "Unassigned";
  case HTTP_428_PreconditionRequired: return "Precondition Required";
  case HTTP_429_TooManyRequests: return "Too Many Requests";
  case HTTP_430_Unassigned: return "Unassigned";
  case HTTP_431_RequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";

  case HTTP_451_UnavailableForLegalReasons: return "Unavailable For Legal Reasons";

  case HTTP_500_InternalServerError: return "Internal Server Error";
  case HTTP_501_NotImplemented: return "Not Implemented";
  case HTTP_502_BadGateway: return "Bad Gateway";
  case HTTP_503_ServiceUnavailable: return "Service Unavailable";
  case HTTP_504_GatewayTimeout: return "Gateway Timeout";
  case HTTP_505_HttpVersionNotSupported: return "HTTP Version Not Supported";
  case HTTP_506_VariantAlsoNegotiates: return "Variant Also Negotiates";
  case HTTP_507_InsufficientStorage: return "Insufficient Storage";
  case HTTP_508_LoopDetected: return "Loop Detected";

  case HTTP_510_NotExtended: return "Not Extended";
  case HTTP_511_NetworkAuthenticationRequired: return "Network Authentication Required";
  }

  return "Unknown Error";
}
