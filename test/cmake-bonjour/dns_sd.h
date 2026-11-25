/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dns_sd.h
 * @brief Minimal controlled declarations for the private DNSService shim.
 */
#ifndef DMN_TEST_DNS_SD_H_
#define DMN_TEST_DNS_SD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DNSServiceRef_t *DNSServiceRef;
typedef int32_t DNSServiceErrorType;
typedef uint32_t DNSServiceFlags;
typedef uint32_t DNSServiceProtocol;
struct sockaddr;

#define DNSSD_API
#define kDNSServiceFlagsMoreComing 0x1
#define kDNSServiceFlagsAdd 0x2
#define kDNSServiceFlagsNoAutoRename 0x8
#define kDNSServiceType_TXT 16
#define kDNSServiceType_SRV 33
#define kDNSServiceClass_IN 1
#define kDNSServiceProtocol_IPv4 0x1
#define kDNSServiceProtocol_IPv6 0x2
#define kDNSServiceMaxDomainName 1009
#define kDNSServiceErr_NoMemory -65539
#define kDNSServiceErr_Unsupported -65544
#define kDNSServiceErr_NameConflict -65548
#define kDNSServiceErr_Firewall -65550
#define kDNSServiceErr_Refused -65553
#define kDNSServiceErr_NoAuth -65555
#define kDNSServiceErr_Transient -65562
#define kDNSServiceErr_ServiceNotRunning -65563
#define kDNSServiceErr_Timeout -65568
#define kDNSServiceErr_DefunctConnection -65569
#define kDNSServiceErr_PolicyDenied -65570
#define kDNSServiceErr_NotPermitted -65571

typedef void(DNSSD_API *DNSServiceBrowseReply)(
    DNSServiceRef ref, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType error, const char *serviceName, const char *regtype,
    const char *replyDomain, void *context);

typedef void(DNSSD_API *DNSServiceQueryRecordReply)(
    DNSServiceRef ref, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType error, const char *fullname, uint16_t rrtype,
    uint16_t rrclass, uint16_t rdlen, const void *rdata, uint32_t ttl,
    void *context);

typedef void(DNSSD_API *DNSServiceGetAddrInfoReply)(
    DNSServiceRef ref, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType error, const char *hostname,
    const struct sockaddr *addr, uint32_t ttl, void *context);

typedef void(DNSSD_API *DNSServiceRegisterReply)(
    DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType error,
    const char *name, const char *regtype, const char *domain, void *context);

DNSServiceErrorType DNSServiceBrowse(DNSServiceRef *ref, DNSServiceFlags flags,
                                     uint32_t interfaceIndex,
                                     const char *regtype, const char *domain,
                                     DNSServiceBrowseReply callback,
                                     void *context);
DNSServiceErrorType
DNSServiceQueryRecord(DNSServiceRef *ref, DNSServiceFlags flags,
                      uint32_t interfaceIndex, const char *fullname,
                      uint16_t rrtype, uint16_t rrclass,
                      DNSServiceQueryRecordReply callback, void *context);
DNSServiceErrorType
DNSServiceGetAddrInfo(DNSServiceRef *ref, DNSServiceFlags flags,
                      uint32_t interfaceIndex, DNSServiceProtocol protocol,
                      const char *hostname, DNSServiceGetAddrInfoReply callback,
                      void *context);
DNSServiceErrorType DNSServiceRegister(
    DNSServiceRef *ref, DNSServiceFlags flags, uint32_t interfaceIndex,
    const char *name, const char *regtype, const char *domain, const char *host,
    uint16_t port, uint16_t txtLen, const void *txtRecord,
    DNSServiceRegisterReply callback, void *context);
DNSServiceErrorType DNSServiceConstructFullName(char *fullName,
                                                const char *service,
                                                const char *regtype,
                                                const char *domain);
int DNSServiceRefSockFD(DNSServiceRef ref);
DNSServiceErrorType DNSServiceProcessResult(DNSServiceRef ref);
void DNSServiceRefDeallocate(DNSServiceRef ref);

#ifdef __cplusplus
}
#endif

#endif
