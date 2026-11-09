#ifndef _BSD_I386__TYPES_H_
#define _BSD_I386__TYPES_H_
typedef unsigned int __darwin_natural_t;
typedef long __darwin_intptr_t;
typedef unsigned long __darwin_size_t;
typedef long __darwin_ssize_t;
typedef int __darwin_pid_t;

typedef __darwin_natural_t __darwin_mach_port_name_t; typedef __darwin_mach_port_name_t __darwin_mach_port_t;

typedef unsigned char __darwin_uuid_t[16];
#endif
