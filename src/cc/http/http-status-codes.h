/*
 * http-status-codes.h
 *
 *  Created on: Jul 29, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __http_status_codes_h__
#define __http_status_codes_h__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * http://www.iana.org/assignments/http-status-codes/http-status-codes.xhtml
 */
typedef
enum HTTP_STATUS_CODE {

  // 1xx Informational
  HTTP_100_Continue = 100,
  HTTP_101_SwitchingProtocols = 101,
  HTTP_102_Processing = 102,
  // 103-199  Unassigned

  // 2xx Success
  HTTP_200_OK = 200,
  HTTP_201_Created = 201,
  HTTP_202_Accepted = 202,
  HTTP_203_NonAuthoritativeInformation = 203,
  HTTP_204_NoContent = 204,
  HTTP_205_ResetContent = 205,
  HTTP_206_PartialContent = 206,
  HTTP_207_MultiStatus = 207,
  HTTP_208_AlreadyReported = 208,
  // 209-225  Unassigned
  HTTP_226_IM_Used = 226,
  // 227-299  Unassigned

  // 3xx Redirection
  HTTP_300_MultipleChoices = 300,
  HTTP_301_MovedPermanently = 301,
  HTTP_302_Found = 302,
  HTTP_303_SeeOther = 303,
  HTTP_304_NotModified = 304,
  HTTP_305_UseProxy = 305,
  HTTP_306_Unused = 306,
  HTTP_307_TemporaryRedirect = 307,
  HTTP_308_PermanentRedirect = 308,
  // 309-399_Unassigned

  // 4xx Client Error
  HTTP_400_BadRequest = 400,
  HTTP_401_Unauthorized = 401,
  HTTP_402_PaymentRequired = 402,
  HTTP_403_Forbidden = 403,
  HTTP_404_NotFound = 404,
  HTTP_405_MethodNotAllowed = 405,
  HTTP_406_NotAcceptable = 406,
  HTTP_407_ProxyAuthenticationRequired = 407,
  HTTP_408_RequestTimeout = 408,
  HTTP_409_Conflict = 409,
  HTTP_410_Gone = 410,
  HTTP_411_LengthRequired = 411,
  HTTP_412_PreconditionFailed = 412,
  HTTP_413_PayloadTooLarge = 413,
  HTTP_414_UriTooLong = 414,
  HTTP_415_UnsupportedMediaType = 415,
  HTTP_416_RangeNotSatisfiable = 416,
  HTTP_417_ExpectationFailed = 417,
  // 418-420   Unassigned
  HTTP_421_MisdirectedRequest = 421,
  HTTP_422_UnprocessableEntity = 422,
  HTTP_423_Locked = 423,
  HTTP_424_FailedDependency = 424,
  // 425   Unassigned
  HTTP_426_UpgradeRequired = 426,
  HTTP_427_Unassigned = 427,
  HTTP_428_PreconditionRequired = 428,
  HTTP_429_TooManyRequests = 429,
  HTTP_430_Unassigned = 430,
  HTTP_431_RequestHeaderFieldsTooLarge = 431,
  // 432-450  Unassigned
  HTTP_451_UnavailableForLegalReasons = 451,
  // 452-499   Unassigned

  // 5xx Server Error
  HTTP_500_InternalServerError = 500,
  HTTP_501_NotImplemented = 501,
  HTTP_502_BadGateway = 502,
  HTTP_503_ServiceUnavailable = 503,
  HTTP_504_GatewayTimeout = 504,
  HTTP_505_HttpVersionNotSupported = 505,
  HTTP_506_VariantAlsoNegotiates = 506,
  HTTP_507_InsufficientStorage = 507,
  HTTP_508_LoopDetected = 508,
  // 509   Unassigned
  HTTP_510_NotExtended = 510,
  HTTP_511_NetworkAuthenticationRequired = 511,
  // 512-599 Unassigned

} HTTP_STATUS_CODE;


const char * http_status_message(int http_status_code);


#ifdef __cplusplus
}
#endif

#endif /* __http_status_codes_h__ */
