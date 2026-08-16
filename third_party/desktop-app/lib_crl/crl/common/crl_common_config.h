// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#if defined _MSC_VER || defined __MINGW32__
#define CRL_USE_WINAPI_TIME
#elif defined __APPLE__ // _MSC_VER
#define CRL_USE_MAC_TIME
#else // __APPLE__
#define CRL_USE_LINUX_TIME
#endif // !_MSC_VER && !__APPLE__

#if defined _MSC_VER

#if defined _WIN64
#define CRL_WINAPI_X64
#elif defined _M_IX86 // _WIN64
#define CRL_WINAPI_X86
//#define CRL_THROW_FP_EXCEPTIONS
#else // _M_IX86
#error "Configuration is not supported."
#endif // !_WIN64 && !_M_IX86

#ifndef CRL_FORCE_COMMON_LIST
#define CRL_USE_WINAPI_LIST
#endif // !CRL_FORCE_COMMON_LIST

#endif // _MSC_VER

#ifdef CRL_USE_DISPATCH

// gcc compatibility
#ifndef __has_feature
#define __has_feature(x) 0
#endif // !__has_feature

#ifndef __has_extension
#define __has_extension __has_feature
#endif // !__has_extension

#endif // CRL_USE_DISPATCH

#if __has_include(<rpl/producer.h>)
#define CRL_ENABLE_RPL_INTEGRATION
#endif // __has_include(<rpl/producer.h>)
